import java.util.concurrent.BrokenBarrierException;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Store Buffering litmus test —— Java 普通字段 / volatile 字段对照实验。
 *
 * <pre>
 *   初始状态：a = 0, b = 0
 *   线程 1                线程 2
 *   a = 1                 b = 1
 *   x = b                 y = a
 * </pre>
 *
 * JMM 的 synchronizes-with 只由 volatile 访问等同步动作建立。volatile 字段上
 * 的写与读构成全序（synchronization order），因此 volatile 组不可能出现
 * (0, 0)；普通字段没有这个全序，允许出现。
 *
 * 结果变量 x/y 是普通字段，由 CyclicBarrier 的 happens-before 安全发布给主线程，
 * 这正好演示了"屏障负责发布、volatile 负责同步"的分工。
 *
 * 用法：java Test [options] [iterations]
 */
public final class Test {
    private static final int DEFAULT_ITERATIONS = 100_000;
    private static final int DEFAULT_REPEAT = 3;

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

    private abstract static class State {
        int x;
        int y;

        abstract String label();

        abstract void resetSharedVariables();

        abstract void runThread1();

        abstract void runThread2();

        final void reset() {
            resetSharedVariables();
            x = 0;
            y = 0;
        }
    }

    private static final class PlainState extends State {
        private int a;
        private int b;

        @Override
        String label() {
            return Mode.PLAIN.label();
        }

