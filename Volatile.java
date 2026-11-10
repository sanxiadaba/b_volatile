import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.IntConsumer;

/**
 * 同样的问题，Java 版。
 *
 * 两个线程同时做两件事：线程 1 写 a 再读 b，线程 2 写 b 再读 a。
 * 按顺序执行的话，两个线程不可能同时读到 0。
 *
 * Java 里只有两种写法值得比：普通字段，和 volatile 字段。
 * 和 C 不一样的地方在于 —— Java 的 volatile 是真的能让它不出问题。
 *
 *   javac -encoding UTF-8 Volatile.java
 *   java Volatile
 */
public final class Volatile {
    private static final int ROUNDS = 1_000_000;

    /* 热身轮数。JIT 还没把这两条语句编译成机器码之前，测出来的数不能用。 */
    private static final int WARMUP = 100_000;

    /* 被测变量：普通字段用 a、b，volatile 字段用 va、vb。 */
    private static int a, b;
    private static volatile int va, vb;

    /* 两个线程各自读到的值。写的人和读的人被闸门隔开了，所以普通 int 就够。 */
    private static int x, y;

    /* ============================================================= */
    /* 两种写法 —— 正题就在这 15 行                                   */
    /* ============================================================= */

    /* 第一种：普通字段，什么特殊处理都没有。 */
    static void use_plain(int me) {
        if (me == 1) {
            a = 1;
            x = b;
        } else {
            b = 1;
            y = a;
        }
    }

    /*
     * 第二种：volatile 字段。
     * Java 的 volatile 是真的同步：它保证这两个读写按顺序发生，别的线程也能立刻看到。
     * 和 C 的 volatile 完全是两回事。
     */
    static void use_volatile(int me) {
        if (me == 1) {
            va = 1;
            x = vb;
        } else {
            vb = 1;
            y = va;
        }
    }

    /* ============================================================= */
    /* 下面是脚手架，和结论无关，可以先跳过                            */
    /* ============================================================= */

    /*
     * 闸门：让主线程和两个 worker 步调一致。
     * 每轮先把变量清零、两个线程都到位才开跑；两边都做完了，主线程才读结果。
     * 没用 CyclicBarrier，是因为那个每次唤醒要几十微秒，而真正要测的只有两条语句。
     */
    private static final class Gate {
        private final AtomicInteger arrivals = new AtomicInteger();

        /* 中间垫 64 字节，让 generation 落在另一条缓存行上，不然自旋读它会和 arrivals 抢。 */
        private long p0, p1, p2, p3, p4, p5, p6, p7;

        private volatile int generation;

        void await() {
            int gen = generation;

            if (arrivals.getAndIncrement() == 2) { /* 三个都到齐了 */
                arrivals.set(0);
                generation = gen + 1;
                return;
            }
            int spins = 0;
            while (generation == gen) {
                if (++spins == 1000) { /* 只有一两个核的时候，别死占着 CPU */
                    Thread.yield();
                    spins = 0;
                }
            }
        }
    }

    /* 跑 rounds 轮，返回四种结果的计数，下标是 x*2+y。 */
    private static long[] run(IntConsumer impl, int rounds) throws InterruptedException {
        Gate start = new Gate();
        Gate finish = new Gate();
        long[] count = new long[4];

        Thread first = new Thread(() -> {
            for (int round = 0; round < rounds; round++) {
                start.await();
                impl.accept(1);
                finish.await();
            }
        });
        Thread second = new Thread(() -> {
            for (int round = 0; round < rounds; round++) {
                start.await();
                impl.accept(2);
                finish.await();
            }
        });

        first.start();
        second.start();

        for (int round = 0; round < rounds; round++) {
            a = 0; /* 清零要在第一次闸门之前 */
            b = 0;
            va = 0;
            vb = 0;
            start.await();
            finish.await();
            count[x * 2 + y]++;
        }

        first.join();
        second.join();
        return count;
    }

    private static void report(String name, long[] count, int rounds) {
        System.out.printf(
                "%-9s 两边都读到 0：%-7d 次 (%.1f%%)   只有一方读到 1：%-7d / %-7d   两边都读到 1：%d%n",
                name, count[0], 100.0 * count[0] / rounds,
                count[1], count[2], count[3]);
    }

    public static void main(String[] arguments) throws InterruptedException {
        System.out.printf("x = 线程1读到的 b，y = 线程2读到的 a。每种写法各跑 %,d 轮。%n%n",
                ROUNDS);

        run(Volatile::use_plain, WARMUP); /* 先热身，不统计 */
        run(Volatile::use_volatile, WARMUP);

        long[] plain = run(Volatile::use_plain, ROUNDS);
        report("plain", plain, ROUNDS);

        long[] observed = run(Volatile::use_volatile, ROUNDS);
        report("volatile", observed, ROUNDS);

        System.out.println();

        if (observed[0] != 0) {
            System.out.println("出问题了：volatile 也出现了两边都读到 0。");
        } else if (plain[0] == 0) {
            System.out.println("只出现了部分结果：plain 一次都没有。换台机器或重跑几次试试。");
        } else {
            System.out.println("volatile 一次都没有。plain 有。");
        }
    }
}
