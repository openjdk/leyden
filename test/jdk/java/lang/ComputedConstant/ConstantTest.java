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
 * @summary Verify basic java.lang.Constant operations
 * @enablePreview
 * @run junit ConstantTest
 */

import org.junit.jupiter.api.Test;

import java.util.NoSuchElementException;

import static org.junit.jupiter.api.Assertions.*;

final class ConstantTest {

    @Test
    void unbound() {
        Constant<Integer> constant = Constant.of();
        assertThrows(NoSuchElementException.class, constant::get);

        assertEquals(1, constant.orElse(1));
        assertThrows(ArrayIndexOutOfBoundsException.class, () ->
                constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
    }

    @Test
    void boundViaConstructor() {
        Constant<Integer> constant = Constant.of(1);
        assertEquals(1, constant.get());
        assertEquals(1, constant.orElse(2));
        assertEquals(1, constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
        assertCannotSet(constant);
    }

    @Test
    void setValue() {
        Constant<Integer> constant = Constant.of();
        constant.set(1);

        assertEquals(1, constant.get());
        assertEquals(1, constant.orElse(2));
        assertEquals(1, constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
        assertCannotSet(constant);
    }

    @Test
    void nullValue() {
        Constant<Integer> constant = Constant.of(null);

        assertNull(constant.get());
        assertNull(constant.orElse(2));
        assertNull(constant.orElseThrow(ArrayIndexOutOfBoundsException::new));
        assertCannotSet(constant);
    }

    @Test
    void testToString() {
        Constant<Integer> constant = Constant.of();
        assertEquals("StandardConstant.unbound", constant.toString());
        constant.set(1);
        assertEquals("StandardConstant[1]", constant.toString());
    }

    @Test
    void testToStringNull() {
        Constant<Integer> constant = Constant.of(null);
        assertEquals("StandardConstant[null]", constant.toString());
    }

    @Test
    void predicates() {
        Constant<Integer> constant = Constant.of();
        assertTrue(constant.isUnbound());
        assertFalse(constant.isBound());
        constant.set(1);
        assertFalse(constant.isUnbound());
        assertTrue(constant.isBound());
    }

    @Test
    void setOrDiscard() {
        Constant<Integer> constant = Constant.of(1);
        constant.setOrDiscard(2);
        assertEquals(1, constant.get());
    }

    // Support methods

    static void assertCannotSet(Constant<Integer> constant) {
        assertThrows(IllegalStateException.class, () ->
                constant.set(2)
        );
    }

}
