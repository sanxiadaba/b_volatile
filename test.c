/*
 * Store Buffering litmus test —— C11 原子操作对照实验
 *
 *   初始状态：a = 0, b = 0
 *   线程 1                线程 2
 *   a = 1                 b = 1
 *   x = b                 y = a
 *
 * 顺序一致（SC）下 (x, y) = (0, 0) 不可能出现：它要求
 *   w(a) <po r(b), w(b) <po r(a)    （程序顺序）
 * 而 0/0 又要求 r(b) < w(b), r(a) < w(a)，四条边构成环。
 * relaxed/plain 语义下没有这个约束，(0, 0) 可以出现。
 *
 * 本程序测量的是"分布"，不是"允许性"：
 *   - 允许性来自内存模型（relaxed 允许 0/0，seq_cst 禁止）；
 *   - 出现率来自硬件 store buffer、指令间距、线程时序。
 * 所以两组之间的计数**不能**直接横比，详见 README。
 *
 * 构建（Linux/glibc 与 Windows/MinGW 均可）：
 *   gcc test.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -o test
 *   Windows 建议追加 -static，否则产物依赖 libwinpthread-1.dll。
 */

/*
 * -std=c11 在 glibc 上会定义 __STRICT_ANSI__，从而隐藏 pthread_barrier_t、
 * sched_setaffinity 等 POSIX/GNU 声明（它们在 __USE_XOPEN2K/__USE_GNU 之后）。
 * 在源码里显式打开特性宏，构建命令就可以继续用严格的 -std=c11。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#define DEFAULT_ITERATIONS 100000U
#define DEFAULT_REPEAT 3U
#define CACHE_LINE_BYTES 64U

/*
 * 默认让 a、b 各占一条缓存行。把它们放进同一行（-DFALSE_SHARING=1）会让两个
 * 线程争抢同一条缓存行，实测出现率明显不同 —— 这是实验设计层面的对照，
 * 而不是"内存模型"的差异。
 */
#ifndef FALSE_SHARING
#define FALSE_SHARING 0
#endif

/* -DPIN_THREADS=1 时把两个工作线程绑到不同的 CPU 上，降低调度抖动。 */
#ifndef PIN_THREADS
#define PIN_THREADS 0
#endif

typedef enum {
    MODE_RELAXED = 0,
    MODE_SEQ_CST = 1
} memory_mode_t;

#if FALSE_SHARING
typedef struct {
    atomic_int a;
    atomic_int b;
    atomic_int x;
    atomic_int y;
} shared_state_t;
#else
typedef struct {
    _Alignas(CACHE_LINE_BYTES) atomic_int a;
    _Alignas(CACHE_LINE_BYTES) atomic_int b;
    _Alignas(CACHE_LINE_BYTES) atomic_int x;
    _Alignas(CACHE_LINE_BYTES) atomic_int y;
} shared_state_t;
#endif

typedef struct {
    shared_state_t shared;
    atomic_int ready_workers;
    atomic_int start_workers;
    size_t iterations;
    memory_mode_t mode;
    pthread_barrier_t start_barrier;
    pthread_barrier_t finish_barrier;
} experiment_t;

typedef struct {
    experiment_t *experiment;
    int index; /* 1 或 2，决定该工作线程走哪条关键路径 */
} worker_t;

typedef struct {
    uint64_t zero_zero;
    uint64_t zero_one;
    uint64_t one_zero;
    uint64_t one_one;
} outcome_counts_t;

typedef struct {
    memory_mode_t mode;
    size_t iterations;
    size_t repeat;
    bool csv;
    bool all_modes;
} options_t;

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static void wait_at_barrier(pthread_barrier_t *barrier)
{
    int result = pthread_barrier_wait(barrier);

    if (result != 0 && result != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "pthread_barrier_wait failed: %d\n", result);
        abort();
    }
}

static unsigned int logical_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO system_info;

    GetSystemInfo(&system_info);
    return (unsigned int) system_info.dwNumberOfProcessors;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);

    return count > 0 ? (unsigned int) count : 0U;
