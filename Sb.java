import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.IntConsumer;

/**
 * Store Buffering，Java 版：普通字段 vs volatile 字段。
 *
 * <pre>
 *   初始  a = 0   b = 0
 *
 *   线程 1          线程 2
 *   a = 1           b = 1
 *   x = b           y = a
 * </pre>
 *
 * 问：(x, y) 会不会两个都是 0？
 *
 *   JMM 里 volatile 字段的读写构成全序，所以不会；普通字段没有，会。
 *   和 sb.c 是同一个实验，三个实现换成这里的两个。
 *
 *   javac -encoding UTF-8 Sb.java
 *   java Sb
 */
public final class Sb {
    private static final int ROUNDS = 1_000_000;

    /* 热身轮数。JIT 把两条语句编译成机器码之前，测出来的是解释执行的结果，不能用。 */
    private static final int WARMUP = 100_000;

    /* 被测变量。普通字段用 a、b，volatile 用 va、vb。 */
    private static int a, b;
    private static volatile int va, vb;

    /* 两个线程各自读到的值。写它的人和读它的人被闸门同步过，所以普通 int 就够。 */
    private static int x, y;

    /*
     * 三方闸门：主线程和两个 worker 在这里对齐。最后一个到达的把代际加一，其余自旋等它。
     * 不用 CyclicBarrier，是因为那个每次唤醒要几十微秒，而这里每轮只有两条语句。
     */
    private static final class Gate {
        private final AtomicInteger arrivals = new AtomicInteger();

        /* 补 64 字节，把 generation 挤到另一条缓存行上：自旋读它会不断让这行易主。 */
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
                if (++spins == 1000) { /* 单核或高负载时别把 CPU 烧在自旋上 */
                    Thread.yield();
                    spins = 0;
                }
            }
        }
    }

    /* ============================================================= */
    /* 两个实现 —— 正题就在这 10 行                                   */
    /* ============================================================= */

    static void plain(int me) {
        if (me == 1) {
            a = 1;
            x = b;
        } else {
            b = 1;
            y = a;
        }
    }

    static void withVolatile(int me) {
        if (me == 1) {
            va = 1;
            x = vb;
        } else {
            vb = 1;
            y = va;
        }
    }

    /* ============================================================= */
    /* 下面是脚手架                                                  */
    /* ============================================================= */

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
            a = 0; /* 复位必须在第一次闸门之前 */
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
                "%-8s (0,0)=%-7d %5.1f%%   (0,1)=%-7d (1,0)=%-7d (1,1)=%d%n",
                name, count[0], 100.0 * count[0] / rounds,
                count[1], count[2], count[3]);
    }

    public static void main(String[] arguments) throws InterruptedException {
        System.out.printf("# (x,y): x = 线程1读到的 b，y = 线程2读到的 a  |  每轮 %,d 次%n",
                ROUNDS);

        run(Sb::plain, WARMUP);
        run(Sb::withVolatile, WARMUP);

        report("plain", run(Sb::plain, ROUNDS), ROUNDS);

        long[] observed = run(Sb::withVolatile, ROUNDS);
        report("volatile", observed, ROUNDS);

        if (observed[0] != 0) {
            System.err.println("FAIL: volatile 出现了 (0,0)");
            System.exit(1);
        }
        System.out.println();
        System.out.println("volatile 一次 (0,0) 都没有。plain 有。");
    }
}
