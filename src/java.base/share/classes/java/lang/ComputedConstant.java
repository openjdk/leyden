package java.lang;

import jdk.internal.constant.AbstractComputedConstant;
import jdk.internal.constant.ListElementComputedConstant;
import jdk.internal.constant.MethodHandleComputedConstant;
import jdk.internal.constant.OnDemandComputedConstantList;
import jdk.internal.constant.StandardComputedConstant;
import jdk.internal.javac.PreviewFeature;

import java.lang.invoke.MethodHandle;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.function.Function;
import java.util.function.IntFunction;
import java.util.function.Supplier;

/**
 * An immutable value holder that is initialized at most once via a provider, offering the performance
 * and safety benefits of final fields, while providing greater flexibility as to the timing of initialization.
 * <p>
 * A computed constant can be queried after being created to provide a bound value,
 * for example when {@link ComputedConstant#get() get()} is invoked.
 * <p>
 * Once bound (or if it failed to bound), computed constant instances are guaranteed to
 * be lock free and are eligible for constant folding optimizations by the JVM.
 *
 * Providers of constant values are guaranteed to be invoked at most one time. In other words,
 * a provider can only run once and in the first-calling thread and so, there is no race across threads
 * which guarantees the at-most-once evaluation.  The life cycle of a computed constant is said
 * to be <em>monotonic</em> where it can go from the initial state of <em>unbound</em>
 * (when it is not associated with any value) eventually to the terminal state of <em>bound</em> (when it is
 * permanently associated with a fixed value). The value can be {@code null}.
 * <p>
 * This contrasts with {@link java.util.concurrent.atomic.AtomicReference } where any number of
 * updates can be done and where there is no simple way to atomically compute a value
 * (guaranteed to only be computed once) if missing. Computed constants also contrasts to
 * {@link java.util.concurrent.Future} where a value is computed in another thread.
 * <p>
 * The implementations are optimized for providing high average performance
 * for get operations over many invocations.
 *
 *  <h2 id="computed-constant-factories">Computed Constant Factories</h2>
 *
 * New instances of ComputedConstants can only be obtained via the following factory methods:
 * <ul>
 *     <li>Single Instances
 *         <ul>
 *             <li>{@linkplain ComputedConstant#of(Supplier)
 *             ComputedConstant.of(Supplier&lt;? extends V&gt; provider)}
 *             providing a single new ComputedConstant instance</li>
 *
 *             <li>{@linkplain ComputedConstant#of(Class, MethodHandle)
 *             ComputedConstant.of(Class&lt;? super V&gt; type; MethodHandle provider)}
 *             providing a single new ComputedConstant instance</li>
 *         </ul>
 *     </li>
 *     <li>Collections
 *         <ul>
 *             <li>{@linkplain ComputedConstant#of(int, IntFunction) ComputedConstant.of(int length, IntFunction&lt;? super V&gt; mappingProvider)}
 *             providing a new List of ComputedConstant elements</li>
 *         </ul>
 *     </li>
 * </ul>
 *
 * <h2 id="computed-constant">ComputedConstant</h2>
 *
 * {@code ComputedConstant} provides atomic evaluation using a <em>provider</em>:
 *
 * {@snippet lang = java:
 *     class DemoPreset {
 *
 *         private static final ComputedConstant<Foo> FOO = ComputedConstant.of(Foo::new); // provider = Foo::new
 *
 *         public Foo theFoo() {
 *             // Foo is constructed and recorded before the first invocation returns
 *             return FOO.get();
 *         }
 *     }
 *}
 * The performance of the {@code get()} method in the example above is on par with using an
 * inner/private class holding a lazily initialized variable but with no overhead imposed by
 * the extra holder class.  Such a holder class might implement a lazy value as follows:
 *
 {@snippet lang = java :
  *     class DemoHolder {
  *
  *         public Foo theBar() {
  *             class Holder {
  *                 private static final Foo FOO = new Foo();
  *             }
  *
  *             // Foo is lazily constructed and recorded here upon first invocation
  *             return Holder.FOO;
  *         }
  *     }
  *}
 *
 * Here is how a constant value can be computed in the background so that it may already be bound
 * when first requested from user code:
 * {@snippet lang = java:
 *     class DemoBackground {
 *
 *         private static final ComputedConstant<Foo> CONSTANT = ComputedConstant.of(Foo::new);
 *
 *         static {
 *             Thread.ofVirtual().start(CONSTANT::get);
 *         }
 *
 *         public static void main(String[] args) throws InterruptedException {
 *             Thread.sleep(1000);
 *             // CONSTANT is likely already pre-computed here by a background thread
 *             System.out.println(CONSTANT.get());
 *         }
 *     }
 *}
 *
 * {@code ComputedConstant<T>} implements {@code Supplier<T>} allowing simple
 * interoperability with legacy code and less specific type declaration
 * as shown in the following example:
 * {@snippet lang = java:
 *     class SupplierDemo {
 *
 *         // Eager Supplier of Foo
 *         private static final Supplier<Foo> EAGER_FOO = Foo::new;
 *
 *         // Turns an eager Supplier into a caching constant Supplier
 *         private static final Supplier<Foo> LAZILY_CACHED_FOO = ComputedConstant.of(EAGER_FOO);
 *
 *         public static void main(String[] args) {
 *             // Lazily compute the one-and-only `Foo`
 *             Foo theFoo = LAZILY_CACHED_FOO.get();
 *         }
 *     }
 *}
 *
 * <h2 id="list-of-computed-constant-elements">List of ComputedConstant Elements</h2>
 *
 * Lists of ComputedConstant elements can also be obtained via {@code ComputedConstant} factory
 * methods in a similar way as for {@code ComputedConstant} instances but with an extra initial arity, indicating
 * the desired size of the List:
 * {@snippet lang = java:
 *     class DemoList {
 *
 *         // 1. Declare a List of ComputedConstant elements of size 32
 *         private static final List<ComputedConstant<Long>> VALUE_PO2_CACHE =
 *             ComputedConstant.of(32, index -> 1L << index); // mappingProvider = index -> 1L << index
 *
 *         public long powerOfTwo(int n) {
 *             // 2. The n:th slot is computed and bound here before
 *             //    the first call of get(n) returns. The other elements are not affected.
 *             // 3. Using an n outside the list will throw an IndexOutOfBoundsException
 *             return VALUE_PO2_CACHE.get(n).get();
 *         }
 *     }
 *}
 * As can be seen above, a List factory takes an {@link java.util.function.IntFunction} rather
 * than a {@link java.util.function.Supplier }, allowing custom values to be
 * computed and bound for elements in the list depending on the current index being used.
 *
 * <h2 id="state">States</h2>
 * {@code ComputedConstant} instances maintain an internal state described as follows:
 *
 * <ul>
 *     <li><em>Unbound</em>
 *     <ul>
 *         <li>Indicates no value is bound (transient state)</li>
 *         <li>Can move to "Binding"</li>
 *         <li>This state can be detected using the method {@link ComputedConstant#isUnbound()}</li>
 *     </ul>
 *     </li>
 *     <li><em>Binding</em>
 *     <ul>
 *         <li>Indicates an attempt to bind a value is in progress (transient state)</li>
 *         <li>Can move to "Bound" or "Error"</li>
 *         <li>This state can be unreliably assumed using a combination of the "is" predicates (where all are false)</li>
 *     </ul>
 *     </li>
 *     <li><em>Bound</em>
 *     <ul>
 *         <li>Indicates a value is successfully bound (final state)</li>
 *         <li>Cannot move
 *         <li>This state can be detected using the method {@link ComputedConstant#isBound()}</li>
 *     </ul>
 *     </li>
 *     <li><em>Error</em>
 *     <ul>
 *         <li>Indicates an error when trying to bind a value (final state)</li>
 *         <li>Cannot move
 *         <li>This state can be detected using the method {@link ComputedConstant#isError()}</li>
 *     </ul>
 *     </li>
 * </ul>
 * Transient states can change at any time, whereas if a final state is observed, it is
 * guaranteed the state will never change again.
 * <p>
 * The internal states and their transitions are depicted below where gray nodes indicate final states:
 * <p style="text-align:center">
 * <img src = "doc-files/computed-constant-states.svg" alt="the internal states">
 *
 * <h2 id="general">General Properties of ComputedConstant</h2>
 *
 * All methods of this class will throw a {@link java.lang.NullPointerException}
 * if a reference parameter is {@code null} unless otherwise specified.
 *
 * All computed constant constructs are "null-friendly" meaning a value can be bound to {@code null}.  As usual, values of type
 * Optional may also express optionality, without using {@code null}, as exemplified here:
 * {@snippet lang = java:
 *     class DemoNull {
 *
 *         private final Supplier<Optional<Color>> backgroundColor =
 *                 ComputedConstant.of(() -> Optional.ofNullable(calculateBgColor()));
 *
 *         Color backgroundColor(Color defaultColor) {
 *             return backgroundColor.get()
 *                     .orElse(defaultColor);
 *         }
 *
 *         private Color calculateBgColor() {
 *             // Read background color from file returning "null" if it fails.
 *             // ...
 *             return null;
 *         }
 *     }
 *}
 *
 * @param <V> The type of the value to be bound
 * @since 22
 */
