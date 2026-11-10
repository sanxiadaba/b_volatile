import java.util.concurrent.atomic.AtomicInteger;

/**
 * Store Buffering litmus test —— Java 普通字段 vs volatile 字段。
 *
 * <pre>
 *   初始 a = 0, b = 0
 *
 *   线程 1          线程 2
 *   a = 1           b = 1
 *   x = b           y = a
 * </pre>
 *
 * JMM 的 synchronizes-with 只由 volatile 访问这类同步动作建立。volatile 字段的
 * 读写构成 synchronization order，所以 volatile 组不可能出现 (0, 0)；普通字段
 * 没有这个全序，允许出现 —— 而且普通字段的读本身就是一个 data race。
 *
 * 这里和 sb.c 是同一个实验的两种语言版本，看的是同一件事：C 的 volatile 只是
 * "别把访存优化掉"，Java 的 volatile 才真的带内存语义。
 *
 * 用法：java Sb [-n N] [-r R] [-m all|plain|volatile]
 */
public final class Sb {
    private static final int DEFAULT_ITERATIONS = 1_000_000;
    private static final int DEFAULT_REPEAT = 1;

    /* 主线程 + 两个 worker。 */
    private static final int GATE_PARTIES = 3;
    private static final int GATE_SPIN_LIMIT = 1000;

    /* 热身轮数，见 warmUp()。 */
    private static final int WARMUP_ITERATIONS = 100_000;

    private Sb() {
    }

    /**
     * 三方自旋闸门，和 sb.c 里那个是同一个东西。
     *
     * 不用 CyclicBarrier：它走锁 + 条件队列，一次唤醒几十微秒，而两个 worker
     * 醒来后本来就得重新对齐，屏障那个"同时起跑"完全用不上，只剩开销。
     */
    private static final class Gate {
        /*
         * arrivals 和 generation 必须落在不同的缓存行上。三个线程都在自旋读
         * generation，那会让它所在的行不断易主；写者却正需要同一行来推进
         * arrivals。挤在一行时实测 (0,0) 在 3%–37% 之间乱跳，分开后稳定在
         * 24%–34%。sb.c 里对应的是两个 _Alignas(64)。
         */
        private final AtomicInteger arrivals = new AtomicInteger();
        private long pad0, pad1, pad2, pad3, pad4, pad5, pad6, pad7;
        private volatile int generation;

        void await() {
            int gen = generation;

            if (arrivals.getAndIncrement() == GATE_PARTIES - 1) {
                arrivals.set(0);
                generation = gen + 1;
                return;
            }

            int spins = 0;
            while (generation == gen) {
                if (++spins == GATE_SPIN_LIMIT) {
                    Thread.yield();
                    spins = 0;
                }
            }
        }
    }

    private enum Mode {
        PLAIN("plain"),
        VOLATILE("volatile");

        private final String label;

        Mode(String label) {
            this.label = label;
        }

        String label() {
            return label;
        }
    }

    /** x、y 是普通字段，由闸门负责发布给主线程。 */
    private abstract static class State {
        int x;
        int y;

        abstract String label();

        abstract void runThread1();

        abstract void runThread2();

        abstract void reset();
    }

    private static final class PlainState extends State {
        private int a;
        private int b;

        @Override
        String label() {
            return Mode.PLAIN.label();
        }

        @Override
        void reset() {
            a = 0;
            b = 0;
        }

        @Override
        void runThread1() {
            a = 1;
            x = b;
        }

        @Override
        void runThread2() {
            b = 1;
            y = a;
        }
    }

    private static final class VolatileState extends State {
        private volatile int a;
        private volatile int b;

        @Override
        String label() {
            return Mode.VOLATILE.label();
        }

        @Override
        void reset() {
            a = 0;
            b = 0;
        }

        @Override
        void runThread1() {
            a = 1;
            x = b;
        }

        @Override
        void runThread2() {
            b = 1;
            y = a;
        }
    }

    /** 下标 = x * 2 + y，即 (0,0) (0,1) (1,0) (1,1)。 */
    private static long[] runExperiment(State state, int iterations)
            throws InterruptedException {
        Gate start = new Gate();
        Gate finish = new Gate();
        long[] counts = new long[4];

        Thread first = new Thread(() -> {
            for (int iteration = 0; iteration < iterations; iteration++) {
                start.await();
                state.runThread1();
                finish.await();
            }
        }, "sb-thread-1");

        Thread second = new Thread(() -> {
            for (int iteration = 0; iteration < iterations; iteration++) {
                start.await();
                state.runThread2();
                finish.await();
            }
        }, "sb-thread-2");

        first.start();
        second.start();

        for (int iteration = 0; iteration < iterations; iteration++) {
            /* 复位必须在 start 闸门之前：闸门同时负责把它发布给两个 worker。 */
            state.reset();

            start.await();
            finish.await();

            counts[state.x * 2 + state.y]++;
        }

        first.join();
        second.join();
        return counts;
    }

    /**
     * 先把两个 State 的关键路径跑到 JIT 编译完成，再开始计数。
     *
     * 不热身的话，第一轮 plain 测到的是解释执行的结果 —— 实测 (0,0) 只有 2%，
     * 而同一条路径在 C2 编译之后是 45%。差的是 JIT 有没有重排这两条访存，
     * 不是内存模型，所以不热身测出来的数没法用。
     */
    private static void warmUp(int iterations) throws InterruptedException {
        int rounds = Math.min(iterations, WARMUP_ITERATIONS);

        runExperiment(new PlainState(), rounds);
        runExperiment(new VolatileState(), rounds);
    }

