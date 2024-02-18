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
import jdk.internal.vm.annotation.ForceInline;
import jdk.internal.vm.annotation.Stable;

import java.util.NoSuchElementException;
import java.util.function.Supplier;

public abstract sealed class AbstractComputedConstant<V, P>
        implements ComputedConstant<V>
        permits ClassProvidedComputedConstant,  
        ListElementComputedConstant,
        MethodHandleComputedConstant,
        StandardComputedConstant,
        MapElementComputedConstant {

    // `Unsafe` is used rather than the supported API of `VarHandle` to allow
    // the use of `ComputedConstant` constructs early in the boot process.
    private static final long VALUE_OFFSET = ConstantUtil.offset(AbstractComputedConstant.class, "value");
    private static final long STATE_OFFSET = ConstantUtil.offset(AbstractComputedConstant.class, "state");
    private static final long BINDING_OFFSET = ConstantUtil.offset(AbstractComputedConstant.class, "binding");

    private final P provider;

    /**
     * This field holds a bound lazy value.
     * If != null, a value is bound, otherwise the state field needs to be consulted.
     * <p>
     * This field is accessed indirectly via Unsafe operations
     */
    @Stable
    private V value;

    /**
     * This non-final state field is used for flagging:
     *   0) if the value is never bound (State.UNBOUND)
     *   1) Flagging if a non-null value is bound (State.BOUND_NON_NULL)
     *   2) if the value was actually evaluated to null (State.BOUND_NULL)
     *   3) if the initial supplier threw an exception (State.ERROR)
     * <p>
     * This field is accessed indirectly via Unsafe operations
     */
    @Stable
    private byte state;

    /**
     * This variable indicates if we are trying to bind (1) or not (0).
     * <p>
     * This field is accessed indirectly via Unsafe operations
     */
    private byte binding;

    AbstractComputedConstant(P provider) {
        this.provider = provider;
    }

    @ForceInline
    @Override
    public final boolean isUnbound() {
        return stateVolatile() == State.UNBOUND.ordinalAsByte();
    }

    @ForceInline
    @Override
    public final boolean isBound() {
        // Try plain memory semantics first
        byte s;
        return value != null ||
                (s = stateVolatile()) == State.NON_NULL.ordinalAsByte()
                || s == State.NULL.ordinalAsByte();
    }

    @ForceInline
    @Override
    public final boolean isError() {
        return stateVolatile() == State.ERROR.ordinalAsByte();
    }

    @ForceInline
    @Override
    public final V get() {
        // Try plain memory semantics first
        V v = value;
        if (v != null) {
            return v;
        }
        if (state == State.NULL.ordinalAsByte()) {
            return null;
        }
        return slowPath(null, true);
    }

    @ForceInline
    @Override
    public final V orElse(V other) {
        // Try plain memory semantics first
        V v = value;
        if (v != null) {
            return v;
        }
        if (state == State.NULL.ordinalAsByte()) {
            return null;
        }
        return slowPath(other, false);
    }

    @ForceInline
    @Override
    public V orElseGet(Supplier<? extends V> supplier) {
        V v = orElse(null);
        if (state == State.ERROR.ordinalAsByte()) {
            return supplier.get();
        }
        return v;
    }

    @ForceInline
    @Override
    public final <X extends Throwable> V orElseThrow(Supplier<? extends X> exceptionSupplier) throws X {
        V v = orElse(null);
        if (state == State.ERROR.ordinalAsByte()) {
            throw exceptionSupplier.get();
        }
        return v;
    }

    @Override
    public final String toString() {
        String v = switch (stateVolatileAsEnum()) {
            case UNBOUND -> isBindingVolatile() ? ".binding" : ".unbound"; // Racy
            case NON_NULL -> "[" + valueVolatile().toString() + "]";
            case NULL -> "[null]";
            case ERROR -> ".error";
        };
        return toStringDescription() + v;
    }

    private synchronized V slowPath(V other,
                                    boolean rethrow) {

        if (binding != 0) {
            throw new StackOverflowError(toStringDescription() + ": Circular provider detected: " + provider);
        }

        // Under synchronization, visibility and atomicy is guaranteed for
        // the fields "value" and "state" as they only change within this block.
        return switch (stateAsEnum()) {
            case UNBOUND -> bindValue(rethrow, other);
            case NON_NULL -> value;
            case NULL -> null;
            case ERROR -> {
                if (rethrow) {
                    throw new NoSuchElementException(toStringDescription() + ": A previous provider threw an exception");
                } else {
                    yield other;
                }
            }
        };
    }

    private V bindValue(boolean rethrow, V other) {
        setBindingVolatile(true);
        try {
            V v = evaluate(provider);
            if (v == null) {
                casState(State.NULL);
            } else {
                casValue(v);
                // Insert a memory barrier for store/store operations
                ConstantUtil.freeze();
                casState(State.NON_NULL);
            }
            return v;
        } catch (Throwable e) {
            casState(State.ERROR);
            if (e instanceof Error err) {
                // Always rethrow errors
                throw err;
            }
            if (rethrow) {
                throw new NoSuchElementException(e);
            }
            return other;
        } finally {
            setBindingVolatile(false);
        }
    }

    abstract V evaluate(P provider);

    abstract String toStringDescription();


    // Accessors

    @SuppressWarnings("unchecked")
    private V valueVolatile() {
        return (V) Unsafe.getUnsafe().getReferenceVolatile(this, VALUE_OFFSET);
    }

    private byte stateVolatile() {
        return Unsafe.getUnsafe().getByteVolatile(this, STATE_OFFSET);
    }

    private State stateAsEnum() {
        return State.of(state);
    }

    private State stateVolatileAsEnum() {
        return State.of(stateVolatile());
    }

    private boolean isBindingVolatile() {
        return Unsafe.getUnsafe().getByteVolatile(this, BINDING_OFFSET) != 0;
    }

    private void casValue(Object o) {
        if (!Unsafe.getUnsafe().compareAndSetReference(this, VALUE_OFFSET, null, o)) {
            throw new InternalError("Value was not null: " + valueVolatile());
        }
    }

    private void casState(State state) {
        if (!Unsafe.getUnsafe().compareAndSetByte(this, STATE_OFFSET, (byte) 0, state.ordinalAsByte())) {
            throw new InternalError("State was not zero: " + stateVolatile());
        }
    }

    private void setBindingVolatile(boolean value) {
        Unsafe.getUnsafe().putByteVolatile(this, BINDING_OFFSET, (byte) (value ? 1 : 0));
    }

}
