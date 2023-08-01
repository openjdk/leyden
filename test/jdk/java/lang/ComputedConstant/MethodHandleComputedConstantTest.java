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
 * @run junit MethodHandleComputedConstantTest
 */

import org.junit.jupiter.api.*;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

import static org.junit.jupiter.api.Assertions.*;

final class MethodHandleComputedConstantTest {

    private MethodHandles.Lookup lookup;
    private MethodHandle handle;
    private CountingIntegerSupplier supplier;
    private ComputedConstant<Integer> constant;

    public Integer value() {
        return supplier.get();
    }

    @BeforeEach
    void setup() throws NoSuchMethodException, IllegalAccessException {
        supplier = new CountingIntegerSupplier();
        lookup = MethodHandles.lookup();
        handle = lookup.findVirtual(MethodHandleComputedConstantTest.class, "value", MethodType.methodType(Integer.class));
        handle = MethodHandles.insertArguments(handle, 0, this);
        constant = ComputedConstant.of(Integer.class, handle);
    }

    @Test
    void get() {
        Integer val = constant.get();
        assertEquals(CountingIntegerSupplier.MAGIC_VALUE, val);
        assertEquals(1, supplier.invocations());
        Integer val2 = constant.get();
        assertEquals(CountingIntegerSupplier.MAGIC_VALUE, val2);
        assertEquals(1, supplier.invocations());
    }

    @Test
    void nullSupplier() {
        assertThrows(NullPointerException.class, () ->
                ComputedConstant.of(Integer.class, null)
        );
    }

    @Test
    void wrongSupplier() {
        assertThrows(IllegalArgumentException.class, () ->
                ComputedConstant.of(String.class, handle)
        );
    }

    @Test
    void moreSpecificType() throws NoSuchMethodException, IllegalAccessException {
        var mh = lookup.findConstructor(Sub.class, MethodType.methodType(void.class));

        ComputedConstant<Super> c0 = ComputedConstant.of(Sub.class, mh);
        ComputedConstant<Sub> c1 = ComputedConstant.of(Sub.class, mh);
        ComputedConstant<Super> c2 = ComputedConstant.of(Super.class, mh);
        // Should not work as Super does not implement Sub
        // ComputedConstant<Sub> c4 = ComputedConstant.ofMh(Super.class, mh);

        var v0 = c0.get();
        assertEquals(Sub.class, v0.getClass());
        var v1 = c1.get();
        assertEquals(Sub.class, v1.getClass());
        var v2 = c2.get();
        assertEquals(Sub.class, v2.getClass());

        assertThrows(IllegalArgumentException.class, () ->
                // mh does not return an Integer
                ComputedConstant.of(Integer.class, mh)
        );
    }


    private static final class CountingIntegerSupplier implements Supplier<Integer> {
        static final int MAGIC_VALUE = 42;
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

    static class Super {}
    static final class Sub extends Super {}

}
