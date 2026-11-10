/*
 * volatile 能不能拿来让两个线程同步？不能。这个程序跑给你看。
 *
 * 两个线程同时做两件事：
 *
 *   线程 1：a = 1，然后读 b         读到的存进 x
 *   线程 2：b = 1，然后读 a         读到的存进 y
 *
 * 如果每条语句都老老实实按写的顺序发生，两个线程不可能同时读到 0：
 * 线程 1 读 b 之前已经写过 a，线程 2 读 a 之前已经写过 b。
 *
 * 可是实际跑起来，有些写法能测出"两边都读到 0"。下面三种写法，看是哪几种。
 * 正题只有那三个函数，后面标了"脚手架"的部分和结论无关，可以先跳过。
 *
 *   gcc volatile.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -o volatile
 *   Windows 加 -static，不然换台机器会报缺 libwinpthread-1.dll。
 */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

#define ROUNDS 1000000

/* 让 a、b 离得远一点，各占一条 64 字节的缓存行。不加也能跑，只是数字会变一点。 */
#define SPACED 64

/* ================================================================= */
/* 三种写法 —— 正题就在这 50 行                                       */
/* ================================================================= */

/*
 * 被测的变量。原子版用 a、b，volatile 版用 va、vb。
 * 类型不一样，没法共用，只能各来一套。
 */
static _Alignas(SPACED) _Atomic int a, b;
static _Alignas(SPACED) volatile int va, vb;

/* 两个线程各自读到的值。写的人和读的人被闸门隔开了，所以普通 int 就够。 */
static _Alignas(SPACED) int x;
static _Alignas(SPACED) int y;

/*
 * 第一种：你可能听说过的写法 —— 给变量加 volatile。
 *
 * volatile 只保证"这几条读写在，编译器别删掉"，它不保证这两条谁先谁后，
 * 也不保证别的线程什么时候能看到你写的值。
 */
static void use_volatile(int me)
{
    if (me == 1) {
        va = 1;
        x = vb;
    } else {
        vb = 1;
        y = va;
    }
}

/*
 * 第二种：改用原子操作 _Atomic，但明确声明"顺序我不管" —— memory_order_relaxed。
 *
 * 它保证每次读写不会被撕裂（要么看到整个 0，要么看到整个 1），
 * 但两条语句之间的先后照样可以让编译器或 CPU 调换。
 */
static void atomic_no_order(int me)
{
    if (me == 1) {
        atomic_store_explicit(&a, 1, memory_order_relaxed);
        x = atomic_load_explicit(&b, memory_order_relaxed);
    } else {
        atomic_store_explicit(&b, 1, memory_order_relaxed);
        y = atomic_load_explicit(&a, memory_order_relaxed);
    }
}

/*
 * 第三种：原子操作 + memory_order_seq_cst，要求这两条严格按写的顺序发生。
 * 三种写法里只有它能保证"不会两边同时读到 0"。
 */
static void atomic_ordered(int me)
{
    if (me == 1) {
        atomic_store_explicit(&a, 1, memory_order_seq_cst);
        x = atomic_load_explicit(&b, memory_order_seq_cst);
    } else {
        atomic_store_explicit(&b, 1, memory_order_seq_cst);
        y = atomic_load_explicit(&a, memory_order_seq_cst);
    }
}

/* ================================================================= */
/* 下面是脚手架，和结论无关，可以先跳过                                 */
/* ================================================================= */

/*
 * 闸门：让主线程和两个 worker 步调一致。
 *
 * 每轮分两段：先把 a、b 清零，两个线程都看到之后才开始；两边都做完了，主线程才读结果。
 * 少了它，读到的会是上一轮剩下的值，实验就不成立了。
 *
 * 没用 pthread_barrier，是因为那个每次唤醒要几十微秒，而真正要测的只有两条语句。
 */
static _Alignas(SPACED) _Atomic int arrivals;
static _Alignas(SPACED) _Atomic int generation;

static void gate(void)
{
    int gen = atomic_load(&generation);
    unsigned spins = 0;

    if (atomic_fetch_add(&arrivals, 1) == 2) { /* 三个都到齐了 */
        atomic_store(&arrivals, 0);
        atomic_store(&generation, gen + 1);
        return;
    }
    while (atomic_load(&generation) == gen) {
        if (++spins == 1000) { /* 只有一两个核的时候，别死占着 CPU */
            sched_yield();
            spins = 0;
        }
    }
}

static void (*current)(int); /* 这一轮在跑哪种写法 */

static void *worker(void *arg)
{
    int me = *(int *)arg;

    for (int round = 0; round < ROUNDS; round++) {
        gate();        /* 等主线程把变量清零 */
        current(me);   /* 写自己的，读对方的 */
        gate();        /* 等两边都做完 */
    }
    return NULL;
}

/* 跑 ROUNDS 轮，返回"两个都读到 0"出现了多少次。 */
static long run(const char *name, void (*fn)(int))
{
    /* 下标就是 x*2+y：(0,0) (0,1) (1,0) (1,1) */
    long count[4] = {0, 0, 0, 0};
    int one = 1;
    int two = 2;
    pthread_t t1, t2;

    current = fn;
    pthread_create(&t1, NULL, worker, &one);
    pthread_create(&t2, NULL, worker, &two);

    for (int round = 0; round < ROUNDS; round++) {
        atomic_store(&a, 0); /* 清零要在第一次闸门之前 */
        atomic_store(&b, 0);
        va = 0;
        vb = 0;
        gate();
        gate();
        count[x * 2 + y]++;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("%-9s 两边都读到 0：%-7ld 次 (%.1f%%)   只有一方读到 1：%-7ld / %-7ld   两边都读到 1：%ld\n",
           name, count[0], 100.0 * count[0] / ROUNDS,
           count[1], count[2], count[3]);
    return count[0];
}

int main(void)
{
    long v, w, s;

    printf("x = 线程1读到的 b，y = 线程2读到的 a。每种写法各跑 %d 轮。\n\n",
           ROUNDS);

    v = run("volatile", use_volatile);
    w = run("relaxed", atomic_no_order);
    s = run("seq_cst", atomic_ordered);

    printf("\n");

    if (s != 0) {
        printf("出问题了：seq_cst 也出现了两边都读到 0，说明代码或机器有问题。\n");
        return 1;
    }
    if (v == 0 || w == 0) {
        printf("只出现了部分结果：volatile=%ld, relaxed=%ld。换台机器或重跑几次试试。\n",
               v, w);
        return 0;
    }
    printf("只有 seq_cst 一次都没有。volatile 和 relaxed 都有。\n");
    return 0;
}
