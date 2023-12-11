package java.lang;

import jdk.internal.constant.AbstractComputedConstant;
import jdk.internal.constant.ListElementComputedConstant;
import jdk.internal.constant.MethodHandleComputedConstant;
import jdk.internal.constant.OnDemandComputedConstantList;
import jdk.internal.constant.StandardComputedConstant;
import jdk.internal.constant.StandardConstant;
import jdk.internal.javac.PreviewFeature;

import java.lang.invoke.MethodHandle;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.function.Function;
import java.util.function.IntFunction;
import java.util.function.Supplier;

/**
 * An immutable value holder that is initialized at most once via a set operation, offering the performance
 * and safety benefits of final fields, while providing greater flexibility as to the timing of initialization.
 *
 * @param <V> the type of the value to be bound
 * @since 23
 */
@PreviewFeature(feature = PreviewFeature.Feature.COMPUTED_CONSTANTS)
public sealed interface Constant<V>
        permits StandardConstant {

    /**
     * {@return {@code true} if no attempt has been made to bind a value to this constant}
     */
    boolean isUnbound();

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
     *    Constant<V> constant = Constant.of(initialValue);
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
     * {@return the bound value of this constant. If no value is bound throws an exception produced by
     *          invoking the provided {@code exceptionSupplier} function}
     * <p>
     * This method is guaranteed to be lock-free.
     *
     * @param <X>               the type of the exception that may be thrown
     * @param exceptionSupplier the supplying function that produces the exception to throw
     * @throws X                if a value cannot be bound.
     */
    <X extends Throwable> V orElseThrow(Supplier<? extends X> exceptionSupplier) throws X;


    /**
     * Sets the bound value of this constant. If a value is already bound, throws a
     *          {@link IllegalStateException}}
     * <p>
     * The most common usage is to set a memoized result, as in:
     * <p>
     * {@snippet lang = java:
     *    Constant<V> constant = Constant.of();
     *    // ...
     *    V initialValue = new V();
     *    constant.set(initialValue);
     *    // ...
     *    V value = constant.get();
     *    assertSame(initialValue, value); // Values are the same
     *}
     * <p>
     * This method is guaranteed to be lock-free.
     *
     * @param value the value to bind
     *
     * @throws IllegalStateException if a value is already bound
     */
    void set(V value);

    /**
     * Sets the bound value of this constant. If a value is already bound, does nothing
     * <p>
     * The most common usage is to set a memoized result, as in:
     * <p>
     * {@snippet lang = java:
     *    Constant<V> constant = Constant.of();
     *    // ...
     *    V initialValue = new V();
     *    constant.setOrDiscard(initialValue);
     *    // ...
     *    V value = constant.get();
     *    assertSame(initialValue, value); // Values are the same
     *}
     * <p>
     * This method is guaranteed to be lock-free.
     *
     * @param value the value to bind
     */
    void setOrDiscard(V value);

    /**
     * {@return a new {@link Constant } that will use this constant's bound value
     * and then apply the provided {@code mapper} as a new value}
     *
     * @param mapper to apply to this constant
     * @param <R>    the return type of the provided {@code mapper}
     */
    default <R> Constant<R> map(Function<? super V, ? extends R> mapper) {
        Objects.requireNonNull(mapper);
        return of(mapper.apply(this.get()));
    }

    /**
     * {@return a new {@link Constant } with no given pre-set value}
     * <p>
     * {@snippet lang = java:
     *    Constant<V> constant = Constant.of();
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
    static <V> Constant<V> of() {
        return StandardConstant.create();
    }
    /**
     * {@return a new {@link Constant } with the given pre-bound {@code value}}
     * <p>
     * {@snippet lang = java:
     *     class DemoPreset {
     *
     *         private static final Constant<Foo> FOO = Constant.of(new Foo());
     *
     *         public Foo theFoo() {
     *             // Foo is obtained here
     *             return FOO.get();
     *         }
     *     }
     *}
     *
     * @param <V>      the type of the value
     * @param value    to bind (can be null)
     */
    static <V> Constant<V> of(V value) {
        return StandardConstant.create(value);
    }

    /**
     * {@return a new List of {@link Constant } elements with the provided
     * {@code size}}
     * <p>
     * The List and its elements are eligible for constant folding optimizations by the JVM.
     * <p>
     * Below, an example of how to cache values in a list is shown:
     * {@snippet lang = java:
     *     class DemoList {
     *
     *         private static final List<Constant<Long>> CONSTANTS =
     *                 Constant.of(32);
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
     * @param <V>             the type of the values
     * @param size            the size of the List
         */
    static <V> List<Constant<V>> ofList(int size) {
        if (size < 0) {
            throw new IllegalArgumentException();
        }
        throw new UnsupportedOperationException();
    }

}