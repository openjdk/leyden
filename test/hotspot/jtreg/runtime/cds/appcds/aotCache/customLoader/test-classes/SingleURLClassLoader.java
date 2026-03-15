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

public class SingleURLClassLoader {
    static URLClassLoader loader;

    public static void main(String args[]) throws Exception {
        if (args.length < 2) {
            throw new RuntimeException("insufficient arguments");
        }
        URL url1 = new File(args[0]).toURI().toURL();
        URL url2 = new File(args[1]).toURI().toURL();
        URL[] urls = new URL[] {url1, url2};

        loader = new URLClassLoader(urls);

        // CustomLoadee is present in classpath of the URLClassLoader and its parent (which is system loader);
        // so the CustomLoadee should get loaded by the parent loader if URLClassLoader follows parent-first delegation model
        Class class01 = loader.loadClass("CustomLoadee");
        // CustomLoadee3 is only present in the classpath of the URLClassLoader
        Class class02 = loader.loadClass("CustomLoadee3");

        System.out.println("class01 = " + class01);
        System.out.println("class02 = " + class02);

        if (class01.getClassLoader() != ClassLoader.getSystemClassLoader()) {
            throw new RuntimeException("CustomLoadee loaded by wrong loader");
        }
        if (class02.getClassLoader() != loader) {
            throw new RuntimeException("CustomLoadee3 loaded by wrong loader");
        }

        WhiteBox wb = WhiteBox.getWhiteBox();

        if (!wb.isAOTSafeCustomLoader(loader)) {
            throw new RuntimeException("loader should be marked as aot-safe");
        }

        if (wb.isSharedClass(SingleURLClassLoader.class)) {
            if (!wb.isSharedClass(class01)) {
                throw new RuntimeException("CustomLoadee class is not shared");
            }
            if (!wb.isSharedClass(class02)) {
                throw new RuntimeException("CustomLoadee3 class is not shared");
            }
            if (!wb.isLoadedByBuiltinLoader(class01)) {
                throw new RuntimeException("CustomLoadee should be loaded by builtin loader");
            }
            if (!wb.isLoadedByAOTSafeCustomLoader(class02)) {
                throw new RuntimeException("CustomLoadee3 should be loaded by aot-safe loader");
            }
        }
    }
}
