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
import java.util.Objects;
import java.util.Set;
import java.util.function.Function;
import java.util.function.IntFunction;
import java.util.function.ToIntFunction;

public final class IndexedComputedConstantMap<K, V> extends AbstractMap<K, ComputedConstant<V>> {

    private static final long ARRAY_BASE_OFFSET = Unsafe.getUnsafe().arrayBaseOffset(ComputedConstant[].class);
    private static final long ARRAY_INDEX_SCALE = Unsafe.getUnsafe().arrayIndexScale(ComputedConstant[].class);

    private final IntFunction<? extends K> keyMapper;
    private final ToIntFunction<? super K> keyUnmapper;
    private final Function<? super K, ? extends V> provider;
    @Stable
    private final ComputedConstant<V>[] values;

    @SuppressWarnings("unchecked")
    public IndexedComputedConstantMap(int size,
                                      IntFunction<? extends K> keyMapper,
                                      ToIntFunction<? super K> keyUnmapper,
                                      Function<? super K, ? extends V> provider) {
        this.keyMapper = Objects.requireNonNull(keyMapper);
        this.keyUnmapper = Objects.requireNonNull(keyUnmapper);
        this.provider = Objects.requireNonNull(provider);
        this.values = (ComputedConstant<V>[]) new ComputedConstant<?>[size];
    }

    @Override
    public int size() {
        return values.length;
    }

    @Override
    public ComputedConstant<V> get(Object key) {
        return getOrDefault(key, null);
    }

    @Override
    public ComputedConstant<V> getOrDefault(Object key, ComputedConstant<V> defaultValue) {
        @SuppressWarnings("unchecked")
        K k = (K) key;
        int index = keyUnmapper.applyAsInt(k); // throws
        if (index < 0 || index > size()) {
            return defaultValue;
        }
        return value(k, index);
    }

    private ComputedConstant<V> value(K key, int index) {
        // Try normal memory semantics first
        ComputedConstant<V> v = values[index];
        if (v != null) {
            return v;
        }
        return slowValue(key, index);
    }

    @SuppressWarnings("unchecked")
    private ComputedConstant<V> slowValue(K key, int index) {
        // Plain read might have missed the value
        ComputedConstant<V> v = elementVolatile(index);
        if (v != null) {
            return v;
        }

        // Racy creation, will use the one that's uploaded first
        return (ComputedConstant<V>) caeElement(index, new MapElementComputedConstant<>(provider, key));
    }

    @Override
    public Set<Entry<K, ComputedConstant<V>>> entrySet() {
        return new AbstractSet<>() {
            @Override
            public Iterator<Entry<K, ComputedConstant<V>>> iterator() {
                return new Itr();
            }

            @Override
            public int size() {
                return values.length;
            }
        };
    }

    final class Itr implements Iterator<Entry<K, ComputedConstant<V>>> {
        int i;

        @Override
        public boolean hasNext() {
            return i < values.length;
        }

        @Override
        public Entry<K, ComputedConstant<V>> next() {
            int i = this.i++;
            var k = keyMapper.apply(i);
            return Map.entry(k, value(k, i));
        }
    }

    // Accessors

    @SuppressWarnings("unchecked")
    private ComputedConstant<V> elementVolatile(int index) {
        return (ComputedConstant<V>) Unsafe.getUnsafe().getReferenceVolatile(values, offset(index));
    }

    private Object caeElement(int index, Object o) {
        var w = Unsafe.getUnsafe().compareAndExchangeReference(values, offset(index), null, o);
        return w == null ? o : w;
    }

    private static long offset(int index) {
        return ARRAY_BASE_OFFSET + index * ARRAY_INDEX_SCALE;
    }

}
