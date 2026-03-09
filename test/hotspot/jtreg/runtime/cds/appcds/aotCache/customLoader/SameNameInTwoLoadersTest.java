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
 * @summary Testing the loading of a class with the same name in two different instances of URLClassLoader
 *          with partially overlapping classpaths. The test is run with support for URLClassLoader enabled
 *          in AOTCache.
 *
 * @requires vm.cds
 * @requires vm.cds.custom.loaders
 *
 * @library /test/lib /test/hotspot/jtreg/runtime/cds/appcds
 * @compile test-classes/CustomLoadee.java
 *     test-classes/CustomLoadee3.java
 *     test-classes/CustomLoadee5.java
 *     test-classes/SameNameUnrelatedLoaders.java
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar WhiteBox.jar jdk.test.whitebox.WhiteBox
 * @run driver SameNameInTwoLoadersTest
 */

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.whitebox.WhiteBox;


public class SameNameInTwoLoadersTest {
    private static String appJar;
    private static String commonJar;
    private static String customJar1;
    private static String customJar2;
    private static final String mainClass = "SameNameUnrelatedLoaders";

    public static void main(String[] args) throws Exception {
        appJar = JarBuilder.build("SameNameInTwoLoadersTest", "SameNameUnrelatedLoaders");

        commonJar = JarBuilder.build("SameNameInTwoLoadersTest_commonjar", "CustomLoadee");
        customJar1 = JarBuilder.build("SameNameInTwoLoadersTest_customjar1", "CustomLoadee3");
        customJar2 = JarBuilder.build("SameNameInTwoLoadersTest_customjar2", "CustomLoadee5");

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
                commonJar,
                customJar1,
                customJar2
            };
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) throws Exception {
            if (isAOTWorkflow()) {
                if (runMode == RunMode.TRAINING) {
                    out.shouldMatch("category .*SameNameInTwoLoadersTest_customjar1.jar\\[0\\] CustomLoadee");
                    out.shouldMatch("category .*SameNameInTwoLoadersTest_customjar2.jar\\[0\\] CustomLoadee");
                }
                if (runMode == RunMode.PRODUCTION) {
                    out.shouldMatch("SameNameInTwoLoadersTest_customjar1.jar CustomLoadee");
                    out.shouldMatch("SameNameInTwoLoadersTest_customjar2.jar CustomLoadee");
                }
            }
        }
    }
}