    private static void printCounts(String name, long[] counts, long total) {
        System.out.printf(
                "%-8s (0,0)=%-8d %5.1f%%   (0,1)=%-8d (1,0)=%-8d (1,1)=%d%n",
                name, counts[0], 100.0 * counts[0] / total,
                counts[1], counts[2], counts[3]);
    }

    private static void printUsage() {
        System.err.println("用法: java Sb [选项]");
        System.err.println();
        System.err.println("Store Buffering litmus test：Java 普通字段 / volatile 字段对照。");
        System.err.println();
        System.err.println("选项:");
        System.err.println("  -n, --iterations N   每轮的关键读写次数（默认 "
                + DEFAULT_ITERATIONS + "）");
        System.err.println("  -r, --repeat R       每个实现独立跑几次（默认 "
                + DEFAULT_REPEAT + "）");
        System.err.println("  -m, --mode M         all | plain | volatile（默认 all）");
        System.err.println("  -h, --help           显示本帮助");
        System.err.println();
        System.err.println("退出码: 0 正常，1 参数错误，2 volatile 观察到了 (0,0)");
    }

    private static int parsePositive(String text, String what) {
        try {
            int value = Integer.parseInt(text);

            if (value > 0) {
                return value;
            }
        } catch (NumberFormatException ignored) {
            // 落到下面统一报错。
        }

        System.err.println(what + "无效: " + text);
        return -1;
    }

    public static void main(String[] arguments) throws InterruptedException {
        int iterations = DEFAULT_ITERATIONS;
        int repeat = DEFAULT_REPEAT;
        boolean allModes = true;
        Mode singleMode = Mode.PLAIN;

        for (int index = 0; index < arguments.length; index++) {
            String option = arguments[index];
            String value;

            if (option.equals("-h") || option.equals("--help")) {
                printUsage();
                return;
            }

            boolean needsValue = option.equals("-n") || option.equals("--iterations")
                    || option.equals("-r") || option.equals("--repeat")
                    || option.equals("-m") || option.equals("--mode");

            if (!needsValue) {
                System.err.println("未知参数: " + option);
                printUsage();
                System.exit(1);
            }
            if (index + 1 >= arguments.length) {
                System.err.println(option + " 缺少取值");
                printUsage();
                System.exit(1);
            }
            value = arguments[++index];

            if (option.equals("-m") || option.equals("--mode")) {
                if (value.equals("all")) {
                    allModes = true;
                } else if (value.equals("plain")) {
                    allModes = false;
                    singleMode = Mode.PLAIN;
                } else if (value.equals("volatile")) {
                    allModes = false;
                    singleMode = Mode.VOLATILE;
                } else {
                    System.err.println("未知模式: " + value);
                    printUsage();
                    System.exit(1);
                }
            } else if (option.equals("-n") || option.equals("--iterations")) {
                iterations = parsePositive(value, "迭代次数");
                if (iterations < 0) {
                    printUsage();
                    System.exit(1);
                }
            } else {
                repeat = parsePositive(value, "重复次数");
                if (repeat < 0) {
                    printUsage();
                    System.exit(1);
                }
            }
        }

        Mode[] order = allModes
                ? new Mode[]{Mode.PLAIN, Mode.VOLATILE}
                : new Mode[]{singleMode};

        System.out.printf("# %s %s | %s %s | %d cpus | %d iterations%n",
                System.getProperty("os.name"), System.getProperty("os.arch"),
                System.getProperty("java.vm.name"), System.getProperty("java.version"),
                Runtime.getRuntime().availableProcessors(), iterations);

        if (allModes || singleMode == Mode.PLAIN) {
            System.out.println("# 注: plain 模式在 JMM 里是 data race：合法，但读到 0 还是 1"
                    + " 没有保证；具体比例随 JIT 状态变化，只有 volatile 恒为 0 是结论");
        }

        long[][] totals = new long[Mode.values().length][4];
        boolean[] ran = new boolean[Mode.values().length];
        int status = 0;

        warmUp(iterations);

        /*
         * 交替先后顺序：如果永远先跑 plain，JIT 层次、缓存预热、CPU 频率都会
         * 系统性地偏向其中一组。
         */
        for (int run = 0; run < repeat; run++) {
            boolean reversed = run % 2 != 0;

            for (int position = 0; position < order.length; position++) {
                Mode mode = order[reversed ? order.length - 1 - position : position];
                State state = mode == Mode.PLAIN ? new PlainState() : new VolatileState();
                long[] counts = runExperiment(state, iterations);

                for (int cell = 0; cell < 4; cell++) {
                    totals[mode.ordinal()][cell] += counts[cell];
                }
                ran[mode.ordinal()] = true;

                printCounts(mode.label(), counts, iterations);

                if (mode == Mode.VOLATILE && counts[0] != 0) {
                    System.err.println("FAIL: volatile 观察到了被禁止的 (0,0)");
                    status = 2;
                }
            }
        }

        if (repeat > 1) {
            System.out.println();
            System.out.println("# " + repeat + " 轮合计");

            for (Mode mode : order) {
                printCounts(mode.label(), totals[mode.ordinal()],
                        (long) iterations * repeat);
            }
        }

        if (status == 0) {
            System.out.println();
            System.out.println("volatile 从未出现 (0,0)，符合 JMM。");

            if (ran[Mode.PLAIN.ordinal()] && totals[Mode.PLAIN.ordinal()][0] != 0) {
                System.out.println("plain 出现了 (0,0)：普通字段之间没有同步顺序。");
            }
        }

        if (status != 0) {
            System.exit(status);
        }
    }
}
