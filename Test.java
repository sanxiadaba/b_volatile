import java.util.concurrent.BrokenBarrierException;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class Test {
    private static final int DEFAULT_ITERATIONS = 100_000;

    private abstract static class State {
        int x;
        int y;

        final void reset() {
            resetSharedVariables();
            x = 0;
            y = 0;
        }

        abstract void resetSharedVariables();

        abstract void runThread1();

        abstract void runThread2();
    }

    private static final class PlainState extends State {
        private int a;
        private int b;

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

        void print(String name, int iterations) {
            System.out.printf("%s (%,d iterations)%n", name, iterations);
            System.out.printf("  (0,0): %,d%n", zeroZero);
            System.out.printf("  (0,1): %,d%n", zeroOne);
            System.out.printf("  (1,0): %,d%n", oneZero);
            System.out.printf("  (1,1): %,d%n", oneOne);
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
                state::runThread1,
                iterations,
                startBarrier,
                finishBarrier,
                readyWorkers,
                workerFailure,
                "store-buffering-thread-1");
        Thread secondThread = createWorker(
                state::runThread2,
                iterations,
                startBarrier,
                finishBarrier,
                readyWorkers,
                workerFailure,
                "store-buffering-thread-2");

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
        } catch (InterruptedException | BrokenBarrierException exception) {
            startBarrier.reset();
            finishBarrier.reset();
            throw exception;
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
                    rendezvousWorkers(readyWorkers);
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

    private static void rendezvousWorkers(AtomicInteger readyWorkers) {
        readyWorkers.incrementAndGet();
        while (readyWorkers.get() != 2) {
            // Intentional spin rendezvous; unlike a timed delay, this waits for the peer.
        }
    }

    private static int parseIterations(String[] arguments) {
        if (arguments.length == 0) {
            return DEFAULT_ITERATIONS;
        }
        if (arguments.length != 1) {
            throw new IllegalArgumentException("expected at most one argument");
        }

        int iterations = Integer.parseInt(arguments[0]);
        if (iterations <= 0) {
            throw new IllegalArgumentException("iteration count must be positive");
        }
        return iterations;
    }

    public static void main(String[] arguments) throws Exception {
        int iterations;
        try {
            iterations = parseIterations(arguments);
        } catch (IllegalArgumentException exception) {
            System.err.println("usage: java Test [positive-iteration-count]");
            System.err.println(exception.getMessage());
            System.exit(1);
            return;
        }

        OutcomeCounts plainCounts = runExperiment(new PlainState(), iterations);
        OutcomeCounts volatileCounts = runExperiment(new VolatileState(), iterations);

        plainCounts.print("Java plain fields", iterations);
        volatileCounts.print("Java volatile fields", iterations);

        if (volatileCounts.zeroZero != 0) {
            throw new AssertionError("volatile fields observed the forbidden (0,0) outcome");
        }
        if (plainCounts.zeroZero == 0) {
            System.out.println(
                    "NOTE: plain-field (0,0) is allowed but was not observed in this run.");
        }
    }
}
