public class Test {
    private volatile static int x = 0, y = 0;
    private static int a = 0, b = 0;

    public static void main(String[] args) throws InterruptedException {
        int i = 0;
        while (true) {
            i++;
            x = 0;y = 0;
            a = 0;b = 0;
            Thread thread1 = new Thread(() -> {
                // 空转cpu，等线程2
                waitThread2();
                a = 1;
                x = b;
            });

            Thread thread2 = new Thread(() -> {
                b = 1;
                y = a;
            });

            thread1.start();thread2.start();
            thread1.join();thread2.join();

            String result = "第 " + i + " 次执行 x = " + x + " y = " + y;
            if (x == 0 && y == 0) {
                System.out.println(result);
                break;
            } else {
                System.out.println(result);
            }
        }
    }

    public static void waitThread2(){
        long start=System.nanoTime();
        long end=start+20000;
        while (start<end){
            start=System.nanoTime();
        }
    }

}