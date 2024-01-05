package jdk.internal.constant;

import jdk.internal.misc.Unsafe;
import jdk.internal.vm.annotation.ForceInline;
import jdk.internal.vm.annotation.Stable;

import java.util.NoSuchElementException;
import java.util.function.Function;
import java.util.function.Supplier;

// Todo: Investigate performance for value == 0
public final class IntSettableConstant
        implements SettableConstant<Integer> {

    private static final long VALUE_OFFSET = ConstantUtil.offset(IntSettableConstant.class, "value");
    private static final long STATE_OFFSET = ConstantUtil.offset(IntSettableConstant.class, "state");

    /**
     * This field holds a bound lazy value.
     * If != null, a value is bound, otherwise the state field needs to be consulted.
     * <p>
     * This field is accessed indirectly via Unsafe operations
     */
    @Stable
    private int value;

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

    private IntSettableConstant() {
    }

    @ForceInline
    @Override
    public boolean isBound() {
        // Try plain memory semantics first
        return value != 0 ||
                stateVolatile() != State.UNBOUND.ordinalAsByte();
    }

    @ForceInline
    @Override
    public Integer get() {
        return getOrApplyIfUnbound("No bound value", c -> {
            throw new NoSuchElementException(c);
        });
    }

    @ForceInline
    @Override
    public Integer orElse(Integer other) {
        return getOrApplyIfUnbound(other, Function.identity());
    }

    @ForceInline
    @Override
    public Integer orElseGet(Supplier<? extends Integer> supplier) {
        return getOrApplyIfUnbound(supplier, Supplier::get);
    }

    @ForceInline
    @Override
    public <X extends Throwable> Integer orElseThrow(Supplier<? extends X> exceptionSupplier) throws X {
        return getOrApplyIfUnbound(exceptionSupplier, es -> {
            // Trick to avoid handling exceptions in the lambda
            ConstantUtil.sneakyThrow(es.get());
            // Not reached
            return null;
        });
    }

    @ForceInline
    @Override
    public synchronized void set(Integer value) {
        if (state != State.UNBOUND.ordinalAsByte()) {
            throw new IllegalStateException("Value already bound: " + get());
        }
        set0(value);
    }

    @Override
    public Integer computeIfUnbound(Supplier<? extends Integer> supplier) {
        return getOrApplyIfUnbound(supplier, this::computeIfUnbound0);
    }

    @Override
    public synchronized void setIfUnbound(Integer value) {
        if (state == State.UNBOUND.ordinalAsByte()) {
            set0(value);
        }
    }

    @Override
    public String toString() {
        String v = switch (stateVolatileAsEnum()) {
            case UNBOUND -> ".unbound";
            case NON_NULL -> "[" + valueVolatile() + "]";
            case NULL -> "[null]";
            case ERROR -> throw new InternalError("Should not reach here");
        };
        return "StandardConstant" + v;
    }

    private <C> Integer getOrApplyIfUnbound(C unboundCarrier,
                                            Function<C, Integer> unboundExtractor) {
        // Try plain memory semantics first
        int v = value;
        if (v != 0) {
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

    private synchronized Integer computeIfUnbound0(Supplier<? extends Integer> supplier) {
        return switch (stateAsEnum()) {
            case UNBOUND -> {
                Integer newValue = supplier.get();
                set0(newValue);
                yield newValue;
            }
            case NON_NULL -> value;
            case NULL -> null;
            case ERROR -> throw new InternalError("Should not reach here");
        };
    }

    private void set0(Integer value) {
        if (value == null) {
            casState(State.NULL);
        } else {
            casValue(value);
            casState(State.NON_NULL);
        }
    }

    public static SettableConstant<Integer> create() {
        return new IntSettableConstant();
    }

    // Accessors
    private int valueVolatile() {
        return Unsafe.getUnsafe().getIntVolatile(this, VALUE_OFFSET);
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

    private void casValue(int value) {
         if (!Unsafe.getUnsafe().compareAndSetInt(this, VALUE_OFFSET, 0, value)) {
             throw new InternalError("Value was not zero: " + stateVolatile());
        };
    }

}
