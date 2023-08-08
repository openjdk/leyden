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
 *
 */

/*
 * @test
 * @bug 8293336
 * @summary Test for archiving resolved invokedynamic call sites (with JavaC)
 * @requires vm.cds.write.archived.java.heap
 * @modules java.base/sun.invoke.util java.logging
 * @library /test/jdk/lib/testlibrary /test/lib
 *          /test/hotspot/jtreg/runtime/cds/appcds
 *          /test/hotspot/jtreg/runtime/cds/appcds/test-classes
 * @build IndyAndJavac
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar Dummy
 * @run driver IndyAndJavac
 */


import java.io.FileWriter;

import jdk.test.lib.helpers.ClassFileInstaller;

public class IndyAndJavac extends IndyTestBase {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass_javac = "com.sun.tools.javac.Main";

    // Force some tests to be disabled during development.
    static String forceSkip = null;

    public static void main(String[] args) throws Exception {
        setup(forceSkip, appJar);
        testJavaC();
    }

    static void testJavaC() throws Exception {
        String sourceFile = "Test.java";
        try (FileWriter fw = new FileWriter(sourceFile)) {
            fw.write("public class Test {\n");
            for (int i = 0; i < 3000; i++) {
                fw.write("    private static final int arr_" + i + "[] = {1, 2, 3};\n");
                fw.write("    private static int method_" + i + "() { return arr_" + i + "[1];}\n");
            }
            fw.write("}\n");
        }
        test("Use javac with archived MethodTypes and LambdaForms", mainClass_javac, sourceFile, sourceFile,
             RUN_STATIC |
           //RUN_BENCH |
             RUN_AOT
             );
        checkExec(null, /*lambdaFormsMustBeArchived*/false); // Some lambda forms are generated because we don't cache invokedynamic for lambda proxies yet
    }
}

class Dummy {

}
