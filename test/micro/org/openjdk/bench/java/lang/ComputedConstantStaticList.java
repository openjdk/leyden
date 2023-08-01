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

import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.IntFunction;
import java.util.function.Supplier;
import java.util.stream.IntStream;

@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Benchmark)
@Warmup(iterations = 5, time = 1)
@Measurement(iterations = 5, time = 1)
@Fork(value=3, jvmArgsAppend = "--enable-preview")
public class ComputedConstantStaticList {

    private static final int SIZE = 10;
    private static final int POS = SIZE / 2;
    private static final IntFunction<Integer> MAPPER = i -> i;
    private static final IntFunction<Integer> NULL_MAPPER = i -> null;

    public static final List<ComputedConstant<Integer>> LIST_OF_CONSTANTS = ComputedConstant.of(SIZE, MAPPER);
    public static final List<ComputedConstant<Integer>> LIST_OF_CONSTANTS_NULL = ComputedConstant.of(SIZE, NULL_MAPPER);
    public static final List<DoubleChecked<Integer>> DC = IntStream.range(0, SIZE)
            .mapToObj(i -> new DoubleChecked<>(i, MAPPER))
            .toList();

    @Benchmark
    public void constant(Blackhole bh) {
        bh.consume(LIST_OF_CONSTANTS.get(POS).get());
    }

    @Benchmark
    public void constantNull(Blackhole bh) {
        bh.consume(LIST_OF_CONSTANTS_NULL.get(POS).get());
    }

    @Benchmark
    public void staticHolder(Blackhole bh) {
        class Lazy {
            private static final int[] INT = IntStream.range(0, SIZE).toArray();
        }
        bh.consume(Lazy.INT[POS]);
    }

    @Benchmark
    public void doubleChecked(Blackhole bh) {
        bh.consume(DC.get(POS).get());
    }

    private static final class DoubleChecked<T> implements Supplier<T> {

        private IntFunction<? extends T> supplier;
        private final int index;
        private volatile T value;


        public DoubleChecked(int index, IntFunction<? extends T> supplier) {
            this.supplier = supplier;
            this.index = index;
        }

        @Override
        public T get() {
            T v = value;
            if (v == null) {
                synchronized (this) {
                    v = value;
                    if (v == null) {
                        v = supplier.apply(index);
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
