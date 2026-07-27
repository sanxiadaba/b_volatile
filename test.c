#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_ITERATIONS 100000U

typedef enum {
    MODE_RELAXED,
    MODE_SEQ_CST
} memory_mode_t;

typedef struct {
    atomic_int a;
    atomic_int b;
    atomic_int ready_workers;
    atomic_int start_workers;
    int x;
    int y;
    size_t iterations;
    memory_mode_t mode;
    pthread_barrier_t start_barrier;
    pthread_barrier_t finish_barrier;
} experiment_t;

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
    atomic_fetch_add_explicit(&experiment->ready_workers, 1,
                              memory_order_seq_cst);
    while (atomic_load_explicit(&experiment->ready_workers,
                                memory_order_seq_cst) != 2) {
    }
}

static int wait_for_worker_start(experiment_t *experiment)
{
    int state;

    do {
        state = atomic_load_explicit(&experiment->start_workers,
                                     memory_order_acquire);
    } while (state == 0);

    return state > 0;
}

static void *thread_01(void *argument)
{
    experiment_t *experiment = argument;
    size_t iteration;

    if (!wait_for_worker_start(experiment)) {
        return NULL;
    }

    if (experiment->mode == MODE_RELAXED) {
        for (iteration = 0; iteration < experiment->iterations; ++iteration) {
            wait_at_barrier(&experiment->start_barrier);
            rendezvous_workers(experiment);
            atomic_store_explicit(&experiment->a, 1, memory_order_relaxed);
            experiment->x = atomic_load_explicit(&experiment->b,
                                                 memory_order_relaxed);
            wait_at_barrier(&experiment->finish_barrier);
        }
    } else {
        for (iteration = 0; iteration < experiment->iterations; ++iteration) {
            wait_at_barrier(&experiment->start_barrier);
            rendezvous_workers(experiment);
            atomic_store_explicit(&experiment->a, 1, memory_order_seq_cst);
            experiment->x = atomic_load_explicit(&experiment->b,
                                                 memory_order_seq_cst);
            wait_at_barrier(&experiment->finish_barrier);
        }
    }

    return NULL;
}

