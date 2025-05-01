
import java.lang.management.ManagementFactory;
import java.lang.management.RuntimeMXBean;
import jdk.management.AOTMXBean;
import jdk.management.VirtualThreadSchedulerMXBean;
import java.util.*;
import java.util.stream.*;

public class MyTest {

    public int x = 0;
    public AOTMXBean aot;
    private Random random = new Random();

    public  MyTest() {
        aot = ManagementFactory.getPlatformMXBean(AOTMXBean.class);
        if (aot != null) {
            System.out.println("AOTMXBean is available");
            System.out.println("AOT mode: " + aot.getMode());
            if (aot.isRecording())  {
                System.out.println("AOT not recording");
            }
            System.out.println("AOT isRecording: " + aot.isRecording());
        } else {
            System.out.println("AOTMXBean is not available");
        }

        // Constructor
        setX(1);

    }

    public void setX(int x) {
        this.x = x;
    }

    public void work(long count)
    {
        long min = 5;
        long max = 42;
        long duration = random.nextInt((int) (max - min)) + min;
        try {
            Thread.sleep(duration);
        } catch (InterruptedException e) {
            System.out.println("Thread interrupted");
        }
    }

//    private String WorkDoneStr(WorkDone workDone) {
//        return workDone.durationMS + " @ " + workDone.eventTime;
 //   }

    public void doSomeWork(long count)
    {
        long time;
        for (long i = 0; i < count; i++) {
            time = System.currentTimeMillis();
            work(count);
           // aot.logWorkDone("test", System.currentTimeMillis() - time);
        }
        if (aot.isRecording()) {
            if (aot.endRecording()) {
                System.out.println("AOT recording ended");
            }
        } else {
            System.out.println("AOT recording not in progress");
        }
        System.out.println("work done");
        System.out.println("  iterations: " + count);

        /*
        System.out.println("  AOT work done first: " + WorkDoneStr(aot.getFirstWorkDone("test")));
        System.out.println("  AOT work done min: " + WorkDoneStr(aot.getMinWorkDone("test")));
        System.out.println("  AOT work done max: " + WorkDoneStr(aot.getMaxWorkDone("test")));
        System.out.println("  AOT work done last: " + WorkDoneStr(aot.getLastWorkDone("test")));
        System.out.println("  AOT work done average: " + aot.getAverageWorkDone("test"));
        System.out.println("  AOT RPS: " + aot.getRequestsPerSecond("test"));
        */

        RuntimeMXBean mxBean = ManagementFactory.getRuntimeMXBean();

        if (aot.getRecordingDuration() > 0){
            System.out.println("  JVM startime: " + mxBean.getStartTime());
            System.out.println("  AOT recording duration: " + aot.getRecordingDuration());
            System.out.println("  AOT recording ended: " + (mxBean.getStartTime() + aot.getRecordingDuration()));
        }
    }

    public static void main(String ... args) {
        System.out.println("test");  // 1
        var test = new MyTest();
        test.doSomeWork(args.length > 0 ? Long.parseLong(args[0]) : 1);
        var words = List.of("hello", "fuzzy", "world");
        var greeting = words.stream()
            .filter(w -> !w.contains("z"))
            .collect(Collectors.joining(", "));
        System.out.println(greeting);  // hello, world
    }
}