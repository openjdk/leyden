package jdk.internal.constant;

import jdk.internal.misc.Unsafe;
import jdk.internal.vm.annotation.Stable;

import java.util.AbstractList;
import java.util.List;
import java.util.RandomAccess;
import java.util.function.IntFunction;

// Unfortunately, `AbstractList` declares `protected transient int modCount = 0;` preventing us
// from annotating a `List` `@ValueBased`
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

    @Override
    public ComputedConstant<V> get(int index) {
        // Try normal memory semantics first
        ComputedConstant<V> v = values[index];
        if (v != null) {
            return v;
        }
        return slowPath(index);
    }

    private ComputedConstant<V> slowPath(int index) {
        // Another thread might have created the element
        ComputedConstant<V> v = elementVolatile(index);
        if (v != null) {
            return v;
        }

        // Several candidates might be created ...
        v = ListElementComputedConstant.create(index, provider);
        // ... but only one will be selected
        if (!casElement(index, v)) {
            // Someone else created the selected element
            v = elementVolatile(index);
        }
        return v;
    }

    // Accessors

    @SuppressWarnings("unchecked")
    private ComputedConstant<V> elementVolatile(int index) {
        return (ComputedConstant<V>) Unsafe.getUnsafe().getReferenceVolatile(values, offset(index));
    }

    private boolean casElement(int index, Object o) {
        return Unsafe.getUnsafe().compareAndSetReference(values, offset(index), null, o);
    }

    private static long offset(int index) {
        return ARRAY_BASE_OFFSET + index * ARRAY_INDEX_SCALE;
    }

    // Factory

    public static <V> List<ComputedConstant<V>> create(int size, IntFunction<? extends V> provider) {
        return new OnDemandComputedConstantList<>(size, provider);
    }

}
