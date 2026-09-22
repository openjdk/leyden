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
 * @summary Test URLClassLoader with multi-level delegation hierarchy i.e. CL2 -> CL1 -> System loader
 * @requires vm.cds.supports.aot.class.linking
 * @library /test/lib /test/hotspot/jtreg/runtime/cds/appcds/aotCache/customLoader/test-classes
 * @build MultiLevelDelegationTest
 * @build MyLoadeeA MyLoadeeB MyLoadeeC
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar AppWithMultiLevelDelegation URLClassLoaderFactory
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar cust.jar MyLoadeeA MyLoadeeB
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar custC.jar MyLoadeeC
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar WhiteBox.jar jdk.test.whitebox.WhiteBox
 * @run driver MultiLevelDelegationTest AOT
 */

import java.net.URL;
import java.net.URLClassLoader;
import java.io.File;
import java.io.InputStream;
import java.nio.file.Path;
import java.nio.file.Paths;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.cds.CDSJarUtils;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.whitebox.WhiteBox;

public class MultiLevelDelegationTest {
    private static final String mainClass = "AppWithMultiLevelDelegation";

    public static void main(String... args) throws Exception {
        Tester tester = new Tester();
        tester.runAOTWorkflow();
    }

    static class Tester extends CDSAppTester {
        public Tester() {
            super(mainClass);
            useWhiteBox(ClassFileInstaller.getJarPath("WhiteBox.jar"));
        }

        @Override
        public String classpath(RunMode runMode) {
            return "app.jar";
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
                runMode.name()
            };
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) throws Exception {
            if (isAOTWorkflow()) {
                if (runMode == RunMode.TRAINING) {
                    out.shouldMatch("\\[aot,load\\] category SYSTEM[:;].*cust.jar\\[0\\] MyLoadeeA");
                    out.shouldMatch("\\[aot,load\\] category SYSTEM[:;].*cust.jar\\[1\\] MyLoadeeB");
                    out.shouldMatch("\\[aot,load\\] category unreg\\[0\\] MyLoadeeC");
                }
                if (runMode == RunMode.PRODUCTION) {
                    out.shouldMatch("\\[aot,load\\] SYSTEM[:;].*cust.jar MyLoadeeA");
                    out.shouldMatch("\\[aot,load\\] SYSTEM[:;].*cust.jar MyLoadeeB");
                }
            }
        }
    }
}

class AppWithMultiLevelDelegation {
    public static void main(String args[]) throws Exception {
        if (args.length != 1) {
            throw new RuntimeException("Unexpected number of arguments. Expects RunMode as the argument");
        }
        test(args[0]);
    }

    // Create multiple loaders with different classpath
    // Ensure loader created later with the same classpath as an existing loader is not marked aot-safe
    // but its class should still be shared under "unregistered" category
    static void test(String runMode) throws Exception {
        ClassLoader loader1 = URLClassLoaderFactory.createURLClassLoader(ClassLoader.getSystemClassLoader(), "cust.jar");
        Class klass1 = loader1.loadClass("MyLoadeeA");
        klass1.newInstance();

        // Another loader with different classpath with previously created loader as its parent
        ClassLoader loader2 = URLClassLoaderFactory.createURLClassLoader(loader1, "custC.jar");
        Class klass2 = loader2.loadClass("MyLoadeeC");
        klass2.newInstance();

        WhiteBox wb = WhiteBox.getWhiteBox();
        if (runMode.equals("TRAINING")) {
            if (!wb.isAOTSafeCustomLoader(loader1)) {
                throw new RuntimeException("loader1 should be marked as aot-safe");
            }
            if (wb.isAOTSafeCustomLoader(loader2)) {
                throw new RuntimeException("loader2 should not be marked as aot-safe");
            }
        }
        if (runMode.equals("PRODUCTION")) {
            if (!wb.isAOTSafeCustomLoader(loader1)) {
                throw new RuntimeException("loader1 should be marked as aot-safe");
            }
            if (!wb.isSharedClass(klass1)) {
                throw new RuntimeException("class \"klass1\" should be shared");
            }

            if (wb.isAOTSafeCustomLoader(loader2)) {
                throw new RuntimeException("loader2 should not be marked as aot-safe");
            }
            if (!wb.isSharedClass(klass2)) {
                throw new RuntimeException("class \"klass2\" should be shared");
            }
        }
    }
}
