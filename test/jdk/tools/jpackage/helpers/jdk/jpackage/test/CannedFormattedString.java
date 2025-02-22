/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
package jdk.jpackage.test;

import java.util.List;
import java.util.function.BiFunction;

public final class CannedFormattedString {

    CannedFormattedString(BiFunction<String, Object[], String> formatter,
            String key, Object[] args) {
        this.formatter = formatter;
        this.key = key;
        this.args = args;
    }

    public String getValue() {
        return formatter.apply(key, args);
    }

    @Override
    public String toString() {
        if (args.length == 0) {
            return String.format("%s", key);
        } else {
            return String.format("%s+%s", key, List.of(args));
        }
    }

    private final BiFunction<String, Object[], String> formatter;
    private final String key;
    private final Object[] args;
}
