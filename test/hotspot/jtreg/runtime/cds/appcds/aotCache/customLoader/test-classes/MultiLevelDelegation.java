/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import jdk.test.whitebox.WhiteBox;

public class MultiLevelDelegation {
    static URLClassLoader loader1;
    static URLClassLoader loader2;

    public static void main(String args[]) throws Exception {
        if (args.length < 2) {
            throw new RuntimeException("insufficient arguments");
        }
        URL url1 = new File(args[0]).toURI().toURL();
        URL url2 = new File(args[1]).toURI().toURL();

        // loader1 has system loader as its parent
        loader1 = new URLClassLoader(new URL[] {url1});

        // loader2 has loader1 has its parent
        loader2 = new URLClassLoader(new URL[] {url2}, loader1);

        Class cls1 = loader1.loadClass("CustomLoadee");
        System.out.println("CustomLoadee loaded by: " + cls1.getClassLoader());

        if (cls1.getClassLoader() != loader1) {
            throw new RuntimeException("CustomLoadee should be loaded by loader1");
        }

        Class cls2 = loader2.loadClass("CustomLoadee3");
        System.out.println("CustomLoadee3 loaded by: " + cls2.getClassLoader());

        if (cls2.getClassLoader() != loader2) {
            throw new RuntimeException("CustomLoadee3 should be loaded by loader2");
        }

        WhiteBox wb = WhiteBox.getWhiteBox();

        if (!wb.isAOTSafeCustomLoader(loader1)) {
            throw new RuntimeException("loader1 should be marked as aot-safe");
        }
        if (wb.isAOTSafeCustomLoader(loader2)) {
            throw new RuntimeException("loader2 should not be marked as aot-safe");
        }

        if (wb.isSharedClass(MultiLevelDelegation.class)) {
            if (!wb.isSharedClass(cls1)) {
                throw new RuntimeException("CustomLoadee (loaded by loader1) should be shared");
            }
            if (!wb.isSharedClass(cls2)) {
                throw new RuntimeException("CustomLoadee3 (loaded by loader2) should be shared");
            }
            if (!wb.isLoadedByAOTSafeCustomLoader(cls1)) {
                throw new RuntimeException("CustomLoadee should have been loaded by AOT-safe loader");
            }
            if (wb.isLoadedByAOTSafeCustomLoader(cls2)) {
                throw new RuntimeException("CustomLoadee3 should not have been loaded by AOT-safe loader");
            }
        }
    }
}