        @Override
        void resetSharedVariables() {
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
        void resetSharedVariables() {
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

    private static final class OutcomeCounts {
        long zeroZero;
        long zeroOne;
        long oneZero;
        long oneOne;

        void record(int x, int y) {
            if (x == 0 && y == 0) {
                zeroZero++;
            } else if (x == 0 && y == 1) {
                zeroOne++;
            } else if (x == 1 && y == 0) {
                oneZero++;
            } else if (x == 1 && y == 1) {
                oneOne++;
            } else {
                throw new AssertionError("unexpected outcome: x=" + x + ", y=" + y);
            }
        }

        void add(OutcomeCounts other) {
            zeroZero += other.zeroZero;
            zeroOne += other.zeroOne;
            oneZero += other.oneZero;
            oneOne += other.oneOne;
        }

        String toCsvRow(String mode, int run, int iterations) {
            return String.format("%s,%d,%d,%d,%d,%d,%d",
                    mode, run, iterations, zeroZero, zeroOne, oneZero, oneOne);
        }

        String toLine(String mode, int run, int runs, int iterations) {
            double percent = iterations == 0 ? 0.0 : 100.0 * zeroZero / iterations;
            return String.format("%-8s run %d/%d  n=%d  (0,0)=%,d (%5.1f%%)"
                            + "  (0,1)=%,d  (1,0)=%,d  (1,1)=%,d",
                    mode, run, runs, iterations, zeroZero, percent,
                    zeroOne, oneZero, oneOne);
        }

        String toTotalLine(String mode, int iterations) {
            double percent = iterations == 0 ? 0.0 : 100.0 * zeroZero / iterations;
            return String.format("%-8s total  n=%,d  (0,0)=%,d (%5.1f%%)"
                            + "  (0,1)=%,d  (1,0)=%,d  (1,1)=%,d",
                    mode, iterations, zeroZero, percent, zeroOne, oneZero, oneOne);
        }
    }

    private static void spinRendezvous(AtomicInteger readyWorkers) {
        readyWorkers.incrementAndGet();
        int spins = 0;
        while (readyWorkers.get() != 2) {
            if (++spins == 1000) {
                Thread.yield();
                spins = 0;
            }
        }
    }

    private static OutcomeCounts runExperiment(State state, int iterations)
            throws InterruptedException, BrokenBarrierException {
        CyclicBarrier startBarrier = new CyclicBarrier(3);
        CyclicBarrier finishBarrier = new CyclicBarrier(3);
        AtomicInteger readyWorkers = new AtomicInteger();
        AtomicReference<Throwable> workerFailure = new AtomicReference<>();
        OutcomeCounts counts = new OutcomeCounts();

        Thread firstThread = createWorker(
                state::runThread1, iterations, startBarrier, finishBarrier,
                readyWorkers, workerFailure, "store-buffering-thread-1");
        Thread secondThread = createWorker(
                state::runThread2, iterations, startBarrier, finishBarrier,
                readyWorkers, workerFailure, "store-buffering-thread-2");

        firstThread.start();
        secondThread.start();

        try {
            for (int iteration = 0; iteration < iterations; iteration++) {
                state.reset();
                readyWorkers.set(0);
                startBarrier.await();
                finishBarrier.await();
                counts.record(state.x, state.y);
            }
        } finally {
            firstThread.join();
            secondThread.join();
        }

        Throwable failure = workerFailure.get();
        if (failure != null) {
            throw new IllegalStateException("worker thread failed", failure);
        }

        return counts;
    }

    private static Thread createWorker(
            Runnable action,
            int iterations,
            CyclicBarrier startBarrier,
            CyclicBarrier finishBarrier,
            AtomicInteger readyWorkers,
            AtomicReference<Throwable> workerFailure,
            String name) {
        return new Thread(() -> {
            try {
                for (int iteration = 0; iteration < iterations; iteration++) {
                    startBarrier.await();
                    spinRendezvous(readyWorkers);
                    action.run();
                    finishBarrier.await();
                }
            } catch (InterruptedException exception) {
                Thread.currentThread().interrupt();
                workerFailure.compareAndSet(null, exception);
                startBarrier.reset();
                finishBarrier.reset();
            } catch (BrokenBarrierException | RuntimeException | Error exception) {
                workerFailure.compareAndSet(null, exception);
                startBarrier.reset();
                finishBarrier.reset();
            }
        }, name);
    }

    private static void printEnvironment(boolean csv, int iterations, int repeat) {
        java.io.PrintStream stream = csv ? System.err : System.out;
        stream.printf("# %s %s | %s %s | %d logical CPUs%n",
                System.getProperty("os.name"),
                System.getProperty("os.arch"),
                System.getProperty("java.vm.name"),
                System.getProperty("java.version"),
                Runtime.getRuntime().availableProcessors());
        stream.printf("# iterations per run: %,d | runs per mode: %d%n",
                iterations, repeat);
    }

    private static void printUsage() {
        System.err.println("usage: java Test [options] [iterations]");
        System.err.println();
        System.err.println("Store Buffering litmus test (Java plain vs volatile fields).");
        System.err.println();
        System.err.println("options:");
        System.err.println("  -n, --iterations N   iterations per run (default "
                + DEFAULT_ITERATIONS + ")");
        System.err.println("  -r, --repeat R       independent runs per mode (default "
                + DEFAULT_REPEAT + ")");
        System.err.println("  -m, --mode MODE      all | plain | volatile (default all)");
        System.err.println("      --csv            machine readable output on stdout");
        System.err.println("  -h, --help           show this help");
        System.err.println();
        System.err.println("exit status: 0 ok, 1 usage error, 2 volatile 观察到 (0,0)");
    }

    private static Integer parsePositive(String text, String what) {
        try {
            int value = Integer.parseInt(text);
            if (value <= 0) {
                System.err.println(what + " must be positive: " + text);
                return null;
            }
            return value;
        } catch (NumberFormatException exception) {
            System.err.println("invalid " + what + ": " + text);
            return null;
        }
    }

    public static void main(String[] arguments) {
        int iterations = DEFAULT_ITERATIONS;
        int repeat = DEFAULT_REPEAT;
        boolean csv = false;
        boolean allModes = true;
        Mode singleMode = Mode.PLAIN;

        for (int index = 0; index < arguments.length; index++) {
            String argument = arguments[index];
            String value = null;
            String option = argument;

            if (argument.startsWith("--") && argument.indexOf('=') > 0) {
                int equals = argument.indexOf('=');
                option = argument.substring(0, equals);
                value = argument.substring(equals + 1);
            }

            if (option.equals("-h") || option.equals("--help")) {
                printUsage();
                return;
            }
            if (option.equals("--csv")) {
                csv = true;
                continue;
            }
            if (option.equals("-n") || option.equals("--iterations")
                    || option.equals("-r") || option.equals("--repeat")
                    || option.equals("-m") || option.equals("--mode")) {
                if (value == null) {
                    if (index + 1 >= arguments.length) {
                        System.err.println(option + " requires a value");
                        printUsage();
                        System.exit(1);
                        return;
                    }
                    value = arguments[++index];
                }

                if (option.equals("-n") || option.equals("--iterations")) {
                    Integer parsed = parsePositive(value, "iteration count");
                    if (parsed == null) {
                        printUsage();
                        System.exit(1);
                        return;
                    }
                    iterations = parsed;
                } else if (option.equals("-r") || option.equals("--repeat")) {
                    Integer parsed = parsePositive(value, "repeat count");
                    if (parsed == null) {
                        printUsage();
                        System.exit(1);
                        return;
                    }
                    repeat = parsed;
                } else if (value.equals("all")) {
                    allModes = true;
                } else if (value.equals("plain")) {
                    allModes = false;
                    singleMode = Mode.PLAIN;
                } else if (value.equals("volatile")) {
                    allModes = false;
                    singleMode = Mode.VOLATILE;
                } else {
                    System.err.println("unknown mode: " + value);
                    printUsage();
                    System.exit(1);
                    return;
                }
                continue;
            }
            if (argument.matches("[0-9]+")) {
                Integer parsed = parsePositive(argument, "iteration count");
                if (parsed == null) {
                    System.exit(1);
                    return;
                }
                iterations = parsed;
                continue;
            }

            System.err.println("unknown option: " + argument);
            printUsage();
            System.exit(1);
            return;
        }

        printEnvironment(csv, iterations, repeat);
        if (csv) {
            System.out.println("mode,run,iterations,zero_zero,zero_one,one_zero,one_one");
        }

        OutcomeCounts[] totals = {new OutcomeCounts(), new OutcomeCounts()};
        int status = 0;

        try {
            for (int run = 0; run < repeat; run++) {
                Mode[] order = allModes
                        ? (run % 2 == 0
                                ? new Mode[]{Mode.PLAIN, Mode.VOLATILE}
                                : new Mode[]{Mode.VOLATILE, Mode.PLAIN})
                        : new Mode[]{singleMode};

                for (Mode mode : order) {
                    State state = mode == Mode.PLAIN
                            ? new PlainState()
                            : new VolatileState();
                    OutcomeCounts counts = runExperiment(state, iterations);

                    totals[mode.ordinal()].add(counts);

                    if (csv) {
                        System.out.println(counts.toCsvRow(mode.label(), run + 1, iterations));
                    } else {
                        System.out.println(counts.toLine(mode.label(), run + 1, repeat, iterations));
                    }

                    if (mode == Mode.VOLATILE && counts.zeroZero != 0) {
                        System.err.println(
                                "FAIL: volatile fields observed the forbidden (0,0) outcome");
                        status = 2;
                    }
                }
            }
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
            System.err.println("interrupted");
            System.exit(1);
            return;
        } catch (BrokenBarrierException | IllegalStateException exception) {
            System.err.println("experiment failed: " + exception);
            System.exit(1);
            return;
        }

        if (!csv) {
            Mode[] printed = allModes ? new Mode[]{Mode.PLAIN, Mode.VOLATILE} : new Mode[]{singleMode};
            for (Mode mode : printed) {
                System.out.println(totals[mode.ordinal()].toTotalLine(mode.label(), iterations * repeat));
            }
            if (status == 0 && allModes) {
                if (totals[Mode.PLAIN.ordinal()].zeroZero == 0) {
                    System.out.println(
                            "NOTE: plain-field (0,0) is allowed but was not observed in these runs.");
                } else {
                    System.out.println(
                            "OK: plain fields produced (0,0), volatile fields never did.");
                }
            }
        }

        if (status != 0) {
            System.exit(status);
        }
    }
}