#endif
}

static void cpu_brand(char *buffer, size_t size)
{
#if defined(__i386__) || defined(__x86_64__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    char text[49];
    size_t index;
    const char *start;
    size_t length;

    if (__get_cpuid_max(0x80000000U, NULL) < 0x80000004U) {
        snprintf(buffer, size, "unknown");
        return;
    }

    for (index = 0; index < 3; ++index) {
        __cpuid(0x80000002U + (unsigned int) index, eax, ebx, ecx, edx);
        memcpy(text + index * 16U + 0U, &eax, 4U);
        memcpy(text + index * 16U + 4U, &ebx, 4U);
        memcpy(text + index * 16U + 8U, &ecx, 4U);
        memcpy(text + index * 16U + 12U, &edx, 4U);
    }
    text[48] = '\0';

    start = text;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    length = strlen(start);
    while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\t')) {
        --length;
    }
    snprintf(buffer, size, "%.*s", (int) length, start);
#else
    (void) buffer;
    (void) size;
    snprintf(buffer, size, "unknown");
#endif
}

static const char *compiler_name(void)
{
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
    return "msvc";
#else
    return "unknown compiler";
#endif
}

static const char *platform_name(void)
{
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown os";
#endif
}

static int pin_current_thread(int index)
{
#if PIN_THREADS
#if defined(_WIN32)
    DWORD_PTR mask = (DWORD_PTR) 1U << (unsigned int) index;

    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(index, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void) index;
    return 0;
#endif
#else
    (void) index;
    return 1;
#endif
}

static void print_environment(const options_t *options, FILE *stream)
{
    char brand[64];

    cpu_brand(brand, sizeof(brand));
    fprintf(stream, "# %s | %s | %u logical CPUs | %u-bit pointers | C%ld\n",
            platform_name(), compiler_name(), logical_cpu_count(),
            (unsigned int) (sizeof(void *) * 8U),
            (long) (__STDC_VERSION__ / 100L % 100L));
    fprintf(stream, "# cpu: %s\n", brand);
    fprintf(stream, "# layout: a/b %s one cache line | pinning %s\n",
            FALSE_SHARING ? "share" : "do not share",
            PIN_THREADS ? "on" : "off");
    fprintf(stream,
            "# iterations per run: %" PRIuMAX " | runs per mode: %" PRIuMAX
            "\n",
            (uintmax_t) options->iterations, (uintmax_t) options->repeat);
}

/* ------------------------------------------------------------------ */
/* 实验主体                                                           */
/* ------------------------------------------------------------------ */

static void rendezvous_workers(experiment_t *experiment)
{
    unsigned int spins = 0;

    atomic_fetch_add_explicit(&experiment->ready_workers, 1,
                              memory_order_seq_cst);
    while (atomic_load_explicit(&experiment->ready_workers,
                                memory_order_seq_cst) != 2) {
        /* 单核或高负载下不要把 CPU 烧在自旋上。 */
        if (++spins == 1000U) {
            sched_yield();
            spins = 0;
        }
    }
}

static bool wait_for_worker_start(experiment_t *experiment)
{
    int state;

    do {
        state = atomic_load_explicit(&experiment->start_workers,
                                     memory_order_acquire);
    } while (state == 0);

    return state > 0;
}

/*
 * 关键读写。两个线程走同一条代码路径，只靠 index 选择地址 —— "两种内存序 +
 * 两个线程"因此只需要一份循环体。
 *
 * 注意：下面两个函数的函数体几乎一样，是**故意**的。内存序必须是编译期常量：
 * 把 memory_order 当成函数参数传进来时，gcc 4.9 -O2 无法常量折叠，会保守地
 * 生成 xchg（隐含 lock，全栅栏）—— 实测 relaxed 组会因此表现得和 seq_cst 完全
 * 一样（(0,0) 恒为 0）。用 `gcc -S` 就能看到这个退化，见 README。
 */
