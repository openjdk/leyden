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

public class MultipleURLClassLoadersSameClasspath {
    static URLClassLoader loader1, loader2, loader3;

    public static void main(String args[]) throws Exception {
        if (args.length < 1) {
            throw new RuntimeException("insufficient arguments");
        }
        String path = args[0];
        URL url = new File(path).toURI().toURL();
        URL[] urls = new URL[] {url};

        loader1 = new URLClassLoader(urls);
        loader2 = new URLClassLoader(urls);
        loader3 = new URLClassLoader(urls);

        Class class01 = loader1.loadClass("CustomLoadee");
        Class class02 = loader2.loadClass("CustomLoadee");
        Class class03 = loader3.loadClass("CustomLoadee");
        
        System.out.println("class01 = " + class01);
        System.out.println("class02 = " + class02);
        System.out.println("class03 = " + class03);

        if (class01.getClassLoader() != loader1) {
            throw new RuntimeException("class01 loaded by wrong loader");
        }
        if (class02.getClassLoader() != loader2) {
            throw new RuntimeException("class02 loaded by wrong loader");
        }
        if (class03.getClassLoader() != loader3) {
            throw new RuntimeException("class03 loaded by wrong loader");
        }

        WhiteBox wb = WhiteBox.getWhiteBox();

        if (!wb.isAOTSafeCustomLoader(loader1)) {
            throw new RuntimeException("loader1 should be marked as aot-safe");
        }
        if (wb.isAOTSafeCustomLoader(loader2)) {
            throw new RuntimeException("loader2 should not be marked as aot-safe");
        }
        if (wb.isAOTSafeCustomLoader(loader3)) {
            throw new RuntimeException("loader3 should not be marked as aot-safe");
        }

        if (wb.isSharedClass(MultipleURLClassLoadersSameClasspath.class)) {
            if (!wb.isSharedClass(class01)) {
                throw new RuntimeException("first class is not shared");
            }
            if (!wb.isSharedClass(class02)) {
                throw new RuntimeException("second class is not shared");
            }
            if (wb.isSharedClass(class03)) {
                throw new RuntimeException("third class should not be shared");
            }
            if (!wb.isLoadedByAOTSafeCustomLoader(class01)) {
                throw new RuntimeException("first class should have been loaded by AOT-safe loader");
            }
            if (wb.isLoadedByAOTSafeCustomLoader(class02)) {
                throw new RuntimeException("second class should not have been loaded by AOT-safe loader");
            }
        }
    }
}
