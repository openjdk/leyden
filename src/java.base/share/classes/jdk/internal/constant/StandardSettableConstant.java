package jdk.internal.constant;

import jdk.internal.misc.Unsafe;
import jdk.internal.vm.annotation.ForceInline;
import jdk.internal.vm.annotation.Stable;

import java.util.NoSuchElementException;
import java.util.function.Function;
import java.util.function.Supplier;

public final class StandardSettableConstant<V>
        implements SettableConstant<V> {

    private static final long VALUE_OFFSET = ConstantUtil.offset(StandardSettableConstant.class, "value");
    private static final long STATE_OFFSET = ConstantUtil.offset(StandardSettableConstant.class, "state");

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

    private StandardSettableConstant() {
    }

    private StandardSettableConstant(V value) {
        set(value);
    }

    @ForceInline
    @Override
    public boolean isBound() {
        // Try plain memory semantics first
        return value != null ||
                stateVolatile() != State.UNBOUND.ordinalAsByte();
    }

    @ForceInline
    @Override
    public V get() {
        return getOrApplyIfUnbound("No bound value", c -> {
            throw new NoSuchElementException(c);
        });
    }

    @ForceInline
    @Override
    public V orElse(V other) {
        return getOrApplyIfUnbound(other, Function.identity());
    }

    @ForceInline
    @Override
    public V orElseGet(Supplier<? extends V> supplier) {
        return getOrApplyIfUnbound(supplier, Supplier::get);
    }

    @ForceInline
    @Override
    public <X extends Throwable> V orElseThrow(Supplier<? extends X> exceptionSupplier) throws X {
        return getOrApplyIfUnbound(exceptionSupplier, es -> {
            // Trick to avoid handling exceptions in the lambda
            ConstantUtil.sneakyThrow(es.get());
            // Not reached
            return null;
        });
    }

    @ForceInline
    @Override
    public synchronized void set(V value) {
        if (state != State.UNBOUND.ordinalAsByte()) {
            throw new IllegalStateException("Value already bound: " + get());
        }
        set0(value);
    }

    @Override
    public V computeIfUnbound(Supplier<? extends V> supplier) {
        return getOrApplyIfUnbound(supplier, this::computeIfUnbound0);
    }


    @Override
    public synchronized void setIfUnbound(V value) {
        if (state == State.UNBOUND.ordinalAsByte()) {
            set0(value);
        }
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

    private <C> V getOrApplyIfUnbound(C unboundCarrier,
                                      Function<C, V> unboundExtractor) {
        // Try plain memory semantics first
        V v = value;
        if (v != null) {
            return v;
        }
        if (this.state == State.NULL.ordinalAsByte()) {
            return null;
        }
        return switch (stateVolatileAsEnum()) {
            case UNBOUND -> unboundExtractor.apply(unboundCarrier);
            case NON_NULL -> value;
            case NULL -> null;
            case ERROR -> throw new InternalError("Should not reach here");
        };
    }

    private synchronized V computeIfUnbound0(Supplier<? extends V> supplier) {
        return switch (stateAsEnum()) {
            case UNBOUND -> {
                V newValue = supplier.get();
                set0(newValue);
                yield newValue;
            }
            case NON_NULL -> value;
            case NULL -> null;
            case ERROR -> throw new InternalError("Should not reach here");
        };
    }

    private void set0(V value) {
        if (value == null) {
            casState(State.NULL);
        } else {
            casValue(value);
            ConstantUtil.freeze();
            casState(State.NON_NULL);
        }
    }

    public static <V> SettableConstant<V> create() {
        return new StandardSettableConstant<>();
    }

    public static <V> SettableConstant<V> create(V value) {
        return new StandardSettableConstant<>(value);
    }

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

}
