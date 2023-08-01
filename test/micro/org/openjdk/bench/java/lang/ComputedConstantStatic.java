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

package org.openjdk.bench.java.lang;

import org.openjdk.jmh.annotations.Benchmark;
import org.openjdk.jmh.annotations.BenchmarkMode;
import org.openjdk.jmh.annotations.Fork;
import org.openjdk.jmh.annotations.Measurement;
import org.openjdk.jmh.annotations.Mode;
import org.openjdk.jmh.annotations.OutputTimeUnit;
import org.openjdk.jmh.annotations.Scope;
import org.openjdk.jmh.annotations.State;
import org.openjdk.jmh.annotations.Warmup;
import org.openjdk.jmh.infra.Blackhole;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Benchmark)
@Warmup(iterations = 5, time = 1)
@Measurement(iterations = 5, time = 1)
@Fork(value=3, jvmArgsAppend = "--enable-preview")
public class ComputedConstantStatic {

    private static final Supplier<Integer> SUPPLIER = () -> 2 << 16;
    private static final Supplier<Integer> NULL_SUPPLIER = () -> null;

    public static final Supplier<Integer> CONSTANT = ComputedConstant.of(SUPPLIER);
    public static final Supplier<Integer> CONSTANT_NULL = ComputedConstant.of(NULL_SUPPLIER);
    public static final Supplier<Integer> DC = new DoubleChecked<>(SUPPLIER);

    private int value;

    private static VarHandle valueHandle() {
        try {
            return MethodHandles.lookup()
                    .findVarHandle(ComputedConstantStatic.class, "value", int.class);
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    private static final VarHandle VALUE_HANDLE = valueHandle();
    private static final ComputedConstant<VarHandle> CONSTANT_VALUE_HANDLE = ComputedConstant.of(ComputedConstantStatic::valueHandle);

    @State(Scope.Thread)
    public static class MyState {
        public int n = 10;
    }

    @Benchmark
    public void constant(Blackhole bh) {
        bh.consume(CONSTANT.get());
    }

    @Benchmark
    public void constantNull(Blackhole bh) {
        bh.consume(CONSTANT_NULL.get());
    }

    @Benchmark
    public void staticHolder(Blackhole bh) {
        class Lazy {
            private static final int INT = SUPPLIER.get();
        }
        bh.consume(Lazy.INT);
    }

    @Benchmark
    public void doubleChecked(Blackhole bh) {
        bh.consume(DC.get());
    }

    @Benchmark
    public void methodHandle(Blackhole bh) {
        bh.consume((int) VALUE_HANDLE.get(this));
    }

    @Benchmark
    public void methodHandleConstant(Blackhole bh) {
        bh.consume((int) CONSTANT_VALUE_HANDLE.get().get(this));
    }


    private static final class DoubleChecked<T> implements Supplier<T> {

        private Supplier<? extends T> supplier;

        private volatile T value;

        public DoubleChecked(Supplier<? extends T> supplier) {
            this.supplier = supplier;
        }

        @Override
        public T get() {
            T v = value;
            if (v == null) {
                synchronized (this) {
                    v = value;
                    if (v == null) {
                        v = supplier.get();
                        if (v == null) {
                            throw new NullPointerException();
                        }
                        value = v;
                        supplier = null;
                    }
                }
            }
            return v;
        }
    }

}
