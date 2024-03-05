/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
package jdk.internal.constant;

import jdk.internal.misc.Unsafe;
import jdk.internal.vm.annotation.Stable;

import java.util.AbstractMap;
import java.util.AbstractSet;
import java.util.Iterator;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.Set;
import java.util.function.BiFunction;
import java.util.function.Function;

public final class OnDemandComputedConstantMap<K, V> extends AbstractMap<K, ComputedConstant<V>> {

    private static final long SALT32L = Long.MAX_VALUE; // TODO dummy; also investigate order
    private static final int EXPAND_FACTOR = 2;

    private static final Unsafe U = Unsafe.getUnsafe();

    private final Function<K, V> generator;
    @Stable
    private final Object[] table; // pairs of key, value
    private final int size; // number of pairs

    // keys array not trusted
    public OnDemandComputedConstantMap(Object[] keys, Function<K, V> generator) {
        this.generator = Objects.requireNonNull(generator);
        this.size = keys.length;

        int len = EXPAND_FACTOR * keys.length * 2;
        len = (len + 1) & ~1; // ensure table is even length
        Object[] table = new Object[len];

        for (Object key : keys) {
            @SuppressWarnings("unchecked")
            K k = Objects.requireNonNull((K) key);
            int idx = probe(k);
            if (idx >= 0) {
                throw new IllegalArgumentException("duplicate key: " + k);
            } else {
                int dest = -(idx + 1);
                table[dest] = k;
            }
        }

        U.storeFence(); // ensure keys are visible if table is visible
        this.table = table;
    }

    // returns index at which the probe key is present; or if absent,
    // (-i - 1) where i is location where element should be inserted.
    // Callers are relying on this method to perform an implicit nullcheck
    // of pk.
    private int probe(Object pk) {
        int idx = Math.floorMod(pk.hashCode(), table.length >> 1) << 1;
        while (true) {
            @SuppressWarnings("unchecked")
            K ek = (K)table[idx];
            if (ek == null) {
                return -idx - 1;
            } else if (pk.equals(ek)) {
                return idx;
            } else if ((idx += 2) == table.length) {
                idx = 0;
            }
        }
    }

    private ComputedConstant<V> value(int keyIndex) {
        @SuppressWarnings("unchecked")
        ComputedConstant<V> cc = (ComputedConstant<V>) table[keyIndex + 1];
        if (cc != null) {
            return cc;
        }

        return slowValue(keyIndex);
    }

    @SuppressWarnings("unchecked")
    private ComputedConstant<V> slowValue(int keyIndex) {
        ComputedConstant<V> cc = (ComputedConstant<V>) getTableItemVolatile(keyIndex + 1);
        if (cc != null) {
            return cc;
        }

        // racy, only use the one who uploaded first
        return (ComputedConstant<V>) caeTableItemVolatile(keyIndex, new MapElementComputedConstant<>(generator, (K) table[keyIndex]));
    }

    private Object getTableItemVolatile(int index) {
        return U.getReferenceVolatile(table, offset(index));
    }

    private Object caeTableItemVolatile(int index, Object o) {
        var w = U.compareAndExchangeReference(table, offset(index), null, o);
        return w == null ? o : w;
    }

    private static long offset(int index) {
        return Unsafe.ARRAY_OBJECT_BASE_OFFSET + (long) index * Unsafe.ARRAY_OBJECT_INDEX_SCALE;
    }

    @Override
    public boolean containsKey(Object o) {
        Objects.requireNonNull(o);
        return size > 0 && probe(o) >= 0;
    }

    @Override
    public int size() {
        return size;
    }

    @Override
    public boolean isEmpty() {
        return size == 0;
    }

    @Override
    public ComputedConstant<V> get(Object key) {
        if (size == 0) {
            return null;
        }
        int i = probe(key);
        if (i >= 0) {
            return value(i);
        } else {
            return null;
        }
    }

    @Override
    public Set<Entry<K, ComputedConstant<V>>> entrySet() {
        return new AbstractSet<>() {
            @Override
            public int size() {
                return size;
            }

            @Override
            public Iterator<Entry<K, ComputedConstant<V>>> iterator() {
                return new Itr();
            }
        };
    }

    final class Itr implements Iterator<Entry<K, ComputedConstant<V>>> {
        private int remaining;

        private int idx;

        Itr() {
            remaining = size;
            // pick an even starting index in the [0 .. table.length-1]
            // range randomly based on SALT32L
            idx = (int) ((SALT32L * (table.length >> 1)) >>> 32) << 1;
        }

        @Override
        public boolean hasNext() {
            return remaining > 0;
        }

        private int nextIndex() {
            int idx = this.idx;
                if ((idx += 2) >= table.length) {
                    idx = 0;
                }
            return this.idx = idx;
        }

        @Override
        public Map.Entry<K, ComputedConstant<V>> next() {
            if (remaining > 0) {
                int idx;
                while (table[idx = nextIndex()] == null) {}
                @SuppressWarnings("unchecked")
                Map.Entry<K, ComputedConstant<V>> e =
                        Map.entry((K)table[idx], value(idx));
                remaining--;
                return e;
            } else {
                throw new NoSuchElementException();
            }
        }
    }

    // Prevents modification
    public ComputedConstant<V> put(K key, ComputedConstant<V> value) { throw new UnsupportedOperationException(); }
    public ComputedConstant<V> remove(Object key) { throw new UnsupportedOperationException(); }
    public void putAll(Map<? extends K, ? extends ComputedConstant<V>> m) { throw new UnsupportedOperationException(); }
    public void clear() { throw new UnsupportedOperationException(); }
    public void replaceAll(BiFunction<? super K, ? super ComputedConstant<V>, ? extends ComputedConstant<V>> function) { throw new UnsupportedOperationException(); }
    public ComputedConstant<V> putIfAbsent(K key, ComputedConstant<V> value) { throw new UnsupportedOperationException(); }
    public boolean remove(Object key, Object value) { throw new UnsupportedOperationException(); }
    public boolean replace(K key, ComputedConstant<V> oldValue, ComputedConstant<V> newValue) { throw new UnsupportedOperationException(); }
    public ComputedConstant<V> replace(K key, ComputedConstant<V> value) { throw new UnsupportedOperationException(); }
    public ComputedConstant<V> computeIfAbsent(K key, Function<? super K, ? extends ComputedConstant<V>> mappingFunction) { throw new UnsupportedOperationException(); }
    public ComputedConstant<V> computeIfPresent(K key, BiFunction<? super K, ? super ComputedConstant<V>, ? extends ComputedConstant<V>> remappingFunction) { throw new UnsupportedOperationException(); }
    public ComputedConstant<V> compute(K key, BiFunction<? super K, ? super ComputedConstant<V>, ? extends ComputedConstant<V>> remappingFunction) { throw new UnsupportedOperationException(); }
    public ComputedConstant<V> merge(K key, ComputedConstant<V> value, BiFunction<? super ComputedConstant<V>, ? super ComputedConstant<V>, ? extends ComputedConstant<V>> remappingFunction) { throw new UnsupportedOperationException(); }
}