static void *thread_02(void *argument)
{
    experiment_t *experiment = argument;
    size_t iteration;

    if (!wait_for_worker_start(experiment)) {
        return NULL;
    }

    if (experiment->mode == MODE_RELAXED) {
        for (iteration = 0; iteration < experiment->iterations; ++iteration) {
            wait_at_barrier(&experiment->start_barrier);
            rendezvous_workers(experiment);
            atomic_store_explicit(&experiment->b, 1, memory_order_relaxed);
            experiment->y = atomic_load_explicit(&experiment->a,
                                                 memory_order_relaxed);
            wait_at_barrier(&experiment->finish_barrier);
        }
    } else {
        for (iteration = 0; iteration < experiment->iterations; ++iteration) {
            wait_at_barrier(&experiment->start_barrier);
            rendezvous_workers(experiment);
            atomic_store_explicit(&experiment->b, 1, memory_order_seq_cst);
            experiment->y = atomic_load_explicit(&experiment->a,
                                                 memory_order_seq_cst);
            wait_at_barrier(&experiment->finish_barrier);
        }
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

static int run_experiment(memory_mode_t mode, size_t iterations,
                          outcome_counts_t *counts)
{
    experiment_t experiment;
    pthread_t first_thread;
    pthread_t second_thread;
    int result;
    int status = EXIT_SUCCESS;
    size_t iteration;

    experiment.x = 0;
    experiment.y = 0;
    experiment.iterations = iterations;
    experiment.mode = mode;
    atomic_init(&experiment.a, 0);
    atomic_init(&experiment.b, 0);
    atomic_init(&experiment.ready_workers, 0);
    atomic_init(&experiment.start_workers, 0);

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

    result = pthread_create(&first_thread, NULL, thread_01, &experiment);
    if (result != 0) {
        fprintf(stderr, "pthread_create(thread_01) failed: %d\n", result);
        pthread_barrier_destroy(&experiment.finish_barrier);
        pthread_barrier_destroy(&experiment.start_barrier);
        return EXIT_FAILURE;
    }

    result = pthread_create(&second_thread, NULL, thread_02, &experiment);
    if (result != 0) {
        fprintf(stderr, "pthread_create(thread_02) failed: %d\n", result);
        atomic_store_explicit(&experiment.start_workers, -1,
                              memory_order_release);
        pthread_join(first_thread, NULL);
        pthread_barrier_destroy(&experiment.finish_barrier);
        pthread_barrier_destroy(&experiment.start_barrier);
        return EXIT_FAILURE;
    }

    atomic_store_explicit(&experiment.start_workers, 1, memory_order_release);

    for (iteration = 0; iteration < iterations; ++iteration) {
        atomic_store_explicit(&experiment.a, 0, memory_order_relaxed);
        atomic_store_explicit(&experiment.b, 0, memory_order_relaxed);
        atomic_store_explicit(&experiment.ready_workers, 0,
                              memory_order_seq_cst);
        experiment.x = 0;
        experiment.y = 0;

        wait_at_barrier(&experiment.start_barrier);
        wait_at_barrier(&experiment.finish_barrier);
        record_outcome(counts, experiment.x, experiment.y);
    }

    {
        int first_result = pthread_join(first_thread, NULL);
        int second_result = pthread_join(second_thread, NULL);

        if (first_result != 0) {
            fprintf(stderr, "pthread_join(thread_01) failed: %d\n",
                    first_result);
        }
        if (second_result != 0) {
            fprintf(stderr, "pthread_join(thread_02) failed: %d\n",
                    second_result);
        }
        if (first_result != 0 || second_result != 0) {
            status = EXIT_FAILURE;
        }
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

static void print_counts(const char *name, size_t iterations,
                         const outcome_counts_t *counts)
{
    printf("%s (%lu iterations)\n", name, (unsigned long) iterations);
    printf("  (0,0): %" PRIu64 "\n", counts->zero_zero);
    printf("  (0,1): %" PRIu64 "\n", counts->zero_one);
    printf("  (1,0): %" PRIu64 "\n", counts->one_zero);
    printf("  (1,1): %" PRIu64 "\n", counts->one_one);
}

static int parse_iterations(const char *text, size_t *iterations)
{
    char *end;
    const char *cursor;
    uintmax_t value;

    if (*text == '\0') {
        return EXIT_FAILURE;
    }

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return EXIT_FAILURE;
        }
    }

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > SIZE_MAX) {
        return EXIT_FAILURE;
    }

    *iterations = (size_t) value;
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    size_t iterations = DEFAULT_ITERATIONS;
    outcome_counts_t relaxed_counts = {0, 0, 0, 0};
    outcome_counts_t seq_cst_counts = {0, 0, 0, 0};

    if (argc > 2 || (argc == 2 &&
        parse_iterations(argv[1], &iterations) != EXIT_SUCCESS)) {
        fprintf(stderr, "usage: %s [positive-iteration-count]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (run_experiment(MODE_RELAXED, iterations, &relaxed_counts) !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    if (run_experiment(MODE_SEQ_CST, iterations, &seq_cst_counts) !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    print_counts("C11 relaxed atomics", iterations, &relaxed_counts);
    print_counts("C11 sequentially consistent atomics", iterations,
                 &seq_cst_counts);

    if (seq_cst_counts.zero_zero != 0) {
        fprintf(stderr, "FAIL: seq_cst observed the forbidden (0,0) outcome\n");
        return EXIT_FAILURE;
    }

    if (relaxed_counts.zero_zero == 0) {
        printf("NOTE: relaxed (0,0) is allowed but was not observed in this run.\n");
    }

    return EXIT_SUCCESS;
}
