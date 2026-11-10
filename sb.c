/*
 * Store Buffering litmus test —— 三段关键读写并排，看编译器和 CPU 允许什么
 *
 *   初始 a = 0, b = 0
 *
 *   线程 1          线程 2
 *   a = 1           b = 1
 *   x = b           y = a
 *
 * 顺序一致（SC）下 (x, y) = (0, 0) 不可能。它要求
 *   w(a) <po r(b)  且  w(b) <po r(a)     —— 程序顺序
 *   r(b) <  w(b)   且  r(a) <  w(a)      —— (0,0) 的含义
 * 四条边闭合成环，与 SC 的全序冲突。
 *
 * 三段实现回答的是同一个问题：
 *
 *   relaxed  两个访存都是 relaxed 原子操作   (0,0) 允许
 *   seq_cst  两个访存都是 seq_cst 原子操作   (0,0) 禁止
 *   volatile 两个访存都是 volatile 普通读写   (0,0) 会出现，但这整段程序是
 *            data race（UB），标准不保证任何结果 —— 它只作反面演示
 *
 * 构建：
 *   gcc sb.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -o sb
 *   Windows 追加 -static，否则产物依赖 libwinpthread-1.dll。
 *   运行：./sb -n 1000000
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
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

#define DEFAULT_ITERATIONS 1000000U
#define DEFAULT_REPEAT 1U

/* a、b 各占一条缓存行。挤在同一条上看到的是缓存行竞争，不是内存序，所以不做对照。 */
#define CACHE_LINE 64U

/* 主线程 + 两个 worker。 */
#define GATE_PARTIES 3
#define GATE_SPIN_LIMIT 1000U

typedef enum {
    IMPL_RELAXED = 0,
    IMPL_SEQ_CST,
    IMPL_VOLATILE,
    IMPL_COUNT
} impl_t;

static const char *const IMPL_NAME[IMPL_COUNT] = {"relaxed", "seq_cst", "volatile"};

/*
 * a、b 是被测变量，x、y 装两个线程读到的值。
 *
 * relaxed / seq_cst 需要 _Atomic，volatile 演示需要 volatile —— 两者布局相同，
 * 这里用 union 复用同一块内存。每次实验只用其中一边，不存在类型双关。
 */
typedef union {
    struct {
        _Alignas(CACHE_LINE) _Atomic int a, b, x, y;
    } atom;
    struct {
        _Alignas(CACHE_LINE) volatile int a, b, x, y;
    } vol;
} shared_t;

/* ------------------------------------------------------------------ */
/* 三段关键读写 —— 全仓库的重点就在这里                                 */
/* ------------------------------------------------------------------ */

static void run_relaxed(shared_t *shared, int index)
{
    int observed;

    if (index == 1) {
        atomic_store_explicit(&shared->atom.a, 1, memory_order_relaxed);
        observed = atomic_load_explicit(&shared->atom.b, memory_order_relaxed);
        atomic_store_explicit(&shared->atom.x, observed, memory_order_relaxed);
    } else {
        atomic_store_explicit(&shared->atom.b, 1, memory_order_relaxed);
        observed = atomic_load_explicit(&shared->atom.a, memory_order_relaxed);
        atomic_store_explicit(&shared->atom.y, observed, memory_order_relaxed);
    }
}

static void run_seq_cst(shared_t *shared, int index)
{
    int observed;

    if (index == 1) {
        atomic_store_explicit(&shared->atom.a, 1, memory_order_seq_cst);
        observed = atomic_load_explicit(&shared->atom.b, memory_order_seq_cst);
        atomic_store_explicit(&shared->atom.x, observed, memory_order_relaxed);
    } else {
        atomic_store_explicit(&shared->atom.b, 1, memory_order_seq_cst);
        observed = atomic_load_explicit(&shared->atom.a, memory_order_seq_cst);
        atomic_store_explicit(&shared->atom.y, observed, memory_order_relaxed);
    }
}

static void run_volatile(shared_t *shared, int index)
{
    if (index == 1) {
        shared->vol.a = 1;
        shared->vol.x = shared->vol.b;
    } else {
        shared->vol.b = 1;
        shared->vol.y = shared->vol.a;
    }
}

