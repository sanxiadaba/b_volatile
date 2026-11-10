/*
 * volatile_demo.c —— 反面教材：volatile 不是同步原语
 *
 * 这是本仓库最初那版演示（B 站视频里的代码）的整理版，单独立出来，避免把
 * 未定义行为混进 test.c 那套严谨的 C11 原子实验。
 *
 * 演示的仍然是 Store Buffering：
 *
 *   初始状态：a = 0, b = 0
 *   线程 1                线程 2
 *   a = 1                 b = 1
 *   x = b                 y = a
 *
 * 四个变量都加了 volatile。你会看到 (0, 0) 照样出现 —— 这恰好说明
 * volatile 只保证"每次都真的去读写内存"（不缓存到寄存器、不删掉），
 * 它不提供原子性、不建立 happens-before、也拦不住 CPU 的 store buffer。
 *
 * 更重要的一点：按 C11/C99 标准，这段程序里 a、b、x、y 都存在数据竞争，
 * 整个程序是未定义行为（UB）。标准不保证任何结果 —— 它能打印出 (0,0)，
 * 也只是"这台机器上这次编译恰好如此"。要观察语言层面允许什么，
 * 请用 test.c（C11 原子操作），别用这个文件。
 *
 * 构建：
 *   gcc volatile_demo.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -o volatile_demo
 *   Windows 追加 -static 可去掉 libwinpthread-1.dll 依赖。
 *
 * 看编译器怎么处理 volatile：
 *   gcc volatile_demo.c -std=c11 -O2 -pthread -S -masm=intel -o volatile_demo.s
 * 你会看到每轮都是真实的 mov 读写，但**没有任何** lock/mfence ——
 * volatile 解决的是"别把访问优化掉"，不是"别让指令并发"。
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

#define DEFAULT_ITERATIONS 100000U
#define DEFAULT_REPEAT 1U

typedef struct {
    volatile int a;
    volatile int b;
    volatile int x;
    volatile int y;

    atomic_int ready_workers;
    atomic_int start_workers;
    size_t iterations;
    pthread_barrier_t start_barrier;
    pthread_barrier_t finish_barrier;
} experiment_t;

typedef struct {
    experiment_t *experiment;
    int index;
} worker_t;

typedef struct {
    uint64_t zero_zero;
    uint64_t zero_one;
    uint64_t one_zero;
    uint64_t one_one;
} outcome_counts_t;

static void wait_at_barrier(pthread_barrier_t *barrier)
{
    int result = pthread_barrier_wait(barrier);

    if (result != 0 && result != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "pthread_barrier_wait failed: %d\n", result);
        abort();
    }
}

static void rendezvous_workers(experiment_t *experiment)
{
    unsigned int spins = 0;

    atomic_fetch_add_explicit(&experiment->ready_workers, 1,
                              memory_order_seq_cst);
    while (atomic_load_explicit(&experiment->ready_workers,
                                memory_order_seq_cst) != 2) {
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

/* 关键读写：全部是 volatile，没有任何同步语义。 */
static void critical_section(experiment_t *experiment, int index)
{
    if (index == 1) {
        experiment->a = 1;
        experiment->x = experiment->b;
    } else {
        experiment->b = 1;
        experiment->y = experiment->a;
    }
}

static void *worker_main(void *argument)
{
    worker_t *worker = argument;
    experiment_t *experiment = worker->experiment;
    size_t iteration;

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

static int run_experiment(size_t iterations, outcome_counts_t *counts)
{
    experiment_t experiment;
    worker_t workers[2];
    pthread_t threads[2];
    int created;
    int result;
    int status = EXIT_SUCCESS;
    size_t iteration;

    experiment.a = 0;
    experiment.b = 0;
    experiment.x = 0;
    experiment.y = 0;
    atomic_init(&experiment.ready_workers, 0);
    atomic_init(&experiment.start_workers, 0);
    experiment.iterations = iterations;

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
        experiment.a = 0;
        experiment.b = 0;
        experiment.x = 0;
        experiment.y = 0;
        atomic_store_explicit(&experiment.ready_workers, 0,
                              memory_order_seq_cst);

        wait_at_barrier(&experiment.start_barrier);
        wait_at_barrier(&experiment.finish_barrier);
        record_outcome(counts, experiment.x, experiment.y);
    }

    result = pthread_join(threads[0], NULL);
    if (result != 0) {
        fprintf(stderr, "pthread_join(worker 1) failed: %d\n", result);
        status = EXIT_FAILURE;
    }
    result = pthread_join(threads[1], NULL);
    if (result != 0) {
        fprintf(stderr, "pthread_join(worker 2) failed: %d\n", result);
        status = EXIT_FAILURE;
    }

    if (pthread_barrier_destroy(&experiment.finish_barrier) != 0) {
        status = EXIT_FAILURE;
    }
    if (pthread_barrier_destroy(&experiment.start_barrier) != 0) {
        status = EXIT_FAILURE;
    }

    return status;
}

