/*
 * Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package jdk.internal.vm.annotation;

import java.lang.annotation.*;

/**
 * A field may be annotated as "stable" to indicate that it is a
 * <em>stable variable</em>, expected to change value at most once.
 * The first value stored into the field (assuming it is not a
 * duplicate of the the field's default initial value), allows the VM
 * to assume that no more significant changes will occur.  This in
 * turn enables the VM to optimize the stable variable, treating uses
 * of it as constant values.  This behavior is a useful building block
 * for lazy evaluation or memoization of results.  In rare and subtle
 * use cases, stable fields may also assume on multiple values over
 * time, with effects to be described below.
 * <p>
 * For example, declaring two stable fields of type {@code int} and
 * {@code String} creates a pair of stable variables, initialized to
 * zero and the null reference (respectively).  Storing a non-zero
 * integer to the first field, or a non-null string to the second,
 * will enable the VM to expect that the stored value is now the
 * permanent value of the field, going forward.  This condition may be
 * used by the VM compiler to improve code quality more aggressively.
 * <p>
 * Since all heap variables begin with a default null value for
 * references (resp., zero for primitives), there is an ambiguity when
 * the VM discovers a stable variable holding a null or primitive zero
 * value.  Does the user intend the VM to constant fold that
 * (uninteresting) value?  Or is the user waiting until later to
 * assign another value to the variable?  The VM does not
 * systematically record stores of a default null (or primitive zero),
 * so there is no way for the VM to decide if a default field value is
 * an undisturbed initial default value, or has been overwritten with
 * an intentionally stored null (or primitive zero).  This is why the
 * programmer should store non-default values into stable variables,
 * if the consequent optimization is desired.
 * <p>
 * As a special case, if a stable field is declared as an array type
 * with one dimension, both the array as a whole, and its eventual
 * components, are treated as independent stable variables.  When a
 * reference to an array of length <i>N</i> is stored to the field,
 * then the array object itself is taken to be a constant, as with any
 * stable field.  But then all <i>N</i> of the array components are
 * <em>also</em> treated as independent stable variables.  Such a
 * stable array may contain any type, reference or primitive.
 * <p>
 * More generally, if a stable field is declared as an array type with
 * <em>D</em> dimensions, then all the non-null components of the
 * array, and of any sub-arrays up to a nesting depth less than
 * <em>D</em>, are treated as stable variables.  Thus, a stable field
 * declared as an array potentially defines a tree (of fixed depth
 * <em>D</em>) containing many stable variables, with each such stable
 * variable is independently considered for optimization.  In this
 * way, and depending on program execution, a single {@code Stable}
 * annotation can potentially create many independent stable
 * variables.  Since the top-level array reference is always stable,
 * it is in general a bad idea to resize the array, even while keeping
 * all existing components unchanged.  (This could be relaxed in the
 * future, to allow expansion of stable arrays, if there were a use
 * case that could deal correctly with races.)
 * <p>
 * As very simple example, a boolean variable is constant-folded only
 * when it is set to {@code true}.  Even this simple behavior is
 * sometimes useful for recording a permanent one-shot state change,
 * in such a way that the compiler can remove dead code associated
 * with the initial state.  It is in general a bad idea to reset
 * such a variable to {@code false}, since compiled code might have
 * "frozen" the {@code true} value, and will never detect the reset
 * value.
 * <p>
 * Fields which are declared {@code final} may also be annotated as stable.
 * Since final fields already behave as stable values, such an annotation
 * conveys no additional information regarding change of the field's value, but
 * still conveys information regarding change of additional component values if
 * the type of the field is an array type (as described above).
 * <p>
 * There are special times early in application startup when constant
 * folding of stable variables is disabled.  Specifically, the VM does
 * not observe the effects of any {@code @Stable} annotation until
 * after an AOT cache assembly phase (if any) has run to completion.
 * Therefore, during these early times, a stable variable can be
 * changed in any way, just like a regular mutable variable (field or
 * array component).  If a field is annotated {@code @Stable} but is
 * treated in this way during AOT cache assembly, that fact must be
 * clearly stated as a warning on the field declaration.  If there is
 * no such warning, maintainers can ignore this edge case.
 * <p>
 * In order to assist refactoring between {@code final} and
 * {@code @Stable} field declarations, the Java Memory Model
 * <em>freeze</em> operation is applied to both kinds of fields, when
 * the assignment occurs in a class or object initializer (i.e.,
 * static initialization code in {@code <clinit>} or constructor code
 * in {@code <init>}).  The freezing of a final or stable field is
 * (currently) triggered only when an actual assignment occurs, directly
 * from the initializer method ({@code <clinit>} or {@code <init>}).
 * It is implemented in HotSpot by an appropriate memory barrier
 * instruction at the return point of the initializer method.  In this
 * way, any non-null (or non-zero) value stored to a stable variable
 * (either field or array component) will appear without races to any
 * user of the class or object that has been initialized.
 * <p>
 * There is no such freeze operation applied to stable field stores in
 * any other context.  This implies that a constructor may choose to
 * initialize a stable variable, rather than "leaving it for later",
 * and that initial will be safely published, as if the field were
 * {@code final}.  The stored value may (or may not) contain
 * additional stable variables, not yet initialized.  Note that if a
 * stable variable is written outside of the code of a constructor (or
 * class initializer), then data races are possible, just the same as
 * if there were no {@code @Stable} annotation, and the field was a
 * regular mutable field.  In fact, the usual case for lazily
 * evaluated data structures is to assign to stable variables much
 * later than the enclosing data structure is created.  This means
 * that racing reads and writes might observe nulls (or primitive
 * zeroes) as well as non-default values.
 * <p>
 * Therefore, most code which reads stable variables should not assume
 * that the value has been set, and should dynamically test for a null
 * (or zero) value.  Code which cannot prove proper ordering of
 * initialization may use stable variables without performing the null
 * (or zero) test.  Code which omits the null (or zero) test should be
 * documented as to why the initialization order is reliable.  In
 * general, some sort of critical section for initialization should be
 * pointed out as provably preceding all uses of the (unchecked)
 * stable variable.  Examples of such a critical section would be a
 * constructor body which directly writes a final or stable variable,
 * or the AOT cache assembly phase as a whole.
 * <p>
 * The HotSpot VM relies on this annotation to promote a non-null (or
 * non-zero) stable variable use to a constant, thereby enabling superior
 * optimizations of code depending on the value (such as constant folding).
 * More specifically, the HotSpot VM will process non-null stable fields (final
 * or otherwise) in a similar manner to static final fields with respect to
 * promoting the field's value to a constant.  Thus, placing aside the
 * differences for null/non-null values and arrays, a final stable field is
 * treated as if it is really final from both the Java language and the HotSpot
 * VM.
 * <p>
 * After constant folding, the compiler can make use of may aspects of
 * the object: Its dynamic type, its length (if it is an array), and
 * the values of its fields (if they are themselves constants, either
 * final or stable).  It is in general a bad idea to reset such
 * variables to any other value, since compiled code might have folded
 * an earlier stored value, and will never detect the reset value.
 * <p>
 * The HotSpot interpreter is not fully aware of stable annotations,
 * and treats annotated fields (and any affected arrays) as regular
 * mutable variables.  Thus, a field annotated as {@code @Stable} may
 * be given a series of values, by explicit assignment, by reflection,
 * or by some other means.  If the HotSpot compiler observes one of
 * these values, and constant-folds it in the setting of some
 * particular compilation task, then in some contexts (execution of
 * fully optimized code) the field will appear to have one
 * "historical" value, while in others (less optimized contexts) the
 * field will appear to have a more recent value.  (And it is no good
 * to try to "reset" a stable value by storing its default again,
 * because there is currently no way to find and deoptimize any and
 * all affected compiled code.)  Race conditions would make this even
 * more complex, since with races there is no definable "most recent"
 * value.
 * <p>
 * Note also each compiliation task makes its own decisions about
 * whether to observe stable variable values, and how aggressively to
 * constant-fold them.  And a method that uses a stable variable might
 * be inlined by many different compilation tasks.  The net result of
 * all this is that, if stable variables are multiply assigned, the
 * program execution may observe any "historical" value (if it was
 * captured by some particular compilation task), as well as a "most
 * recent" value observed by the interpreter or less-optimized code.
 * <p>
 * For all these reasons, a user who bends the rules for a stable
 * variable, by assigning several values to it, must state the
 * intended purposes carefully in warning documentation on the
 * relevant stable field declaration.  That user's code must function
 * correctly when observing any or all of the assigned values, at any
 * time.  Alternatively, field assignments must be constrained
 * appropriately so that unwanted values are not observable by
 * compiled code.
 * <p>
 * The {@code @Stable} annotation is intended for use in the JDK
 * implemention, and with the HotSpot VM, to support optimization of
 * classes and algorithms defined by the JDK.  Any class which uses
 * this annotation is responsible for constraining assignments in such
 * a way as not to violate API contracts of the class.  Such
 * constraints can be arranged using explicit atomic access
 * (sychronization, CAS, etc.), or by concealing the effects of
 * multiple assignments in some API-dependent way, or by providing
 * some other internal proof of correctness (accounting for any
 * possible racing API access), or by some appropriate disclaimer in
 * the API about undefined behavior.
 *
 * @implNote
 * This annotation only takes effect for fields of classes loaded by the boot
 * loader.  Annotations on fields of classes loaded outside of the boot loader
 * are ignored.
 */
@Target(ElementType.FIELD)
@Retention(RetentionPolicy.RUNTIME)
public @interface Stable {
}
