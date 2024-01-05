/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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

/*
 * @test
 * @summary Verify basic java.lang.SettableConstant operations
 * @enablePreview
 * @run junit SettableConstantTest
 */

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import java.util.NoSuchElementException;
import java.util.stream.Stream;

import static org.junit.jupiter.api.Assertions.*;

final class SettableConstantTest {

    @Test
    void superTypeValidation() {
        SettableConstant<Integer> constant = SettableConstant.of(int.class);
        SettableConstant<Integer> constant2 = SettableConstant.of(Integer.class);
        SettableConstant<Integer> constant3 = SettableConstant.of(Number.class);
        SettableConstant<Integer> constant4 = SettableConstant.of(Object.class);
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void unbound(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType);
        assertThrows(NoSuchElementException.class, constant::get);

        assertEquals(1, constant.orElse(1));
        assertThrows(ArrayIndexOutOfBoundsException.class, () ->
                constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void boundViaConstructor(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType, 1);
        assertEquals(1, constant.get());
        assertEquals(1, constant.orElse(2));
        assertEquals(1, constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
        assertCannotSet(constant);
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void setValue(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType);
        constant.set(1);
        assertEquals(1, constant.get());
        assertEquals(1, constant.orElse(2));
        assertEquals(1, constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
        assertCannotSet(constant);
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void nullValue(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType, null);

        assertNull(constant.get());
        assertNull(constant.orElse(2));
        assertNull(constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
        assertCannotSet(constant);
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void testToString(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType);
        assertEquals("StandardConstant.unbound", constant.toString());
        constant.set(1);
        assertEquals("StandardConstant[1]", constant.toString());
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void testToStringNull(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType, null);
        assertEquals("StandardConstant[null]", constant.toString());
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void predicates(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType);
        assertFalse(constant.isBound());
        constant.set(1);
        assertTrue(constant.isBound());
    }

    @ParameterizedTest
    @MethodSource("storageTypes")
    void setIfUnbound(Class<? super Integer> storageType) {
        SettableConstant<Integer> constant = SettableConstant.of(storageType, 1);
        constant.setIfUnbound(2);
        assertEquals(1, constant.get());
    }

    // Support methods

    static void assertCannotSet(SettableConstant<Integer> constant) {
        assertThrows(IllegalStateException.class, () ->
                constant.set(2)
        );
    }

    static Stream<Arguments> storageTypes() {
        return Stream.of(
                Arguments.of(int.class),
                Arguments.of(Integer.class),
                Arguments.of(Number.class),
                Arguments.of(Object.class)
        );
    }

}
