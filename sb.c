/*
 * Store Buffering：两个线程各写一个变量，再去读对方那个变量。
 *
 *   初始  a = 0   b = 0
 *
 *   线程 1          线程 2
 *   a = 1           b = 1
 *   x = b           y = a
 *
 * 问：(x, y) 会不会两个都是 0？
 *
 *   顺序一致说不会，relaxed 和 C 的 volatile 说会 —— 这页代码就是把三种写法都跑一遍。
 *   全文件只有四样东西：三个实现、一个闸门、一个统计循环、一个 main。
 *   读代码只看下面三个函数就够了，它们唯一的差别是内存序。
 *
 *   gcc sb.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -o sb
 *   Windows 加 -static，否则产物依赖 libwinpthread-1.dll。
 */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

#define ROUNDS 1000000

/* a、b 各占一条缓存行。挤在同一条上看到的是缓存行竞争，跟内存序无关。 */
#define CACHE 64

/* 被测的变量。原子版用 a、b，volatile 版用 va、vb —— 类型不同，只能各来一套。 */
static _Alignas(CACHE) _Atomic int a, b;
static _Alignas(CACHE) volatile int va, vb;

/* 两个线程各自读到的值。写它的人和读它的人被闸门同步过，所以普通 int 就够。 */
static _Alignas(CACHE) int x;
static _Alignas(CACHE) int y;

/* ================================================================= */
/* 三个实现 —— 正题就在这 40 行                                       */
/* ================================================================= */

static void relaxed(int me)
{
    if (me == 1) {
        atomic_store_explicit(&a, 1, memory_order_relaxed);
        x = atomic_load_explicit(&b, memory_order_relaxed);
    } else {
        atomic_store_explicit(&b, 1, memory_order_relaxed);
        y = atomic_load_explicit(&a, memory_order_relaxed);
    }
}

static void seq_cst(int me)
{
    if (me == 1) {
        atomic_store_explicit(&a, 1, memory_order_seq_cst);
        x = atomic_load_explicit(&b, memory_order_seq_cst);
    } else {
        atomic_store_explicit(&b, 1, memory_order_seq_cst);
        y = atomic_load_explicit(&a, memory_order_seq_cst);
    }
}

static void with_volatile(int me)
{
    if (me == 1) {
        va = 1;
        x = vb;
    } else {
        vb = 1;
        y = va;
    }
}

/* ================================================================= */
/* 下面是脚手架，和内存序无关，可以先跳过                               */
/* ================================================================= */

/*
 * 三方闸门：主线程和两个 worker 在这里对齐。最后一个到达的把代际加一，其余自旋等它。
 * 不用 pthread_barrier_t，是因为那个每次唤醒要几十微秒，而这里每轮只有两条指令。
 */
static _Alignas(CACHE) _Atomic int arrivals;
static _Alignas(CACHE) _Atomic int generation;

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
        if (++spins == 1000) { /* 单核或高负载时别把 CPU 烧在自旋上 */
            sched_yield();
            spins = 0;
        }
    }
}

static void (*impl)(int); /* 当前在跑哪一种 */

static void *worker(void *arg)
{
    int me = *(int *)arg;

    for (int round = 0; round < ROUNDS; round++) {
        gate();       /* 等主线程把 a、b 清零 */
        impl(me);     /* 写自己的，读对方的 */
        gate();       /* 等两边都做完 */
    }
    return NULL;
}

/* 跑 ROUNDS 轮，返回 (0,0) 出现了多少次。 */
static long report(const char *name, void (*fn)(int))
{
    long count[4] = {0, 0, 0, 0}; /* 下标就是 x*2+y，即 (0,0) (0,1) (1,0) (1,1) */
    int one = 1;
    int two = 2;
    pthread_t t1, t2;

    impl = fn;
    pthread_create(&t1, NULL, worker, &one);
    pthread_create(&t2, NULL, worker, &two);

    for (int round = 0; round < ROUNDS; round++) {
        atomic_store(&a, 0); /* 复位必须在第一次闸门之前 */
        atomic_store(&b, 0);
        va = 0;
        vb = 0;
        gate();
        gate();
        count[x * 2 + y]++;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("%-8s (0,0)=%-7ld %5.1f%%   (0,1)=%-7ld (1,0)=%-7ld (1,1)=%ld\n",
           name, count[0], 100.0 * count[0] / ROUNDS,
           count[1], count[2], count[3]);
    return count[0];
}

int main(void)
{
    long forbidden;

    printf("# (x,y): x = 线程1读到的 b，y = 线程2读到的 a  |  每轮 %d 次\n", ROUNDS);

    report("relaxed", relaxed);
    forbidden = report("seq_cst", seq_cst);
    report("volatile", with_volatile);

    if (forbidden != 0) {
        fprintf(stderr, "FAIL: seq_cst 出现了 (0,0)\n");
        return 1;
    }
    printf("\nseq_cst 一次 (0,0) 都没有。relaxed 和 volatile 有。\n");
    return 0;
}