static void critical_section_relaxed(shared_state_t *shared, int index)
{
    int observed;

    if (index == 1) {
        atomic_store_explicit(&shared->a, 1, memory_order_relaxed);
        observed = atomic_load_explicit(&shared->b, memory_order_relaxed);
        atomic_store_explicit(&shared->x, observed, memory_order_relaxed);
    } else {
        atomic_store_explicit(&shared->b, 1, memory_order_relaxed);
        observed = atomic_load_explicit(&shared->a, memory_order_relaxed);
        atomic_store_explicit(&shared->y, observed, memory_order_relaxed);
    }
}

static void critical_section_seq_cst(shared_state_t *shared, int index)
{
    int observed;

    if (index == 1) {
        atomic_store_explicit(&shared->a, 1, memory_order_seq_cst);
        observed = atomic_load_explicit(&shared->b, memory_order_seq_cst);
        atomic_store_explicit(&shared->x, observed, memory_order_relaxed);
    } else {
        atomic_store_explicit(&shared->b, 1, memory_order_seq_cst);
        observed = atomic_load_explicit(&shared->a, memory_order_seq_cst);
        atomic_store_explicit(&shared->y, observed, memory_order_relaxed);
    }
}

static void critical_section(experiment_t *experiment, int index)
{
    if (experiment->mode == MODE_SEQ_CST) {
        critical_section_seq_cst(&experiment->shared, index);
    } else {
        critical_section_relaxed(&experiment->shared, index);
    }
}

static void *worker_main(void *argument)
{
    worker_t *worker = argument;
    experiment_t *experiment = worker->experiment;
    size_t iteration;

    pin_current_thread(worker->index);

    if (!wait_for_worker_start(experiment)) {
        return NULL;
    }

    for (iteration = 0; iteration < experiment->iterations; ++iteration) {
        wait_at_barrier(&experiment->start_barrier);
        rendezvous_workers(experiment);
        critical_section(experiment, worker->index);
        wait_at_barrier(&experiment->finish_barrier);
    }

    return NULL;
}

static void record_outcome(outcome_counts_t *counts, int x, int y)
{
    if (x == 0 && y == 0) {
        ++counts->zero_zero;
    } else if (x == 0 && y == 1) {
        ++counts->zero_one;
    } else if (x == 1 && y == 0) {
        ++counts->one_zero;
    } else if (x == 1 && y == 1) {
        ++counts->one_one;
    } else {
        fprintf(stderr, "unexpected outcome: x=%d, y=%d\n", x, y);
        abort();
    }
}

/*
 * 跑一轮实验：创建两个工作线程，按 iterations 轮做关键读写，返回四种结果的计数。
 * 返回 EXIT_SUCCESS / EXIT_FAILURE。
 */
