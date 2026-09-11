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
 * @summary Ensure change in classpath is caught by verification during assembly and production run.
 * @requires vm.cds.supports.aot.class.linking
 * @library /test/lib /test/hotspot/jtreg/runtime/cds/appcds/aotCache/customLoader/test-classes
 * @build ClasspathVerificationFailureTest
 * @build MyLoadeeA MyLoadeeB MyLoadeeC
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar AppWithClasspathVerificationFailure URLClassLoaderFactory
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar cust.jar MyLoadeeA MyLoadeeB
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar cust1.jar MyLoadeeA MyLoadeeB MyLoadeeC
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar WhiteBox.jar jdk.test.whitebox.WhiteBox
 * @run driver ClasspathVerificationFailureTest AOT
 */

import java.net.URL;
import java.net.URLClassLoader;
import java.io.File;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.cds.CDSJarUtils;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.whitebox.WhiteBox;

public class ClasspathVerificationFailureTest {
    private static final String mainClass = "AppWithClasspathVerificationFailure";

    enum ExpectedVerificationFailurePhase {
        ASSEMBLY_PHASE,
        PRODUCTION_PHASE
    };

    public static void main(String... args) throws Exception {
        // Make a copy of the original jar file used by URLClassLoader
        Files.copy(Paths.get("cust.jar"), Paths.get("cust-orig.jar"));
        Tester tester1 = new Tester(ExpectedVerificationFailurePhase.ASSEMBLY_PHASE);
        tester1.recordAOTConfiguration();
        // Replace cust.jar with cust1.jar
        Files.copy(Paths.get("cust1.jar"), Paths.get("cust.jar"), StandardCopyOption.REPLACE_EXISTING);
        // AOTCache creation should fail
        tester1.setCheckExitValue(false);
        tester1.createAOTCache();

        // Restore original cust.jar
        Files.copy(Paths.get("cust-orig.jar"), Paths.get("cust.jar"), StandardCopyOption.REPLACE_EXISTING);

        Tester tester2 = new Tester(ExpectedVerificationFailurePhase.PRODUCTION_PHASE);
        tester2.runAOTAssemblyWorkflow();
        // Replace cust.jar with cust1.jar
        Files.copy(Paths.get("cust1.jar"), Paths.get("cust.jar"), StandardCopyOption.REPLACE_EXISTING);
        tester2.productionRun();
    }

    static class Tester extends CDSAppTester {
        ExpectedVerificationFailurePhase _failurePhase;

        public Tester(ExpectedVerificationFailurePhase failurePhase) {
            super(mainClass);
            _failurePhase = failurePhase;
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
                    // MyLoadeeA from cust.jar should be in a custom loader category
                    out.shouldMatch("\\[aot,load\\] category SYSTEM:.*cust.jar\\[0\\] MyLoadeeA");
                }
                if (_failurePhase == ExpectedVerificationFailurePhase.ASSEMBLY_PHASE && runMode == RunMode.ASSEMBLY) {
                    out.shouldMatch("This file is not the one used while building the AOT cache:.*cust.jar");
                    out.shouldMatch("URLClassLoader classpath validation failed");
                }
                if (_failurePhase == ExpectedVerificationFailurePhase.PRODUCTION_PHASE && runMode == RunMode.PRODUCTION) {
                    out.shouldMatch("This file is not the one used while building the AOT cache:.*cust.jar");
                    out.shouldMatch("URLClassLoader classpath validation failed");
                    out.shouldNotMatch("\\[aot,load\\] SYSTEM:.*cust.jar MyLoadeeA");
                }
            }
        }
    }
}

class AppWithClasspathVerificationFailure {
    public static void main(String args[]) throws Exception {
        if (args.length != 1) {
            throw new RuntimeException("Unexpected number of arguments. Expects RunMode as the argument");
        }
        test(args[0]);
    }

    // Ensure loader with system loader as parent is marked as aot-safe, and classes loaded by it are shared
    static void test(String runMode) throws Exception {
        ClassLoader loader = URLClassLoaderFactory.createURLClassLoader(ClassLoader.getSystemClassLoader(), "cust.jar");
        Class klass = loader.loadClass("MyLoadeeA");
        klass.newInstance();

        WhiteBox wb = WhiteBox.getWhiteBox();
        if (runMode.equals("TRAINING")) {
            if (!wb.isAOTSafeCustomLoader(loader)) {
                throw new RuntimeException("loader should be marked as aot-safe");
            }
        }
        if (runMode.equals("PRODUCTION")) {
            if (wb.isAOTSafeCustomLoader(loader)) {
                throw new RuntimeException("loader should not be marked as aot-safe");
            }
            if (wb.isSharedClass(klass)) {
                throw new RuntimeException("class should not be shared");
            }
        }
    }
}
