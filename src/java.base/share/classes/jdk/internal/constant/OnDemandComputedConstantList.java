package jdk.internal.constant;

import jdk.internal.ValueBased;
import jdk.internal.vm.annotation.Stable;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.AbstractList;
import java.util.List;
import java.util.function.IntFunction;

@ValueBased
public class OnDemandComputedConstantList<V>
        extends AbstractList<ComputedConstant<V>>
        implements List<ComputedConstant<V>> {

    private static final VarHandle ARRAY_HANDLE = MethodHandles.arrayElementVarHandle(ComputedConstant[].class);

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
        ComputedConstant<V> v = values[index];
        if (v != null) {
            return v;
        }
        v = ListElementComputedConstant.create(index, provider);
        if (!ARRAY_HANDLE.compareAndSet(values, index, null, v)) {
            v = (ComputedConstant<V>) ARRAY_HANDLE.getVolatile(values, index);
        }
        return v;
    }

    public static <V> List<ComputedConstant<V>> create(int size, IntFunction<? extends V> provider) {
        return new OnDemandComputedConstantList<>(size, provider);
    }

}
