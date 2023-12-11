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

import org.openjdk.jmh.annotations.*;
import org.openjdk.jmh.infra.Blackhole;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;

@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Benchmark)
@Warmup(iterations = 5, time = 1)
@Measurement(iterations = 5, time = 1)
@Fork(value=3, jvmArgsAppend = "--enable-preview")
public class ComputedConstantMapVsCC {

    private static final FibonacciArray fibonacciArray = new FibonacciArray(20);
    private static final FibonacciHashMap fibonacciHashMap = new FibonacciHashMap(20);
    private static final FibonacciConcurrentMap fibonacciConcurrentMap = new FibonacciConcurrentMap(20);
    private static final FibonacciCC fibonacciCC = new FibonacciCC(20);
    private static final FibonacciRecord fibonacciRecord = new FibonacciRecord(20);

    @Benchmark
    public void array(Blackhole bh) {
        bh.consume(fibonacciArray.number(10));
    }

    @Benchmark
    public void hashMap(Blackhole bh) {
        bh.consume(fibonacciHashMap.number(10));
    }

    @Benchmark
    public void concurrentMap(Blackhole bh) {
        bh.consume(fibonacciConcurrentMap.number(10));
    }

    @Benchmark
    public void ccUntrusted(Blackhole bh) {
        bh.consume(fibonacciCC.number(10));
    }

    @Fork(value = 3, jvmArgsAppend = {"--enable-preview", "-XX:+UnlockExperimentalVMOptions", "-XX:+TrustFinalNonStaticFields"})
    @Benchmark
    public void ccTrusted(Blackhole bh) {
        bh.consume(fibonacciCC.number(10));
    }

    @Benchmark
    public void ccRecord(Blackhole bh) {
        bh.consume(fibonacciRecord.number(10));
    }

    static class FibonacciHashMap {

        private final Map<Integer, Integer> map;

        public FibonacciHashMap(int upperBound) {
            map = new HashMap<>(upperBound);
        }

        public int number(int n) {
            if (n < 2) {
                return n;
            }
            Integer v = map.get(n);
            if (v != null) {
                return v;
            }
            int n1 = number(n - 1);
            int n2 = number(n - 2);
            int sum = n1 + n2;
            map.put(n, sum);
            return sum;
        }

    }

    static class FibonacciConcurrentMap {

        private final Map<Integer, Integer> map;

        public FibonacciConcurrentMap(int upperBound) {
            map = new ConcurrentHashMap<>(upperBound);
        }

        public int number(int n) {
            if (n < 2) {
                return n;
            }
            Integer v = map.get(n);
            if (v != null) {
                return v;
            }
            int n1 = number(n - 1);
            int n2 = number(n - 2);
            int sum = n1 + n2;
            map.put(n, sum);
            return sum;
        }

/*        public int number(int n) {
            return (n < 2)
                    ? n
                    : map.computeIfAbsent(n, nk -> number(nk - 1) + number(nk - 2));
        }*/

    }

    static final class FibonacciCC {

        private final List<ComputedConstant<Integer>> list;

        public FibonacciCC(int upperBound) {
            list = ComputedConstant.of(upperBound, this::number);
        }

        public int number(int n) {
            return (n < 2)
                    ? n
                    : list.get(n - 1).get() + list.get(n - 2).get();
        }

    }

    static class FibonacciArray {

        private final int[] array;

        public FibonacciArray(int upperBound) {
            array = new int[upperBound];
        }

        public int number(int n) {
            if (n < 2) {
                return n;
            }
            int v = array[n];
            if (v != 0) {
                return v;
            }
            int n1 = number(n - 1);
            int n2 = number(n - 2);
            int sum = n1 + n2;
            array[n] = sum;
            return sum;
        }

    }

    record FibonacciRecord(List<ComputedConstant<Integer>> list) {

        public FibonacciRecord(int upperBound) {
            this(ComputedConstant.of(upperBound, FibonacciRecord::fib));
        }

        // This will not use the cached values.
        private static int fib(int n) {
            if (n < 2) {
                return n;
            }
            int n1 = fib(n - 1);
            int n2 = fib(n - 2);
            int sum = n1 + n2;
            return sum;
        }

        public int number(int n) {
            return (n < 2)
                    ? n
                    : list.get(n - 1).get() + list.get(n - 2).get();
        }

    }

}
