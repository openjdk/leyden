package jdk.internal.constant;

import jdk.internal.misc.Unsafe;
import jdk.internal.vm.annotation.ForceInline;
import jdk.internal.vm.annotation.Stable;

import java.util.NoSuchElementException;
import java.util.function.Supplier;

public final class StandardConstant<V>
        implements Constant<V> {

    private static final long VALUE_OFFSET = ConstantUtil.offset(StandardConstant.class, "value");
    private static final long STATE_OFFSET = ConstantUtil.offset(StandardConstant.class, "state");
    private static final long SET_INVOKED_OFFSET = ConstantUtil.offset(StandardConstant.class, "setInvoked");

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
     * <p>
     * This field is accessed indirectly via Unsafe operations
     */
    @Stable
    private byte state;

    /**
     * This variable indicates if we ever have tried to bind a value
     * <p>
     * This field is accessed indirectly via Unsafe operations
     */
    @Stable
    private byte setInvoked;

    private StandardConstant() {
    }

    private StandardConstant(V value) {
        set(value);
    }

    @ForceInline
    @Override
    public boolean isUnbound() {
        return stateVolatile() == State.UNBOUND.ordinalAsByte();
    }

    @ForceInline
    @Override
    public boolean isBound() {
        // Try plain memory semantics first
        byte s;
        return value != null ||
                (s = stateVolatile()) == State.NON_NULL.ordinalAsByte()
                || s == State.NULL.ordinalAsByte();
    }

    @ForceInline
    @Override
    public V get() {
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
    public V orElse(V other) {
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
    public <X extends Throwable> V orElseThrow(Supplier<? extends X> exceptionSupplier) throws X {
        V v = orElse(null);
        if (v == null && stateVolatile() != State.NULL.ordinalAsByte()) {
            throw exceptionSupplier.get();
        }
        return v;
    }

    @Override
    public void set(V value) {
        if (!casSetInvokedVolatile()) {
            throw new IllegalStateException("Value already bound: " + get());
        }
        if (value == null) {
            casState(State.NULL);
        } else {
            casValue(value);
            ConstantUtil.freeze();
            casState(State.NON_NULL);
        }
    }

    private V slowPath(V other,
                       boolean throwIfUnbound) {
        return switch (stateVolatileAsEnum()) {
            case UNBOUND -> {
                if (throwIfUnbound) {
                    throw new NoSuchElementException();
                }
                yield other;
            }
            case NON_NULL -> valueVolatile();
            case NULL -> null;
            case ERROR -> throw new InternalError("Should not reach here");
        };
    }

    @Override
    public String toString() {
        String v = switch (stateVolatileAsEnum()) {
            case UNBOUND -> ".unbound";
            case NON_NULL -> "[" + valueVolatile().toString() + "]";
            case NULL -> "[null]";
            case ERROR -> throw new InternalError("Should not reach here");
        };
        return "StandardConstant" + v;
    }

    public static <V> Constant<V> create() {
        return new StandardConstant<>();
    }

    public static <V> Constant<V> create(V value) {
        return new StandardConstant<>(value);
    }

    // Accessors
    @SuppressWarnings("unchecked")
    private V valueVolatile() {
        return (V) Unsafe.getUnsafe().getReferenceVolatile(this, VALUE_OFFSET);
    }

    private byte stateVolatile() {
        return Unsafe.getUnsafe().getByteVolatile(this, STATE_OFFSET);
    }

    private State stateVolatileAsEnum() {
        return State.of(stateVolatile());
    }

    private void casState(State state) {
        if (!Unsafe.getUnsafe().compareAndSetByte(this, STATE_OFFSET, (byte) 0, state.ordinalAsByte())) {
            throw new InternalError("State was not zero: " + stateVolatile());
        }
    }

    private void casValue(V value) {
         if (!Unsafe.getUnsafe().compareAndSetReference(this, VALUE_OFFSET, null, value)) {
             throw new InternalError("Value was not zero: " + stateVolatile());
        };

    }

    private boolean casSetInvokedVolatile() {
        return Unsafe.getUnsafe().compareAndSetByte(this, SET_INVOKED_OFFSET, (byte) 0, (byte) 1);
    }

}
