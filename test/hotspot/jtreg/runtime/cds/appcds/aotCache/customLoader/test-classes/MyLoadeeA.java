public class MyLoadeeA {
    static Object[] array1;

    public MyLoadeeA() {
        if (array1 == null) {
            test();
            Object o = array1[0];
            System.out.println("array1[0] is of class: " + o.getClass());
            if (!(o instanceof MyLoadeeA)) {
                throw new RuntimeException("array1[0] should be an instanceof MyLoadeeA");
            }
        }
    }

    static void test() {
        array1 = new MyLoadeeA[10];
        for (int i = 0; i < 10; i++) {
            if ((i % 2) == 0) {
                array1[i] = new MyLoadeeB();
            } else {
                array1[i] = new MyLoadeeA();
            }
        }
    }
}
