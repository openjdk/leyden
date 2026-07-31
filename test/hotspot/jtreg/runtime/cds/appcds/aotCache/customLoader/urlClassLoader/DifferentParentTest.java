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
 * @summary Ensure two URLClassLoaders created with the same classpath but different parent are aot-safe
 * @requires vm.cds.supports.aot.class.linking
 * @library /test/lib /test/hotspot/jtreg/runtime/cds/appcds/aotCache/customLoader/test-classes
 * @build DifferentParentTest
 * @build MyLoadeeA
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar AppWithDifferentParent URLClassLoaderFactory
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar cust.jar MyLoadeeA MyLoadeeB
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar WhiteBox.jar jdk.test.whitebox.WhiteBox
 * @run driver DifferentParentTest AOT
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

public class DifferentParentTest {
    private static final String mainClass = "AppWithDifferentParent";

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
                    out.shouldMatch("\\[aot,load\\] category SYSTEM:.*cust.jar\\[0\\] MyLoadeeA");
                    out.shouldMatch("\\[aot,load\\] category BOOT:.*cust.jar\\[0\\] MyLoadeeA");
                }
                if (runMode == RunMode.PRODUCTION) {
                    out.shouldMatch("\\[aot,load\\] SYSTEM:.*cust.jar MyLoadeeA");
                    out.shouldMatch("\\[aot,load\\] BOOT:.*cust.jar MyLoadeeA");
                }
            }
        }
    }
}

class AppWithDifferentParent {
    public static void main(String args[]) throws Exception {
        if (args.length != 1) {
            throw new RuntimeException("Unexpected number of arguments. Expects RunMode as the argument");
        }
        test(args[0]);
    }

    // Creates 2 URLClassLoaders with same classpath but different parent.
    // loader1 has system loader as its parent, loader2 has bootloader as its parent.
    // Ensures the loaders are marked as aot-safe, and classes loaded by them are shared
    static void test(String runMode) throws Exception {
        ClassLoader loader1 = URLClassLoaderFactory.createURLClassLoader(ClassLoader.getSystemClassLoader(), "cust.jar");
        ClassLoader loader2 = URLClassLoaderFactory.createURLClassLoader(null, "cust.jar");

        Class klass1 = loader1.loadClass("MyLoadeeA");
        klass1.newInstance();

        Class klass2 = loader2.loadClass("MyLoadeeA");
        klass2.newInstance();

        // classes loaded by system loader should not be visible to the custom loader
        // that has boot loader as its parent
        try {
            Class appClass = loader2.loadClass("AppWithURLClassLoaders");
            throw new RuntimeException("should have thrown ClassNotFoundException");
        } catch (ClassNotFoundException e) {
            // expected; ignore it
        }

        WhiteBox wb = WhiteBox.getWhiteBox();
        if (runMode.equals("TRAINING")) {
            if (!wb.isAOTSafeCustomLoader(loader1)) {
                throw new RuntimeException("loader1 should be marked as aot-safe");
            }
            if (!wb.isAOTSafeCustomLoader(loader2)) {
                throw new RuntimeException("loader2 should be marked as aot-safe");
            }
        }

        if (runMode.equals("PRODUCTION")) {
             if (!wb.isAOTSafeCustomLoader(loader1)) {
                throw new RuntimeException("loader1 should be marked as aot-safe");
            }
            if (!wb.isSharedClass(klass1)) {
                throw new RuntimeException("class loaded by loader1 should be marked as aot-safe");
            }

            if (!wb.isAOTSafeCustomLoader(loader2)) {
                throw new RuntimeException("loader2 should be marked as aot-safe");
            }
            if (!wb.isSharedClass(klass2)) {
                throw new RuntimeException("class loaded by loader2 should be marked as aot-safe");
            }
        }
    }
}