static bool parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    uintmax_t parsed;

    if (text == NULL || *text == '\0') {
        return false;
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

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [options] [iterations]\n"
            "\n"
            "volatile store-buffering demo (C11 data race, undefined behavior).\n"
            "\n"
            "options:\n"
            "  -n, --iterations N   iterations per run (default %u)\n"
            "  -r, --repeat R       independent runs (default %u)\n"
            "  -h, --help           show this help\n",
            program, DEFAULT_ITERATIONS, DEFAULT_REPEAT);
}

int main(int argc, char **argv)
{
    size_t iterations = DEFAULT_ITERATIONS;
    size_t repeat = DEFAULT_REPEAT;
    outcome_counts_t totals = {0, 0, 0, 0};
    size_t run;
    int index;

    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];

        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argument, "-n") == 0 || strcmp(argument, "--iterations") == 0) {
            if (index + 1 >= argc || !parse_size(argv[index + 1], &iterations)) {
                fprintf(stderr, "invalid iteration count\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            ++index;
            continue;
        }
        if (strcmp(argument, "-r") == 0 || strcmp(argument, "--repeat") == 0) {
            if (index + 1 >= argc || !parse_size(argv[index + 1], &repeat)) {
                fprintf(stderr, "invalid repeat count\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            ++index;
            continue;
        }
        if (!parse_size(argument, &iterations)) {
            fprintf(stderr, "unknown argument: %s\n", argument);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    printf("# volatile store-buffering demo —— 按 C11 属于未定义行为（data race）\n");
    printf("# iterations per run: %" PRIuMAX " | runs: %" PRIuMAX "\n",
           (uintmax_t) iterations, (uintmax_t) repeat);

    for (run = 0; run < repeat; ++run) {
        outcome_counts_t counts = {0, 0, 0, 0};

        if (run_experiment(iterations, &counts) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }

        totals.zero_zero += counts.zero_zero;
        totals.zero_one += counts.zero_one;
        totals.one_zero += counts.one_zero;
        totals.one_one += counts.one_one;

        printf("run %" PRIuMAX "/%" PRIuMAX "  n=%" PRIuMAX
               "  (0,0)=%" PRIu64 "  (0,1)=%" PRIu64 "  (1,0)=%" PRIu64
               "  (1,1)=%" PRIu64 "\n",
               (uintmax_t) (run + 1), (uintmax_t) repeat,
               (uintmax_t) iterations, counts.zero_zero, counts.zero_one,
               counts.one_zero, counts.one_one);
    }

    printf("total    n=%" PRIuMAX "  (0,0)=%" PRIu64 "  (0,1)=%" PRIu64
           "  (1,0)=%" PRIu64 "  (1,1)=%" PRIu64 "\n",
           (uintmax_t) (iterations * repeat), totals.zero_zero,
           totals.zero_one, totals.one_zero, totals.one_one);

    if (totals.zero_zero != 0) {
        printf("\n(0,0) 出现了 %" PRIu64
               " 次：volatile 只保证访存不被优化掉，"
               "既没有原子性也没有 happens-before，\n"
               "更拦不住 CPU 的 store buffer。它不能用来做线程间同步。\n",
               totals.zero_zero);
    } else {
        printf("\n本次没有观察到 (0,0)。别急着下结论：这段代码是 UB，"
               "出现与不出现都没有保证；\n"
               "换台机器、换个优化级别、多跑几轮再试。\n");
    }

    printf("如需严谨对照，请用 test.c（C11 原子操作 + seq_cst 对照）。\n");

    return EXIT_SUCCESS;
}