static int run_experiment(memory_mode_t mode, size_t iterations,
                          outcome_counts_t *counts)
{
    experiment_t experiment;
    worker_t workers[2];
    pthread_t threads[2];
    pthread_t first_thread;
    int created = 0;
    int result;
    int status = EXIT_SUCCESS;
    size_t iteration;

    atomic_init(&experiment.shared.a, 0);
    atomic_init(&experiment.shared.b, 0);
    atomic_init(&experiment.shared.x, 0);
    atomic_init(&experiment.shared.y, 0);
    atomic_init(&experiment.ready_workers, 0);
    atomic_init(&experiment.start_workers, 0);
    experiment.iterations = iterations;
    experiment.mode = mode;

    workers[0].experiment = &experiment;
    workers[0].index = 1;
    workers[1].experiment = &experiment;
    workers[1].index = 2;

    result = pthread_barrier_init(&experiment.start_barrier, NULL, 3);
    if (result != 0) {
        fprintf(stderr, "pthread_barrier_init(start) failed: %d\n", result);
        return EXIT_FAILURE;
    }

    result = pthread_barrier_init(&experiment.finish_barrier, NULL, 3);
    if (result != 0) {
        fprintf(stderr, "pthread_barrier_init(finish) failed: %d\n", result);
        pthread_barrier_destroy(&experiment.start_barrier);
        return EXIT_FAILURE;
    }

    for (created = 0; created < 2; ++created) {
        result = pthread_create(&threads[created], NULL, worker_main,
                                &workers[created]);
        if (result != 0) {
            fprintf(stderr, "pthread_create(worker %d) failed: %d\n",
                    created + 1, result);
            break;
        }
    }

    if (created < 2) {
        /* 让已经建好的线程从启动门退出，然后回收，避免卡在屏障上。 */
        atomic_store_explicit(&experiment.start_workers, -1,
                              memory_order_release);
        if (created > 0) {
            pthread_join(threads[0], NULL);
        }
        pthread_barrier_destroy(&experiment.finish_barrier);
        pthread_barrier_destroy(&experiment.start_barrier);
        return EXIT_FAILURE;
    }

    atomic_store_explicit(&experiment.start_workers, 1, memory_order_release);

    for (iteration = 0; iteration < iterations; ++iteration) {
        atomic_store_explicit(&experiment.shared.a, 0, memory_order_relaxed);
        atomic_store_explicit(&experiment.shared.b, 0, memory_order_relaxed);
        atomic_store_explicit(&experiment.shared.x, 0, memory_order_relaxed);
        atomic_store_explicit(&experiment.shared.y, 0, memory_order_relaxed);
        atomic_store_explicit(&experiment.ready_workers, 0,
                              memory_order_seq_cst);

        wait_at_barrier(&experiment.start_barrier);
        wait_at_barrier(&experiment.finish_barrier);
        record_outcome(counts,
                       atomic_load_explicit(&experiment.shared.x,
                                            memory_order_relaxed),
                       atomic_load_explicit(&experiment.shared.y,
                                            memory_order_relaxed));
    }

    first_thread = threads[0];
    result = pthread_join(first_thread, NULL);
    if (result != 0) {
        fprintf(stderr, "pthread_join(worker 1) failed: %d\n", result);
        status = EXIT_FAILURE;
    }

    result = pthread_join(threads[1], NULL);
    if (result != 0) {
        fprintf(stderr, "pthread_join(worker 2) failed: %d\n", result);
        status = EXIT_FAILURE;
    }

    result = pthread_barrier_destroy(&experiment.finish_barrier);
    if (result != 0) {
        fprintf(stderr, "pthread_barrier_destroy(finish) failed: %d\n", result);
        status = EXIT_FAILURE;
    }

    result = pthread_barrier_destroy(&experiment.start_barrier);
    if (result != 0) {
        fprintf(stderr, "pthread_barrier_destroy(start) failed: %d\n", result);
        status = EXIT_FAILURE;
    }

    return status;
}

/* ------------------------------------------------------------------ */
/* 输出                                                               */
/* ------------------------------------------------------------------ */

static const char *mode_name(memory_mode_t mode)
{
    return mode == MODE_SEQ_CST ? "seq_cst" : "relaxed";
}

static double percentage(uint64_t part, size_t total)
{
    return total == 0 ? 0.0 : 100.0 * (double) part / (double) total;
}

static void print_run_line(memory_mode_t mode, size_t run, size_t runs,
                           size_t iterations, const outcome_counts_t *counts,
                           bool csv)
{
    if (csv) {
        printf("%s,%" PRIuMAX ",%" PRIuMAX ",%" PRIu64 ",%" PRIu64
               ",%" PRIu64 ",%" PRIu64 "\n",
               mode_name(mode), (uintmax_t) run, (uintmax_t) iterations,
               counts->zero_zero, counts->zero_one, counts->one_zero,
               counts->one_one);
        return;
    }

    printf("%-8s run %" PRIuMAX "/%" PRIuMAX "  n=%" PRIuMAX
           "  (0,0)=%" PRIu64 " (%5.1f%%)  (0,1)=%" PRIu64 "  (1,0)=%" PRIu64
           "  (1,1)=%" PRIu64 "\n",
           mode_name(mode), (uintmax_t) run, (uintmax_t) runs,
           (uintmax_t) iterations, counts->zero_zero,
           percentage(counts->zero_zero, iterations), counts->zero_one,
           counts->one_zero, counts->one_one);
}

