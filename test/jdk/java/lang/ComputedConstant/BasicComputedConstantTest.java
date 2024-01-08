/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * @test
 * @summary Verify basic ComputedConstant operations
 * @enablePreview
 * @run junit BasicComputedConstantTest
 */

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.Collection;
import java.util.NoSuchElementException;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Supplier;
import java.util.stream.IntStream;
import java.util.stream.Stream;

import static org.junit.jupiter.api.Assertions.*;

final class BasicComputedConstantTest {

    static final int MAGIC_VALUE = 42;

    private CountingIntegerSupplierSingleton supplier;
    private ComputedConstant<Integer> constant;

    @BeforeEach
    void setup() {
        supplier = new CountingIntegerSupplierSingleton();
        constant = ComputedConstant.of(supplier);
    }

    @ParameterizedTest
    @MethodSource("computedConstantTypes")
    void get(ComputedConstant<Integer> cc) {
        int inv = invocations();
        Integer val = cc.get();
        assertEquals(MAGIC_VALUE, val);
        assertEquals(inv + 1, invocations());
        Integer val2 = cc.get();
        assertEquals(MAGIC_VALUE, val2);
        assertEquals(inv + 1, invocations());
    }

    @Test
    void nullSupplier() {
        assertThrows(NullPointerException.class, () ->
                ComputedConstant.of((Supplier<?>) null)
        );
    }

    @Test
    void supplierReturnsNull() {
        var nullBound = ComputedConstant.of(() -> null);
        assertNull(nullBound.get());
    }

    @Test
    void optionalModelling() {
        Supplier<Optional<String>> empty = ComputedConstant.of(Optional::empty);
        assertTrue(empty.get().isEmpty());
        Supplier<Optional<String>> present = ComputedConstant.of(() -> Optional.of("A"));
        assertEquals("A", present.get().orElseThrow());
    }

    @Test
    void error() {
        var cnt = new AtomicInteger();

        Supplier<Integer> throwingSupplier = () -> {
            cnt.getAndIncrement();
            throw new UnsupportedOperationException();
        };
        ComputedConstant<Integer> l = ComputedConstant.of(throwingSupplier);

        try {
            l.get();
        } catch (Exception e) {
            assertEquals(NoSuchElementException.class, e.getClass());
            assertEquals(UnsupportedOperationException.class, e.getCause().getClass());
        }
        assertEquals(1, cnt.get());

        try {
            l.get();
        } catch (Exception e) {
            assertEquals(NoSuchElementException.class, e.getClass());
            // Todo: cause should not be reveled more than once
            assertNull(e.getCause());
        }
        // Should not invoke the supplier again
        assertEquals(1, cnt.get());
    }

    // Todo:repeat the test many times
    @Test
    void threadTest() {

        final class Value {

            public Value() {
                this.one = 1;
                this.two = 2;
                this.three = 3;

            }

            // Non-final fields
            int one,two,three;

            void isValid() {
                assertEquals(3, three);
                assertEquals(2, two);
                assertEquals(1, one);
            }

        }

        for (int i = 0; i < 10; i++) {
            var constant = ComputedConstant.of(Value::new);
            var gate = new AtomicBoolean();
            var threads = IntStream.range(0, Runtime.getRuntime().availableProcessors() * 2)
                    .mapToObj(__ -> new Thread(() -> {
                        while (!gate.get()) {
                            Thread.onSpinWait();
                        }
                        // Try to access the instance "simultaneously"
                        Value v = constant.get();
                        v.isValid();
                    }))
                    .toList();
            threads.forEach(Thread::start);
            LockSupport.parkNanos(500);
            gate.set(true);
            join(threads);
        }
    }

    @Test
    void testToString() {
        var c0 = ComputedConstant.of(() -> 0);
        var c1 = ComputedConstant.of(() -> 1);

        c1.get();

        // Do not touch c0
        c1.get();
        assertEquals(c0.getClass().getSimpleName() + ".unbound", c0.toString());
        assertEquals(c0.getClass().getSimpleName() + "[1]", c1.toString());
    }

    @Test
    void testCircular() {
        staticComputedConstant = ComputedConstant.of(() -> staticComputedConstant.get());
        assertThrows(StackOverflowError.class, () -> {
            staticComputedConstant.get();
        });
    }

    private static ComputedConstant<Integer> staticComputedConstant;

    private static ComputedConstant<Integer> a;
    private static ComputedConstant<Integer> b;

    @Test
    void testCircular2() {
        a = ComputedConstant.of(() -> b.get());
        b = ComputedConstant.of(() -> a.get());
        assertThrows(StackOverflowError.class, () -> {
            a.get();
        });
    }

    @Test
    void testOrElse() {
        ComputedConstant<Integer> c = ComputedConstant.of(() -> {
            throw new UnsupportedOperationException();
        });

        int actual = c.orElse(42);
        assertEquals(42, actual);
        // Try again
        assertEquals(42, c.orElse(42));
    }

    @Test
    void testOrElseGet() {
        ComputedConstant<Integer> c = ComputedConstant.of(() -> {
            throw new UnsupportedOperationException();
        });

        int actual = c.orElseGet(() -> 42);
        assertEquals(42, actual);
        // Try again
        assertEquals(42, c.orElseGet(() -> 42));
    }

    @Test
    void testOrElseThrow() {
        ComputedConstant<Integer> c = ComputedConstant.of(() -> {
            throw new UnsupportedOperationException();
        });

        assertThrows(ArithmeticException.class, () -> {
            c.orElseThrow(ArithmeticException::new);
        });
        // Try again
        assertThrows(ArithmeticException.class, () -> {
            c.orElseThrow(ArithmeticException::new);
        });

    }

    private static void join(Collection<Thread> threads) {
        for (var t : threads) {
            try {
                t.join();
            } catch (InterruptedException e) {
                throw new AssertionError(e);
            }
        }
    }

    public static final class CountingIntegerSupplier implements Supplier<Integer> {
        @Override
        public Integer get() {
            return CountingIntegerSupplierSingleton.INSTANCE.get();
        }
    }

    private static final class CountingIntegerSupplierSingleton implements Supplier<Integer> {

        private static final CountingIntegerSupplierSingleton INSTANCE = new CountingIntegerSupplierSingleton();


        private final AtomicInteger invocations = new AtomicInteger();

        @Override
        public Integer get() {
            invocations.incrementAndGet();
            return MAGIC_VALUE;
        }

        int invocations() {
            return invocations.get();
        }
    }

    static int invocations() {
        return CountingIntegerSupplierSingleton.INSTANCE.invocations();
    }

    static Stream<Arguments> computedConstantTypes() {
        MethodHandle mh;
        try {
            mh = MethodHandles.lookup()
                    .findVirtual(CountingIntegerSupplierSingleton.class, "get", MethodType.methodType(Integer.class))
                    .bindTo(CountingIntegerSupplierSingleton.INSTANCE);
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
        return Stream.of(
                Arguments.of(ComputedConstant.of(new CountingIntegerSupplier())),
                Arguments.of(ComputedConstant.of(CountingIntegerSupplier.class)),
                Arguments.of(ComputedConstant.of(Integer.class, mh))
        );
    }

}
