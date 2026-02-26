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

public class SameNameUnrelatedLoaders {
    static URLClassLoader ldr01, ldr02;

    public static void main(String args[]) throws Exception {
        if (args.length < 2) {
            throw new RuntimeException("insufficient arguments");
        }
        String path = args[0];
        URL url = new File(path).toURI().toURL();
        URL[] ldr01_urls = new URL[] {url};

        path = args[1];
        url = new File(path).toURI().toURL();
        URL[] ldr02_urls = new URL[] {url};

        ldr01 = new URLClassLoader(ldr01_urls);
        ldr02 = new URLClassLoader(ldr02_urls);

        Class class01 = ldr01.loadClass("CustomLoadee");
        Class class02 = ldr02.loadClass("CustomLoadee");

        System.out.println("class01 = " + class01);
        System.out.println("class02 = " + class02);

        if (class01.getClassLoader() != ldr01) {
            throw new RuntimeException("class01 loaded by wrong loader");
        }
        if (class02.getClassLoader() != ldr02) {
            throw new RuntimeException("class02 loaded by wrong loader");
        }

        if (true) {
            if (class01.isAssignableFrom(class02)) {
                throw new RuntimeException("assignable condition failed");
            }

            Object obj01 = class01.newInstance();
            Object obj02 = class02.newInstance();

            if (class01.isInstance(obj02)) {
                throw new RuntimeException("instance relationship condition 01 failed");
            }
            if (class02.isInstance(obj01)) {
                throw new RuntimeException("instance relationship condition 02 failed");
            }
        }

        WhiteBox wb = WhiteBox.getWhiteBox();
        if (wb.isSharedClass(SameNameUnrelatedLoaders.class)) {
            boolean class1Shared = wb.isSharedClass(class01);
            boolean class2Shared = wb.isSharedClass(class02);
            if (!class1Shared) {
                throw new RuntimeException("first class is not shared");
            }

            if (!class2Shared) {
                throw new RuntimeException("second class is not shared");
            }
        }
    }
}