/* ------------------------------------------------------------------ */
/* 命令行                                                             */
/* ------------------------------------------------------------------ */

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [options] [iterations]\n"
            "\n"
            "Store Buffering litmus test with C11 atomics.\n"
            "\n"
            "options:\n"
            "  -n, --iterations N   iterations per run (default %u)\n"
            "  -r, --repeat R       independent runs per mode (default %u)\n"
            "  -m, --mode MODE      all | relaxed | seq_cst (default all)\n"
            "      --csv            machine readable output on stdout\n"
            "  -h, --help           show this help\n"
            "\n"
            "A bare number is treated as --iterations for backward"
            " compatibility.\n"
            "\n"
            "exit status: 0 ok, 1 usage/系统错误, 2 seq_cst 观察到被禁止的"
            " (0,0)\n",
            program, DEFAULT_ITERATIONS, DEFAULT_REPEAT);
}

static bool parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    const char *cursor;
    uintmax_t parsed;

    if (text == NULL || *text == '\0') {
        return false;
    }

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }

    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || parsed == 0) {
        return false;
    }
    if (parsed > (uintmax_t) SIZE_MAX) {
        return false;
    }

    *value = (size_t) parsed;
    return true;
}

/*
 * 支持 "--opt value" 与 "--opt=value" 两种写法。
 * 返回 EXIT_SUCCESS 或 EXIT_FAILURE（后者表示参数错误，已经打印原因）。
 */
