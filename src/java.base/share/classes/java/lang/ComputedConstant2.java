package java.lang;

import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.function.Supplier;

/**
 * A
 * @param <V> type
 */
public interface ComputedConstant2<V> {

    /**
     * {@return {@code true} if no attempt has been made to bind a value to this constant}
     */
    boolean isUnbound();

    /**
     * {@return {@code true} if a value is bound to this constant}
     */
    boolean isBound();

    /**
     * {@return {@code true} if an attempt was made to bind a value but
     * a value could not be bound to this constant}
     */
    boolean isError();

    /**
     * {@return the bound value of this computed constant. If no value is bound, atomically attempts
     * to compute and record a bound value using the pre-set <em> {@linkplain ComputedConstant#of(Supplier) provider}</em>}
     * <p>
     * If the provider returns {@code null}, {@code null} is bound and returned.
     * If the provider throws an (unchecked) exception, the exception is wrapped into
     * a {@link NoSuchElementException} which is thrown, and no value is bound.  If an Error
     * is thrown by the provider, the Error is relayed to the caller.  If an Exception
     * or an Error is thrown by the provider, no further attempt is made to bind the value and all
     * subsequent invocations of this method will throw a new {@link NoSuchElementException}.
     * <p>
     * The most common usage is to construct a new object serving as a memoized result, as in:
     * <p>
     * {@snippet lang = java:
     *    ComputedConstant<V> constant = ComputedConstant.of(Value::new);
     *    // ...
     *    V value = constant.get();
     *    assertNotNull(value); // Value is non-null
     *}
     * <p>
     * If a thread calls this method while being bound by another thread, the current thread will be suspended until
     * the binding completes (successfully or not).  Otherwise, this method is guaranteed to be lock-free.
     *
     * @throws NoSuchElementException if a value cannot be bound
     * @throws StackOverflowError     if a circular dependency is detected (i.e. the provider calls itself
     *                                directly or indirectly in the same thread).
     * @throws Error                  if the provider throws an Error
     */
    V get();

    /**
     * {@return the bound value of this computed constant.  If no value is bound, atomically attempts
     * to compute and record a bound value using the pre-set <em>{@linkplain ComputedConstant#of(Supplier) provider}</em>
     * or, if the provider throws an unchecked exception, returns the provided {@code other} value}
     * <p>
     * If a thread calls this method while being bound by another thread, the current thread will be suspended until
     * the binding completes (successfully or not).  Otherwise, this method is guaranteed to be lock-free.
     *
     * @param other to use if no value neither is bound nor can be bound (can be null)
     * @throws StackOverflowError     if a circular dependency is detected (i.e. the provider calls itself
     *                                directly or indirectly in the same thread).
     * @throws Error                  if the provider throws an Error
     */
    V orElse(V other);

    /**
     * {@return the bound value of this computed constant. If no value is bound, atomically attempts
     * to compute and record a bound value using the pre-set <em>{@linkplain ComputedConstant#of(Supplier) provider}</em>
     * or, if the provider throws an unchecked exception, throws an exception produced by invoking the
     * provided {@code exceptionSupplier} function}
     * <p>
     * If a thread calls this method while being bound by another thread, the current thread will be suspended until
     * the binding completes (successfully or not).  Otherwise, this method is guaranteed to be lock-free.
     *
     * @param <X>               the type of the exception that may be thrown
     * @param exceptionSupplier the supplying function that produces the exception to throw
     * @throws X                if a value cannot be bound.
     * @throws Error            if the provider throws an Error
     */
    <X extends Throwable> V orElseThrow(Supplier<? extends X> exceptionSupplier) throws X;


    /**
     * Sets
     * @param constant c
     * @param value v
     * @param <V> type
     */
    static <V> void set(ComputedConstant2<V> constant, V value) {
        Objects.requireNonNull(constant);
    }

    /**
     * Computes
     * @param constant c
     * @param supplier s
     * @return v
     * @param <V> type
     */
    static <V> V computeIfUnbound(ComputedConstant2<V> constant,
                                  Supplier<? extends V> supplier) {
        Objects.requireNonNull(constant);
        Objects.requireNonNull(supplier);
        throw new UnsupportedOperationException();
    }

    // Factories

    /**
     * {@return a }
     * @param <V> type
     */
    static <V> ComputedConstant2<V> of() {
        throw new UnsupportedOperationException();
    }

    /**
     * {@return a }
     * @param type t
     * @param <V> type
     */
    static <V> ComputedConstant2<V> of(Class<? super V> type) {
        Objects.requireNonNull(type);
        throw new UnsupportedOperationException();
    }

}
