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
 * @summary Test multi-level delegation with URLClassLoader L1 delegates to system loader
 *          and another URLClassLoader L2 delegating to L1 (L2 --> L1 --> SL).
 *          Classes loaded by L1 should get full AOTCache support but
 *          classes loaded by L2 would be added to "unregistered" category.
 *          Only those URLClassLoaders with builtin loader as the parent are currently fully
 *          supported in AOTCache.
 *
 * @requires vm.cds
 * @requires vm.cds.custom.loaders
 *
 * @library /test/lib /test/hotspot/jtreg/runtime/cds/appcds
 * @compile test-classes/CustomLoadee.java
 *     test-classes/CustomLoadee3.java
 *     test-classes/MultiLevelDelegation.java
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar WhiteBox.jar jdk.test.whitebox.WhiteBox
 * @run driver MultiLevelDelegationTest
 */

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.whitebox.WhiteBox;

public class MultiLevelDelegationTest {
    private static String appJar;
    private static String customJar;
    private static String customJar2;
    private static final String mainClass = "MultiLevelDelegation";

    public static void main(String[] args) throws Exception {
        appJar = JarBuilder.build("MultiLevelDelegationTest", "MultiLevelDelegation");
        // jar file for loader L1
        customJar = JarBuilder.build("MultiLevelDelegationTest_customjar", "CustomLoadee");
        // jar file for loader L2
        customJar2 = JarBuilder.build("MultiLevelDelegationTest_customjar2", "CustomLoadee3");

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
                "-XX:+AOTCacheSupportForCustomLoader"
            };
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                mainClass,
                customJar,
                customJar2,
            };
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) throws Exception {
            if (isAOTWorkflow()) {
                if (runMode == RunMode.TRAINING) {
                    // L1's class (CustomLoadee from customJar) should be in a custom loader category
                    out.shouldMatch("category .*MultiLevelDelegationTest_customjar.jar\\[0\\] CustomLoadee");
                    // L2's class (CustomLoadee3 from customJar2) should be in "unregistered" category
                    // because its parent is not the system class loader
                    out.shouldMatch("category unreg\\[0\\] CustomLoadee3");
                }
                if (runMode == RunMode.PRODUCTION) {
                    // L1's class should be eagerly loaded from the AOT cache
                    out.shouldMatch("MultiLevelDelegationTest_customjar.jar CustomLoadee");
                    // L2's class should NOT be eagerly loaded from the AOT cache
                    out.shouldNotMatch("MultiLevelDelegationTest_customjar2.jar CustomLoadee3");
                }
            }
        }
    }
}