static int parse_options(int argc, char **argv, options_t *options)
{
    int index;

    options->mode = MODE_RELAXED;
    options->iterations = DEFAULT_ITERATIONS;
    options->repeat = DEFAULT_REPEAT;
    options->csv = false;
    options->all_modes = true;

    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value = NULL;
        const char *plain;
        char option[32];
        size_t length;

        if (strncmp(argument, "--", 2) == 0) {
            const char *equals = strchr(argument, '=');

            if (equals != NULL) {
                length = (size_t) (equals - argument);
                if (length >= sizeof(option)) {
                    length = sizeof(option) - 1U;
                }
                memcpy(option, argument, length);
                option[length] = '\0';
                value = equals + 1;
            } else {
                length = strlen(argument);
                if (length >= sizeof(option)) {
                    length = sizeof(option) - 1U;
                }
                memcpy(option, argument, length);
                option[length] = '\0';
            }
            plain = option;
        } else if (argument[0] == '-' && argument[1] != '\0') {
            plain = argument;
        } else {
            /* 裸数字：向后兼容 ./test 100000 */
            if (!parse_size(argument, &options->iterations)) {
                fprintf(stderr, "invalid iteration count: %s\n", argument);
                return EXIT_FAILURE;
            }
            continue;
        }

        if (strcmp(plain, "-h") == 0 || strcmp(plain, "--help") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(plain, "--csv") == 0) {
            options->csv = true;
            continue;
        }
        if (strcmp(plain, "-n") == 0 || strcmp(plain, "--iterations") == 0 ||
            strcmp(plain, "-r") == 0 || strcmp(plain, "--repeat") == 0 ||
            strcmp(plain, "-m") == 0 || strcmp(plain, "--mode") == 0) {
            if (value == NULL) {
                if (index + 1 >= argc) {
                    fprintf(stderr, "%s requires a value\n", plain);
                    return EXIT_FAILURE;
                }
                value = argv[++index];
            }

            if (strcmp(plain, "-n") == 0 || strcmp(plain, "--iterations") == 0) {
                if (!parse_size(value, &options->iterations)) {
                    fprintf(stderr, "invalid iteration count: %s\n", value);
                    return EXIT_FAILURE;
                }
            } else if (strcmp(plain, "-r") == 0 ||
                       strcmp(plain, "--repeat") == 0) {
                if (!parse_size(value, &options->repeat)) {
                    fprintf(stderr, "invalid repeat count: %s\n", value);
                    return EXIT_FAILURE;
                }
            } else if (strcmp(value, "all") == 0) {
                options->all_modes = true;
            } else if (strcmp(value, "relaxed") == 0) {
                options->all_modes = false;
                options->mode = MODE_RELAXED;
            } else if (strcmp(value, "seq_cst") == 0) {
                options->all_modes = false;
                options->mode = MODE_SEQ_CST;
            } else {
                fprintf(stderr, "unknown mode: %s\n", value);
                return EXIT_FAILURE;
            }
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", argument);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    options_t options;
    outcome_counts_t totals[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    memory_mode_t modes[2];
    size_t mode_count;
    size_t run;
    int status = EXIT_SUCCESS;

    if (parse_options(argc, argv, &options) != EXIT_SUCCESS) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (options.all_modes) {
        modes[0] = MODE_RELAXED;
        modes[1] = MODE_SEQ_CST;
        mode_count = 2;
    } else {
        modes[0] = options.mode;
        mode_count = 1;
    }

    print_environment(&options, options.csv ? stderr : stdout);

    if (options.csv) {
        printf("mode,run,iterations,zero_zero,zero_one,one_zero,one_one\n");
    }

    /*
     * 交替顺序：第 0 轮先 relaxed，第 1 轮先 seq_cst……
     * 两组实验如果永远是固定先后，CPU 频率、缓存预热、JIT/调度状态都会
     * 系统性地偏向其中一组。
     */
    for (run = 0; run < options.repeat; ++run) {
        size_t position;

        for (position = 0; position < mode_count; ++position) {
            memory_mode_t mode;
            outcome_counts_t counts = {0, 0, 0, 0};
            int result;

            if (mode_count == 1) {
                mode = modes[0];
            } else {
                mode = (run % 2U == 0U) ? modes[position] : modes[1 - position];
            }

            result = run_experiment(mode, options.iterations, &counts);
            if (result != EXIT_SUCCESS) {
                return EXIT_FAILURE;
            }

            totals[mode == MODE_SEQ_CST ? 1 : 0].zero_zero += counts.zero_zero;
            totals[mode == MODE_SEQ_CST ? 1 : 0].zero_one += counts.zero_one;
            totals[mode == MODE_SEQ_CST ? 1 : 0].one_zero += counts.one_zero;
            totals[mode == MODE_SEQ_CST ? 1 : 0].one_one += counts.one_one;

            print_run_line(mode, run + 1, options.repeat, options.iterations,
                           &counts, options.csv);

            if (mode == MODE_SEQ_CST && counts.zero_zero != 0) {
                fprintf(stderr,
                        "FAIL: seq_cst observed the forbidden (0,0) outcome\n");
                status = 2;
            }
        }
    }

    if (!options.csv) {
        size_t index;

        for (index = 0; index < mode_count; ++index) {
            memory_mode_t mode = modes[index];
            const outcome_counts_t *total =
                &totals[mode == MODE_SEQ_CST ? 1 : 0];

            printf("%-8s total  n=%" PRIuMAX "  (0,0)=%" PRIu64
                   " (%5.1f%%)  (0,1)=%" PRIu64 "  (1,0)=%" PRIu64
                   "  (1,1)=%" PRIu64 "\n",
                   mode_name(mode),
                   (uintmax_t) (options.iterations * options.repeat),
                   total->zero_zero,
                   percentage(total->zero_zero,
                              options.iterations * options.repeat),
                   total->zero_one, total->one_zero, total->one_one);
        }

        if (status != 2) {
            const outcome_counts_t *relaxed = &totals[0];
            const outcome_counts_t *seq_cst = &totals[1];

            if (mode_count == 2 && relaxed->zero_zero == 0) {
                printf("NOTE: relaxed (0,0) is allowed but was not observed "
                       "in these runs.\n");
            }
            if (mode_count == 2 && seq_cst->zero_zero == 0) {
                printf("OK: seq_cst never produced (0,0) in these runs.\n");
            }
        }
    }

    return status;
}
