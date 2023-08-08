public class Simple {
    public static void main(String args[]) throws Exception {
        if (args[0].equals("a")) {
            foo("222");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("b")) {
            bar("aaa", "222");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("c")) {
            baz("aaa", 333);
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("loopa")) {
            loopa();
            loopa();
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        }

        if (args.length > 1 && args[1].equals("load-extra-class")) {
            // Work around "There is no class to be included in the dynamic archive." problem, where the
            // dynamic archive is not generated.
            DummyClass.doit();
        }
    }

    static void loopa() {
        for (int i = 0; i < 100000; i++) {
            foo("L");
        }
    }

    static String x;
    static void foo(String b) {
        x = "LIT" + b;
    }
    static void bar(String a, String b) {
        x = a + b;
    }
    static void baz(String a, int b) {
        x = a + b;
    }

    static class DummyClass {
        static void doit() {}
    }
}