@PreviewFeature(feature = PreviewFeature.Feature.COMPUTED_CONSTANTS)
public sealed interface ComputedConstant<V>
        extends Supplier<V>
        permits AbstractComputedConstant,
        ListElementComputedConstant,
        MethodHandleComputedConstant,
        StandardComputedConstant {

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
    @Override
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
     * {@return a new {@link ComputedConstant } that will use this computed constant's eventually bound value
     * and then apply the provided {@code mapper} as the pre-set provider}
     *
     * @param mapper to apply to this computed constant
     * @param <R>    the return type of the provided {@code mapper}
     */
    default <R> ComputedConstant<R> map(Function<? super V, ? extends R> mapper) {
        Objects.requireNonNull(mapper);
        return of(() -> mapper.apply(this.get()));
    }

    /**
     * {@return a new {@link ComputedConstant } with the given pre-set {@code provider} to be used to compute a value}
     * <p>
     * If a later attempt is made to invoke any of the
     * {@link #get()}, {@link #orElse(Object)} or {@link #orElseThrow(Supplier)} methods
     * when this computed constant is unbound, the {@code provider} will automatically be invoked.
     * <p>
     * {@snippet lang = java:
     *     class DemoPreset {
     *
     *         private static final ComputedConstant<Foo> FOO = ComputedConstant.of(Foo::new);
     *
     *         public Foo theBar() {
     *             // Foo is lazily constructed and recorded here upon first invocation
     *             return FOO.get();
     *         }
     *     }
     *}
     *
     * @param <V>      the type of the value
     * @param provider to invoke when computing a value
     */
    static <V> ComputedConstant<V> of(Supplier<? extends V> provider) {
        Objects.requireNonNull(provider);
        return StandardComputedConstant.create(provider);
    }

    /**
     * {@return a new {@link ComputedConstant } with the given pre-set {@code provider} to be used to compute a value
     * and provided {@code superType} indicating the most specific return type that can be expressed}
     * <p>
     * If a later attempt is made to invoke any of the
     * {@link #get()}, {@link #orElse(Object)} or {@link #orElseThrow(Supplier)} methods
     * when this computed constant is unbound, the {@code provider} will automatically be invoked.
     * <p>
     * {@snippet lang = java:
     *     class DemoPreset {
     *
     *         private static final ComputedConstant<Foo> FOO = ComputedConstant.of(createFooMH());
     *
     *         public Foo theBar() {
     *             // Foo is lazily constructed and recorded here upon first invocation
     *             return FOO.get();
     *         }
     *     }
     *}
     *
     * @param <V>        the type of the value
     * @param <T>        a superclass of the value
     * @param superType  a class indicating the most specific return type (that can be expressed) of the provider
     *                   (which is used for type inference and method handle validation)
     * @param provider   to invoke when computing a value
     * @throws IllegalArgumentException if the given MethodHandle {@code provider} has a call signature
     *                                  return type that is not assignable from the provided
     *                                  {@code superType} or if it takes any parameters.
     */
    static <V, T extends V> ComputedConstant<V> of(Class<T> superType, MethodHandle provider) {
        Objects.requireNonNull(superType);
        Objects.requireNonNull(provider);
        if (provider.type().parameterCount() != 0) {
            throw new IllegalArgumentException(
                    "The provider must not take parameters: " + provider);
        }
        if (!superType.isAssignableFrom(provider.type().returnType())) {
            throw new IllegalArgumentException(
                    "The provider return type " + provider.type().returnType().getName() +
                            "is not assignable from " + superType.getName() + ": " + provider);
        }
        return MethodHandleComputedConstant.create(provider);
    }

    /**
     * {@return a new unmodifiable List of {@link ComputedConstant } elements with the provided
     * {@code size} and given pre-set {@code mappingProvider} to be used to compute element values}
     * <p>
     * The List and its elements are eligible for constant folding optimizations by the JVM.
     * <p>
     * Below, an example of how to cache values in a list is shown:
     * {@snippet lang = java:
     *     class DemoList {
     *
     *         private static final List<ComputedConstant<Long>> PO2_CACHE =
     *                 ComputedConstant.of(32, index -> 1L << index);
     *
     *         public long powerOfTwoValue(int n) {
     *             return PO2_CACHE.get(n);
     *         }
     *     }
     *}
     * @apiNote The list is free to return <a href="doc-files/ValueBased.html">value-based</a> ComputedConstant elements</a>
     *          that has no valid identity.
     *
     * @param <V>             the type of the values
     * @param size            the size of the List
     * @param mappingProvider to invoke when computing and binding element values
     */
    static <V> List<ComputedConstant<V>> of(int size,
                                            IntFunction<? extends V> mappingProvider) {
        if (size < 0) {
            throw new IllegalArgumentException();
        }
        Objects.requireNonNull(mappingProvider);
        return OnDemandComputedConstantList.create(size, mappingProvider);
    }

}