/*
 * 上面三个函数体几乎一样，是故意的：内存序必须是编译期常量。把它做成函数参数
 * （atomic_store_explicit(p, 1, order)，order 是运行时变量）时，编译器无法常量
 * 折叠，会保守地生成 xchg —— 隐含 lock、全栅栏 —— relaxed 组会退化成 seq_cst，
 * (0,0) 恒为 0。用 make asm 看 sb.s，relaxed 路径一旦出现 xchg/lock 就是退化了。
 */

typedef void (*critical_fn)(shared_t *, int);

/* ------------------------------------------------------------------ */
/* 自旋闸门                                                           */
/* ------------------------------------------------------------------ */

/*
 * 三方对齐用不着 pthread_barrier_t。那个走 futex/condvar，一次唤醒几十微秒，
 * 而两个 worker 醒来之后本来就得重新对齐 —— 屏障提供的"同时起跑"被完全丢掉，
 * 只剩开销。这里自己对齐：最后一个到达的线程翻代际，其余自旋等它。
 *
 * 代际计数而不是简单的到齐标记，是为了让同一轮里先走的线程能立刻进入下一轮。
 */
typedef struct {
    _Alignas(CACHE_LINE) _Atomic int arrivals;
    _Alignas(CACHE_LINE) _Atomic int generation;
} gate_t;

static void gate_init(gate_t *gate)
{
    atomic_init(&gate->arrivals, 0);
    atomic_init(&gate->generation, 0);
}

