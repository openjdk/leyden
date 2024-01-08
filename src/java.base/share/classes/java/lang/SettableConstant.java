package java.lang;

import jdk.internal.constant.IntSettableConstant;
import jdk.internal.constant.StandardSettableConstant;
import jdk.internal.javac.PreviewFeature;

import java.util.List;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.function.Supplier;

/**
 * An immutable value holder that is initialized at most once via a set operation, offering the performance
 * and safety benefits of final fields, while providing greater flexibility as to the timing of initialization.
 *
 * @param <V> the type of the value to be bound
 * @since 23
 */
@PreviewFeature(feature = PreviewFeature.Feature.COMPUTED_CONSTANTS)
public sealed interface SettableConstant<V>
        permits StandardSettableConstant, IntSettableConstant {

    /**
     * {@return {@code true} if a value is bound to this constant}
     */
    boolean isBound();

    /**
     * {@return the bound value of this constant. If no value is bound, throws a
     *          {@link NoSuchElementException}}
     * <p>
     * The most common usage is to get a memoized result, as in:
     * <p>
     * {@snippet lang = java:
     *    SettableConstant<V> constant = SettableConstant.of(Object.class, initialValue);
     *    // ...
     *    V value = constant.get();
     *    assertSame(initialValue, value); // Values are the same
     *}
     * <p>
     * This method is guaranteed to be lock-free.
     *
     * @throws NoSuchElementException if a value is not bound
     */
    V get();

    /**
     * {@return the bound value of this constant. If no value is bound returns the provided {@code other} value}
     * <p>
     * This method is guaranteed to be lock-free.
     *
     * @param other to use if no value is bound (can be null)
     */
    V orElse(V other);

    /**
     * If a value is bound, returns the value, otherwise returns the result
     * produced by the supplying function.
     * <p>
     * This method is guaranteed to be lock-free.
     *
     * @param supplier the supplying function that produces a value to be returned
     * @return the value, if bound, otherwise the result produced by the
     *         supplying function
     */
    V orElseGet(Supplier<? extends V> supplier);

    /**
     * {@return the bound value of this constant. If no value is bound throws an exception produced by
     *          invoking the provided {@code exceptionSupplier} function}
     * <p>
     * This method is guaranteed to be lock-free.
     *
     * @param <X>               the type of the exception that may be thrown
     * @param exceptionSupplier the supplying function that produces the exception to throw
     * @throws X                if a value is not bound
     */
    <X extends Throwable> V orElseThrow(Supplier<? extends X> exceptionSupplier) throws X;

    /**
     * Sets the bound value of this constant. If a value is already bound, throws a
     *          {@link IllegalStateException}}.
     * <p>
     * The most common usage is to set a memoized result, as in:
     * <p>
     * {@snippet lang = java:
     *    SettableConstant<Value> constant = SettableConstant.of();
     *    // ...
     *    Value initialValue = new Value();
     *    constant.set(initialValue);
     *    // ...
     *    Value value = constant.get();
     *    assertSame(initialValue, value); // Values are the same
     *}
     *
     * @param value the value to bind
     *
     * @throws IllegalStateException if a value is already bound
     */
    void set(V value);

    /**
     * If a value is not already bound, attempts to compute a value using the given
     * supplier and binds it, otherwise returns the bound value.
     * <p>
     * If the mapping function itself throws an (unchecked) exception, the
     * exception is rethrown, and no value is bound. The most
     * common usage is to construct a new object serving as an initial
     * mapped value or memoized result, as in:
     * <p>
     * {@snippet lang = java:
     *    SettableConstant<Value> constant = SettableConstant.of();
     *    // ...
     *    constant.computeIfUnbound(Value::new);
     *    // ...
     *    Value value = constant.get();
     *}
     *
     * @param supplier the supplier to bind a value
     * @return the bound (existing or computed) value
     */
    V computeIfUnbound(Supplier<? extends V> supplier);

    /**
     * Sets the bound value of this constant. If a value is already bound, does nothing.
     * <p>
     * The most common usage is to set a memoized result, as in:
     * <p>
     * {@snippet lang = java:
     *    SettableConstant<V> constant = SettableConstant.of();
     *    // ...
     *    V initialValue = new V();
     *    constant.setIfUnbound(initialValue);
     *    // ...
     *    V value = constant.get();
     *    assertSame(initialValue, value); // Values are the same
     *}
     *
     * @param value the value to bind
     */
    void setIfUnbound(V value);

    /**
     * {@return a new {@link SettableConstant } with no given pre-set value that stores its bound value
     *          as an Object}
     * <p>
     * {@snippet lang = java:
     *    SettableConstant<V> constant = SettableConstant.of();
     *    // ...
     *    V initialValue = new V();
     *    constant.set(initialValue);
     *    // ...
     *    V value = constant.get();
     *    assertSame(initialValue, value); // Values are the same
     *}
     *
     * @param <V>      the type of the value
     */
    static <V> SettableConstant<V> of() {
        return StandardSettableConstant.create();
    }

    /**
     * {@return a new {@link SettableConstant } with no given pre-set value that may optionally store its
     *          bound value using a field of the same type as the provided {@code storageType}}
     * <p>
     * The method can optionally use the provided {@code storageType} to return implementations that
     * are optimized with respect to storage and performance.
     * <p>
     * {@snippet lang = java:
     *    SettableConstant<V> constant = SettableConstant.of();
     *    // ...
     *    V initialValue = new V();
     *    constant.set(initialValue);
     *    // ...
     *    V value = constant.get();
     *    assertSame(initialValue, value); // Values are the same
     *}
     *
     * @param storageType a class literal representing an optional storage type of the bound value
     * @param <V>         the type of the value
     */
    @SuppressWarnings("unchecked")
    static <V> SettableConstant<V> of(Class<? super V> storageType) {
        Objects.requireNonNull(storageType);
        if (storageType.equals(Integer.class) || storageType.equals(int.class)) {
            return (SettableConstant<V>) IntSettableConstant.create();
        }
        return StandardSettableConstant.create();
    }

    /**
     * {@return a new {@link SettableConstant } with the given pre-bound {@code value}}
     * <p>
     * {@snippet lang = java:
     *     class DemoPreset {
     *
     *         private static final SettableConstant<Foo> FOO = SettableConstant.of(Foo.class, new Foo());
     *
     *         public Foo theFoo() {
     *             // Foo is obtained here
     *             return FOO.get();
     *         }
     *     }
     *}
     *
     * @param <V>         the type of the value
     * @param storageType a class literal representing an optional storage type of the bound value
     * @param value       to bind (can be null)
     */
    static <V> SettableConstant<V> of(Class<? super V> storageType, V value) {
        Objects.requireNonNull(storageType);
        SettableConstant<V> constant = of(storageType);
        constant.set(value);
        return constant;
    }

    /**
     * {@return a new List of {@link SettableConstant } elements with the provided
     * {@code size}}
     * <p>
     * The List and its elements are eligible for constant folding optimizations by the JVM.
     * <p>
     * Below, an example of how to cache values in a list is shown:
     * {@snippet lang = java:
     *     class DemoList {
     *
     *         private static final List<SettableConstant<Long>> CONSTANTS =
     *                 SettableConstant.of(long.class, 32);
     *          static {
     *             // Compute values in the CONSTANTS list
     *          }
     *
     *         public long constant(int n) {
     *             return CONSTANTS.get(n);
     *         }
     *     }
     *}
     * @apiNote The list is free to return <a href="{@docRoot}/java.base/java/lang/doc-files/ValueBased.html">value-based</a>
     *          ComputedConstant elements that has no valid identity.
     *
     * @param <V>         the type of the values
     * @param storageType a class literal representing an optional storage type of the bound value
     * @param size        the size of the List
     */
    static <V> List<SettableConstant<V>> list(Class<? super V> storageType, int size) {
        Objects.requireNonNull(storageType);
        if (size < 0) {
            throw new IllegalArgumentException();
        }
        throw new UnsupportedOperationException();
    }

}