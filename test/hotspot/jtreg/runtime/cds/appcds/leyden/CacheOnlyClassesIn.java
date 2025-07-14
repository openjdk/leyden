/*
 * Copyright (c) 2024, 2025, Oracle and/or its affiliates. All rights reserved.
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

// Experimental feature to allow special instrumentation code to be executed during training run:
//
// Example:  app.jar   -- Contains the classes for the real application.
//                        Entry point = the "App" class
//           test.jar  -- Instrumentation code to exercise the app or the framework. 
//                        Entry point = the "TestDriver" class
//
// Training run:
//
//     rm app.cds
//     java -XX:CacheDataStore=app.cds -cp app.jar:test.jar -XX:CacheOnlyClassesIn=app.jar TestDriver
//
// All classes in test.jar will be excluded from app.cds
//
// Production run:
//
//     java -XX:CacheDataStore=app.cds -cp app.jar:test.jar App
//
// TODO:
//     Allow "-cp app.jar" to be used in production run, so test classes don't leaked into
//     the production run.

/*
 * @test id=static
 * @summary test the -XX:CacheOnlyClassesIn flag with the classic static archive workflow
 * @requires vm.cds
 * @library /test/lib
 * @run driver CacheOnlyClassesIn STATIC
 */

/*
 * @test id=dynamic
 * @summary test the -XX:CacheOnlyClassesIn flag with the classic dynamic archive workflow
 * @requires vm.cds
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xbootclasspath/a:. CacheOnlyClassesIn DYNAMIC
 */

/*
 * @test id=aot
 * @summary test the -XX:CacheOnlyClassesIn flag with JEP 483t workflow
 * @requires vm.cds
 * @requires vm.cds.write.archived.java.heap
 * @library /test/lib
 * @run driver CacheOnlyClassesIn AOT
 */

import java.io.File;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class CacheOnlyClassesIn {
    static String mainClass = CacheOnlyClassesInApp.class.getName();
    static String appJar, testJar;

    public static void main(String args[]) throws Exception {
        appJar = ClassFileInstaller.writeJar("app.jar",
                                             "CacheOnlyClassesInApp");
        testJar = ClassFileInstaller.writeJar("test.jar",
                                             "TestHarness");
        MyTester tester = new MyTester();
        tester.run(args);
    }

    static class MyTester extends CDSAppTester {
        public MyTester() {
            super("CacheOnlyClassesInApp");
        }

        @Override
        public String classpath(RunMode runMode) {
            return appJar + File.pathSeparator + testJar;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-XX:CacheOnlyClassesIn=" + appJar,
            };
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                mainClass,
            };
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) throws Exception {
            if (runMode == RunMode.DUMP_STATIC ||
                runMode == RunMode.DUMP_DYNAMIC ||
                runMode == RunMode.TRAINING) {
                out.shouldContain("Skipping TestHarness: excluded via -XX:CacheOnlyClassesIn");
            }
        }
    }
}

class CacheOnlyClassesInApp {
    public static void main(String args[]) {
        System.out.println("Hello World = " + TestHarness.doit());
    }

    static volatile int x;
    public static int testLoop() {
        for (int i = 0; i < 100000000; i++) {
            x ^= (x + 39);
        }
        return x;
    }
}

class TestHarness {
    static int doit() {
        return CacheOnlyClassesInApp.testLoop() + myTestLoop();
    }

    static volatile int x;
    public static int myTestLoop() {
        for (int i = 0; i < 100000000; i++) {
            x ^= (x + 39);
        }
        return x;
    }
}