static void gate_wait(gate_t *gate)
{
    int generation = atomic_load_explicit(&gate->generation, memory_order_acquire);
    unsigned int spins = 0;

    if (atomic_fetch_add_explicit(&gate->arrivals, 1, memory_order_acq_rel)
        == GATE_PARTIES - 1) {
        atomic_store_explicit(&gate->arrivals, 0, memory_order_relaxed);
        atomic_store_explicit(&gate->generation, generation + 1,
                              memory_order_release);
        return;
    }

    while (atomic_load_explicit(&gate->generation, memory_order_acquire)
           == generation) {
        /* 单核或 runner 超卖时不要把 CPU 烧在自旋上。 */
        if (++spins == GATE_SPIN_LIMIT) {
            sched_yield();
            spins = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 一次实验                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    shared_t shared;
    critical_fn critical;
    impl_t impl;
    size_t iterations;
    gate_t start;
    gate_t finish;
} experiment_t;

typedef struct {
    experiment_t *experiment;
    int index; /* 1 或 2，决定该线程走哪一半 */
} worker_t;

typedef struct {
    uint64_t zero_zero;
    uint64_t zero_one;
    uint64_t one_zero;
    uint64_t one_one;
} outcome_counts_t;

/*
 * 只复位 a、b。x、y 每轮都会被两个线程各自写一次，不需要复位。
 */
static void reset_shared(experiment_t *experiment)
{
    if (experiment->impl == IMPL_VOLATILE) {
        experiment->shared.vol.a = 0;
        experiment->shared.vol.b = 0;
    } else {
        atomic_store_explicit(&experiment->shared.atom.a, 0, memory_order_relaxed);
        atomic_store_explicit(&experiment->shared.atom.b, 0, memory_order_relaxed);
    }
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

static void record_current(experiment_t *experiment, outcome_counts_t *counts)
{
    if (experiment->impl == IMPL_VOLATILE) {
        record_outcome(counts, experiment->shared.vol.x,
                       experiment->shared.vol.y);
    } else {
        record_outcome(counts,
                       atomic_load_explicit(&experiment->shared.atom.x,
                                            memory_order_relaxed),
                       atomic_load_explicit(&experiment->shared.atom.y,
                                            memory_order_relaxed));
    }
}

static void *worker_main(void *argument)
{
    worker_t *worker = argument;
    experiment_t *experiment = worker->experiment;
    size_t iteration;

    for (iteration = 0; iteration < experiment->iterations; ++iteration) {
        gate_wait(&experiment->start);
        experiment->critical(&experiment->shared, worker->index);
        gate_wait(&experiment->finish);
    }

    return NULL;
}

static void run_experiment(impl_t impl, size_t iterations,
                           outcome_counts_t *counts)
{
    experiment_t experiment;
    worker_t workers[2];
    pthread_t threads[2];
    size_t iteration;
    int index;

    /* 整体清零：union 两种布局下，这份零表示就是 a = b = x = y = 0。 */
    memset(&experiment, 0, sizeof(experiment));
    experiment.impl = impl;
    experiment.iterations = iterations;
    experiment.critical = impl == IMPL_RELAXED   ? run_relaxed
                          : impl == IMPL_SEQ_CST ? run_seq_cst
                                                 : run_volatile;

    gate_init(&experiment.start);
    gate_init(&experiment.finish);
    reset_shared(&experiment);

    for (index = 0; index < 2; ++index) {
        workers[index].experiment = &experiment;
        workers[index].index = index + 1;

        if (pthread_create(&threads[index], NULL, worker_main,
                           &workers[index]) != 0) {
            /* 两个 worker 此时都还没跑起来，直接退出进程即可，不必回滚。 */
            fprintf(stderr, "pthread_create(worker %d) failed\n", index + 1);
            exit(EXIT_FAILURE);
        }
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        /* 复位必须在 start 闸门之前：闸门同时负责把这次写发布给两个 worker。 */
        reset_shared(&experiment);

        gate_wait(&experiment.start);
        gate_wait(&experiment.finish);

        record_current(&experiment, counts);
    }

    for (index = 0; index < 2; ++index) {
        pthread_join(threads[index], NULL);
    }
}

/* ------------------------------------------------------------------ */
/* 环境与输出                                                         */
/* ------------------------------------------------------------------ */

static unsigned int cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO info;

    GetSystemInfo(&info);
    return (unsigned int) info.dwNumberOfProcessors;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);

    return count > 0 ? (unsigned int) count : 0U;
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

static double percentage(uint64_t part, size_t total)
{
    return total == 0 ? 0.0 : 100.0 * (double) part / (double) total;
}

static void print_counts(const char *name, const outcome_counts_t *counts,
                         size_t total)
{
    printf("%-8s (0,0)=%-8" PRIu64 " %5.1f%%   (0,1)=%-8" PRIu64
           " (1,0)=%-8" PRIu64 " (1,1)=%" PRIu64 "\n",
           name, counts->zero_zero, percentage(counts->zero_zero, total),
           counts->zero_one, counts->one_zero, counts->one_one);
}

/* ------------------------------------------------------------------ */
/* 命令行                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t iterations;
    size_t repeat;
    impl_t impl;
    bool all_impls;
} options_t;

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法: %s [选项]\n"
            "\n"
            "Store Buffering litmus test：relaxed / seq_cst / volatile 三种实现对照。\n"
            "\n"
            "选项:\n"
            "  -n, --iterations N   每轮的关键读写次数（默认 %u）\n"
            "  -r, --repeat R       每个实现独立跑几次（默认 %u）\n"
            "  -m, --mode M         all | relaxed | seq_cst | volatile（默认 all）\n"
            "  -h, --help           显示本帮助\n"
            "\n"
            "退出码: 0 正常，1 参数错误，2 seq_cst 观察到了被禁止的 (0,0)\n",
            program, DEFAULT_ITERATIONS, DEFAULT_REPEAT);
}

static bool parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    uintmax_t parsed;

    if (text == NULL || *text == '\0') {
        return false;
    }

    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }

    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || parsed == 0
        || parsed > (uintmax_t) SIZE_MAX) {
        return false;
    }

    *value = (size_t) parsed;
    return true;
}

static bool parse_options(int argc, char **argv, options_t *options)
{
    int index;

    options->iterations = DEFAULT_ITERATIONS;
    options->repeat = DEFAULT_REPEAT;
    options->impl = IMPL_RELAXED;
    options->all_impls = true;

    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value;
        bool needs_value;

        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }

        needs_value = strcmp(argument, "-n") == 0
                      || strcmp(argument, "--iterations") == 0
                      || strcmp(argument, "-r") == 0
                      || strcmp(argument, "--repeat") == 0
                      || strcmp(argument, "-m") == 0
                      || strcmp(argument, "--mode") == 0;

        if (!needs_value) {
            fprintf(stderr, "未知参数: %s\n", argument);
            return false;
        }
        if (index + 1 >= argc) {
            fprintf(stderr, "%s 缺少取值\n", argument);
            return false;
        }
        value = argv[++index];

        if (strcmp(argument, "-m") == 0 || strcmp(argument, "--mode") == 0) {
            impl_t impl;

            if (strcmp(value, "all") == 0) {
                options->all_impls = true;
                continue;
            }
            for (impl = 0; impl < IMPL_COUNT; ++impl) {
                if (strcmp(value, IMPL_NAME[impl]) == 0) {
                    break;
                }
            }
            if (impl == IMPL_COUNT) {
                fprintf(stderr, "未知模式: %s\n", value);
                return false;
            }
            options->all_impls = false;
            options->impl = impl;
        } else if (strcmp(argument, "-n") == 0
                   || strcmp(argument, "--iterations") == 0) {
            if (!parse_size(value, &options->iterations)) {
                fprintf(stderr, "迭代次数无效: %s\n", value);
                return false;
            }
        } else {
            if (!parse_size(value, &options->repeat)) {
                fprintf(stderr, "重复次数无效: %s\n", value);
                return false;
            }
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    options_t options;
    outcome_counts_t totals[IMPL_COUNT] = {{0, 0, 0, 0}};
    bool ran[IMPL_COUNT] = {false, false, false};
    impl_t order[IMPL_COUNT];
    size_t count = 0;
    size_t run;
    size_t position;
    int status = EXIT_SUCCESS;

    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (options.all_impls) {
        for (position = 0; position < IMPL_COUNT; ++position) {
            order[count++] = (impl_t) position;
        }
    } else {
        order[count++] = options.impl;
    }

    printf("# %s | %s | %u cpus | %u-bit | %" PRIuMAX " iterations\n",
           platform_name(), compiler_name(), cpu_count(),
           (unsigned int) (sizeof(void *) * 8U),
           (uintmax_t) options.iterations);
    printf("# (x,y): x = 线程1读到的 b，y = 线程2读到的 a\n");

    if (options.all_impls || options.impl == IMPL_VOLATILE) {
        printf("# 注: volatile 模式整段程序是 data race（UB），"
               "它的数字只作演示，没有任何保证\n");
    }

    /*
     * 交替先后顺序：如果永远先跑同一个实现，缓存预热、CPU 频率、调度状态
     * 都会系统性地偏向它。
     */
    for (run = 0; run < options.repeat; ++run) {
        for (position = 0; position < count; ++position) {
            impl_t impl =
                order[(run % 2U) ? count - 1 - position : position];
            outcome_counts_t counts = {0, 0, 0, 0};

            run_experiment(impl, options.iterations, &counts);

            totals[impl].zero_zero += counts.zero_zero;
            totals[impl].zero_one += counts.zero_one;
            totals[impl].one_zero += counts.one_zero;
            totals[impl].one_one += counts.one_one;
            ran[impl] = true;

            print_counts(IMPL_NAME[impl], &counts, options.iterations);

            if (impl == IMPL_SEQ_CST && counts.zero_zero != 0) {
                fprintf(stderr,
                        "FAIL: seq_cst 观察到了被禁止的 (0,0)\n");
                status = 2;
            }
        }
    }

    if (options.repeat > 1) {
        printf("\n# %" PRIuMAX " 轮合计\n", (uintmax_t) options.repeat);

        for (position = 0; position < count; ++position) {
            impl_t impl = order[position];

            print_counts(IMPL_NAME[impl], &totals[impl],
                         options.iterations * options.repeat);
        }
    }

    if (status == EXIT_SUCCESS && ran[IMPL_SEQ_CST]) {
        printf("\nseq_cst 从未出现 (0,0)，符合顺序一致。\n");

        if ((ran[IMPL_RELAXED] && totals[IMPL_RELAXED].zero_zero != 0)
            || (ran[IMPL_VOLATILE] && totals[IMPL_VOLATILE].zero_zero != 0)) {
            printf("relaxed / volatile 出现了 (0,0)："
                   "两者都不拦 CPU 的 store buffer。\n");
        }
    }

    return status;
}
