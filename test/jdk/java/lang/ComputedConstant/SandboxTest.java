/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
 * @summary Verify basic SandboxTest operations
 * @modules java.base/jdk.internal.misc
 * @modules java.base/jdk.internal.vm.annotation
 * @modules java.base/jdk.internal
 * @enablePreview
 * @run junit SandboxTest
 */

import jdk.internal.ValueBased;
import jdk.internal.misc.Unsafe;
import jdk.internal.vm.annotation.Stable;
import org.junit.jupiter.api.Test;

import java.util.NoSuchElementException;
import java.util.function.Supplier;

import static org.junit.jupiter.api.Assertions.*;

final class SandboxTest {

    // Holds a general value
    // This is basically a MethodHandle
    interface Value<V> extends Supplier<V> {
        @Override V get();
        V getVolatile();
        void cas(V o);
        boolean needsFreeze();

        static <V> Value<V> ofObject() {
            return new StandardValue<>();
        }

        static Value<Integer> ofInt() {
            return new IntValue();
        }
    }

    // Holds an internal byte state
    // This is basically a MethodHandle
    interface State extends Supplier<Byte> {
        byte UNBOUND = 0;
        byte NON_NULL = 1;
        byte NULL = 2;
        byte ERROR = 3;

        @Override Byte get();
        byte getAsByte();
        byte getVolatile();
        void cas(byte state);

        static State create() {
            return new StandardState();
        }
    }

    // This defines a Computed Constant.
    // It has no operations. Instead, it is pure data
    interface CC<V> {
        State state();
        Value<V> value();
        Supplier<? extends V> supplier();

        static <V> CC<V> of(State state,
                            Value<V> value,
                            Supplier<? extends V> supplier) {
            return new StandardCC<>(state, value, supplier);
        }

    }

    // Operators on CC

    static boolean isBound(CC<?> cc) {
        byte s = cc.state().getAsByte();
        return s == State.NON_NULL || s == State.NULL ||
                (s = cc.state().getVolatile()) == State.NON_NULL || s == State.NULL;
    }

    static <V> V get(CC<V> cc) {
        V v = cc.value().get();
        if (v != null) {
            return v;
        }
        if (cc.state().getAsByte() == State.NULL) {
            return null;
        }
        return Util.slowPath(cc, null, true);
    }

    // Tests

    @Test
    void ofObject() {
        CC<Long> cc = CC.of(State.create(), Value.ofObject(), () -> 42L);
        assertEquals(42L, (long) get(cc));
        assertEquals(42L, (long) get(cc));
    }

    @Test
    void ofIntegerAsObject() {
        CC<Integer> cc = CC.of(State.create(), Value.ofObject(), () -> 42);
        assertEquals(42, (int) get(cc));
        assertEquals(42, (int) get(cc));
    }

    @Test
    void ofIntegerAsInt() {
        CC<Integer> cc = CC.of(State.create(), Value.ofInt(), () -> 42);
        assertEquals(42, (int) get(cc));
        assertEquals(42, (int) get(cc));
    }

    // Implementations

    @ValueBased
    public record StandardCC<V>(State state,
                                Value<V> value,
                                Supplier<? extends V> supplier) implements SandboxTest.CC<V> {
    }

    private static class StandardValue<V> implements Value<V> {

        private static final long VALUE_OFFSET = Unsafe.getUnsafe().objectFieldOffset(StandardValue.class, "value");

        /**
         * This field holds a bound lazy value. If != null, a value is bound, otherwise the state
         * field needs to be consulted.
         * <p>
         * This field is accessed indirectly via Unsafe operations
         */
        @Stable
        private V value;

        public V get() {
            return value;
        }

        @SuppressWarnings("unchecked")
        public V getVolatile() {
            return (V) Unsafe.getUnsafe().getReferenceVolatile(this, VALUE_OFFSET);
        }

        public void cas(V o) {
            if (!Unsafe.getUnsafe().compareAndSetReference(this, VALUE_OFFSET, null, o)) {
                throw new InternalError("Value was not null: " + getVolatile());
            }
        }

        @Override
        public boolean needsFreeze() {
            return true;
        }
    }

    private static class IntValue implements Value<Integer> {

        private static final long VALUE_OFFSET = Unsafe.getUnsafe().objectFieldOffset(IntValue.class, "value");

        @Stable
        int value;

        @Override
        public Integer get() {
            int v = value;
            if (v != 0) {
                return v;
            }
            // We do not know what it is...
            return null;
        }

        @Override
        public Integer getVolatile() {
            int v = Unsafe.getUnsafe().getIntVolatile(this, VALUE_OFFSET);
            if (v != 0) {
                return v;
            }
            return null;
        }

        @Override
        public void cas(Integer o) {
            System.out.println("cas to " + o);
            int val = o;
            if (!Unsafe.getUnsafe().compareAndSetInt(this, VALUE_OFFSET, 0, val)) {
                throw new InternalError("Value was not zero: " + getVolatile());
            }
        }

        @Override
        public boolean needsFreeze() {
            return false;
        }
    }

    private static class StandardState implements State {

        private static final long STATE_OFFSET = Unsafe.getUnsafe().objectFieldOffset(StandardState.class, "state");
        /**
         * This non-final state field is used for flagging: 0) if the value is never bound
         * (State.UNBOUND) 1) Flagging if a non-null value is bound (State.BOUND_NON_NULL) 2) if the
         * value was actually evaluated to null (State.BOUND_NULL) 3) if the initial supplier threw
         * an exception (State.ERROR)
         * <p>
         * This field is accessed indirectly via Unsafe operations
         */
        @Stable
        private byte state;

        @Override
        public Byte get() {
            return state;
        }

        @Override
        public byte getAsByte() {
            return state;
        }

        @Override
        public byte getVolatile() {
            return Unsafe.getUnsafe().getByteVolatile(this, STATE_OFFSET);
        }

        @Override
        public void cas(byte state) {
            if (!Unsafe.getUnsafe().compareAndSetByte(this, STATE_OFFSET, (byte) 0, state)) {
                throw new InternalError("State was not zero: " + getVolatile());
            }
        }
    }

    private static final class Util {

        private Util() {}

        private static <V> V slowPath(CC<V> cc,
                                      V other,
                                      boolean rethrow) {

            synchronized (cc.state()) {
                // Under synchronization, visibility and atomicy is guaranteed for
                // the fields "value" and "state" as they only change within this block.
                return switch (cc.state().getAsByte()) {
                    case State.UNBOUND -> bindValue(cc, rethrow, other);
                    case State.NON_NULL -> cc.value().get();
                    case State.NULL -> null;
                    default -> {
                        if (rethrow) {
                            throw new NoSuchElementException("StandardCC: A previous provider threw an exception");
                        } else {
                            yield other;
                        }
                    }
                };
            }
        }

        static private <V> V bindValue(CC<V> cc, boolean rethrow, V other) {
            // setBindingVolatile(true);
            try {
                V v = cc.supplier().get();
                if (v == null) {
                    cc.state().cas(State.NULL);
                } else {
                    cc.value().cas(v);
                    // Insert a memory barrier for store/store operations
                    if (cc.value().needsFreeze()) {
                        // ConstantUtil.freeze();
                    }
                    cc.state().cas(State.NON_NULL);
                }
                return v;
            } catch (Throwable e) {
                cc.state().cas(State.ERROR);
                if (e instanceof Error err) {
                    // Always rethrow errors
                    throw err;
                }
                if (rethrow) {
                    throw new NoSuchElementException(e);
                }
                return other;
            } finally {
                // setBindingVolatile(false);
            }
        }
    }

}
