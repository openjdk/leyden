package jdk.internal.constant;

import jdk.internal.ValueBased;
import jdk.internal.misc.Unsafe;
import jdk.internal.vm.annotation.Stable;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.AbstractList;
import java.util.List;
import java.util.RandomAccess;
import java.util.function.IntFunction;

@ValueBased
public final class OnDemandComputedConstantList<V>
        extends AbstractList<ComputedConstant<V>>
        implements List<ComputedConstant<V>>,
        RandomAccess {

    private static final long ARRAY_BASE_OFFSET = Unsafe.getUnsafe().arrayBaseOffset(ComputedConstant[].class);
    private static final long ARRAY_INDEX_SCALE = Unsafe.getUnsafe().arrayIndexScale(ComputedConstant[].class);

    private final IntFunction<? extends V> provider;
    @Stable
    private final ComputedConstant<V>[] values;

    @SuppressWarnings("unchecked")
    private OnDemandComputedConstantList(int size, IntFunction<? extends V> provider) {
        this.provider = provider;
        this.values = (ComputedConstant<V>[]) new ComputedConstant<?>[size];
    }

    @Override
    public int size() {
        return values.length;
    }

    @SuppressWarnings("unchecked")
    @Override
    public ComputedConstant<V> get(int index) {
        // Try normal memory semantics first
        ComputedConstant<V> v = values[index];
        if (v != null) {
            return v;
        }
        // Another thread might have created the element
        v = (ComputedConstant<V>) Unsafe.getUnsafe().getReferenceVolatile(values, offset(index));
        if (v != null) {
            return v;
        }
        return slowPath(index);
    }

    @SuppressWarnings("unchecked")
    private ComputedConstant<V> slowPath(int index) {
        // Several candidates might be created ...
        ComputedConstant<V> v = ListElementComputedConstant.create(index, provider);
        // ... but only one will be used
        if (!Unsafe.getUnsafe().compareAndSetReference(values, offset(index), null, v)) {
            // Someone else created the element to use
            v = (ComputedConstant<V>) Unsafe.getUnsafe().getReferenceVolatile(values, offset(index));
        }
        return v;
    }

    public static <V> List<ComputedConstant<V>> create(int size, IntFunction<? extends V> provider) {
        return new OnDemandComputedConstantList<>(size, provider);
    }

    private static long offset(int index) {
        return ARRAY_BASE_OFFSET + index * ARRAY_INDEX_SCALE;
    }

}
