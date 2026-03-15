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

/*
 * @test
 * @summary Testing the loading of a class with an instances of URLClassLoader.
 *          URLClassLoader instance has system loader as its parent, and shares its classpath with the parent.
 *          The test is run with support for URLClassLoader enabled in AOTCache.
 *          It verifies the class is stored in AOTCache and loaded from the AOTCache in production run.
 *
 * @requires vm.cds
 * @requires vm.cds.custom.loaders
 *
 * @library /test/lib /test/hotspot/jtreg/runtime/cds/appcds
 * @compile test-classes/CustomLoadee.java
 *     test-classes/CustomLoadee3.java
 *     test-classes/CustomLoadee5.java
 *     test-classes/SingleURLClassLoader.java
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar WhiteBox.jar jdk.test.whitebox.WhiteBox
 * @run driver SingleURLClassLoaderTest
 */

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.whitebox.WhiteBox;

public class SingleURLClassLoaderTest {
    private static String appJar;
    private static String customJar;
    private static final String mainClass = "SingleURLClassLoader";

    public static void main(String[] args) throws Exception {
        appJar = JarBuilder.build("SingleURLClassLoaderTest", "SingleURLClassLoader", "CustomLoadee");

        customJar = JarBuilder.build("SingleURLClassLoaderTest_customjar", "CustomLoadee3", "CustomLoadee5");

        Tester tester = new Tester();
        tester.runAOTWorkflow("AOT", "--two-step-training");
    }

    static class Tester extends CDSAppTester {
        public Tester() {
            super(mainClass);
            useWhiteBox(ClassFileInstaller.getJarPath("WhiteBox.jar"));
        }

        @Override
        public String classpath(RunMode runMode) {
            return appJar;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-Xlog:aot+load",
                "-XX:+AOTCacheSupportForCustomLoader",
            };
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                mainClass,
                appJar,
                customJar,
            };
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) throws Exception {
            if (isAOTWorkflow()) {
                if (runMode == RunMode.TRAINING) {
                    out.shouldMatch("category .*SingleURLClassLoaderTest_customjar.jar\\[0\\] CustomLoadee3");
                }
                if (runMode == RunMode.PRODUCTION) {
                    out.shouldMatch("SingleURLClassLoaderTest_customjar.jar CustomLoadee3");
                }
            }
        }
    }
}
