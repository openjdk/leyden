/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

package jdk.internal.constant;

public final class ConstantUtil {

    private ConstantUtil() {
    }

    static final Binding BINDING_SENTINEL = new Binding();
    static final NonNull NON_NULL_SENTINEL = new NonNull();
    static final Null NULL_SENTINEL = new Null();
    static final BindError BIND_ERROR_SENTINEL = new BindError();

    sealed interface State {}
    sealed interface Bound {}

    static final class Binding   implements State        { private Binding() {} }
    // The Null class must not have an internal state to enable
    // plain memory semantic access.
    static final class Null      implements State, Bound { private Null() {} }
    static final class NonNull   implements State, Bound { private NonNull() {} }
    static final class BindError implements State        { private BindError() {} }

}
