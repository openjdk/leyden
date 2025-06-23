/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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
 * @summary interation between static archive and dynamic archive related to
 *          regenerated lambda form invoker classes.
 * @requires vm.cds
 * @requires vm.cds.supports.aot.class.linking
 * @library /test/lib
 * @build RegeneratedClassesInDynamicArchive
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar TestApp CachedInDynamic MyInterface
 * @run driver RegeneratedClassesInDynamicArchive
 */

import java.util.List;
import java.util.stream.Collectors;

import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class RegeneratedClassesInDynamicArchive {
    static String appJar = ClassFileInstaller.getJarPath("app.jar");
    static String aotConfigFile = "app.aotconfig";
    static String aotCacheFile = "app.aot";
    static String appClass = TestApp.class.getName();
    static String dynamicArchive = "dynamic.jsa";
    static String helloMsg = "hello, world";
    static String outputWithDynamicArchive = "Hello 1 in 20.0";

    public static void main(String[] args) throws Exception {
        //----------------------------------------------------------------------
        printTestCase("Training Run For Base Archive");
        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(
            "-XX:AOTMode=record",
            "-XX:AOTConfiguration=" + aotConfigFile,
            "-Xlog:aot=debug",
            "-cp", appJar, appClass, "base");

        OutputAnalyzer out = CDSTestUtils.executeAndLog(pb, "train");
        out.shouldContain("AOTConfiguration recorded: " + aotConfigFile);
        out.shouldContain(helloMsg);
        out.shouldNotContain(outputWithDynamicArchive);
        out.shouldHaveExitValue(0);

        test(false);
        test(true);
    }

    static void test(boolean aotClassLinkingForBaseArchive) throws Exception {
        ProcessBuilder pb;
        OutputAnalyzer out;
        String usingAOTClassesMsg = "Using AOT-linked classes: true (static archive: has aot-linked classes)";
        String regenerateMsg = "Regenerate MethodHandle Holder classes...done";

        //----------------------------------------------------------------------
        printTestCase("Assembly Phase");
        pb = ProcessTools.createLimitedTestJavaProcessBuilder(
            "-XX:AOTMode=create",
            "-XX:AOTConfiguration=" + aotConfigFile,
            "-XX:AOTCache=" + aotCacheFile,
            "-XX:" + (aotClassLinkingForBaseArchive ? "+" : "-") + "AOTClassLinking",
            "-Xlog:aot=debug,aot+class=debug",
            "-cp", appJar);
        out = CDSTestUtils.executeAndLog(pb, "asm");
        out.shouldContain("AOTCache creation is complete");
        out.shouldMatch("aot,class.* TestApp");
        out.shouldNotMatch("aot,class.* CachedInDynamic");
        out.shouldHaveExitValue(0);

        //----------------------------------------------------------------------
        printTestCase("Production Run with AOTCache, to Generate " + dynamicArchive);
        pb = ProcessTools.createLimitedTestJavaProcessBuilder(
            "-XX:AOTCache=" + aotCacheFile,
            "-Xlog:aot=debug,aot+class=debug,class+load",
            "-Xlog:aot+resolve=trace",
            "-XX:+AOTClassLinking",
            "-XX:ArchiveClassesAtExit=" + dynamicArchive,
            "-cp", appJar, appClass, "top");
        out = CDSTestUtils.executeAndLog(pb, "dyndump");
        if (aotClassLinkingForBaseArchive) {
            out.shouldContain(usingAOTClassesMsg);
            out.shouldNotContain(regenerateMsg);
        } else {
            out.shouldNotContain(usingAOTClassesMsg);
            out.shouldContain(regenerateMsg);
        }
        out.shouldContain("Opened AOT cache app.aot.");
        out.shouldContain(helloMsg);
        out.shouldContain(outputWithDynamicArchive);
        out.shouldHaveExitValue(0);

        //----------------------------------------------------------------------
        printTestCase("Production Run with " + dynamicArchive);
        pb = ProcessTools.createLimitedTestJavaProcessBuilder(
            "-XX:SharedArchiveFile=" + dynamicArchive,
            "-Xlog:aot,class+load",
            "-cp", appJar, appClass, "top");
        out = CDSTestUtils.executeAndLog(pb, "prod");
        out.shouldContain(helloMsg);
        out.shouldContain(outputWithDynamicArchive);
        out.shouldContain("CachedInDynamic source: shared objects file (top)");
        out.shouldHaveExitValue(0);
    }

    static int testNum = 0;
    static void printTestCase(String s) {
        System.out.println("vvvvvvv TEST CASE " + testNum + ": " + s + ": starts here vvvvvvv");
        testNum++;
    }
}

class TestApp {
    public static void main(String args[]) {
        // Run a few lambdas -- if -XX:+AOTClassLinking is enabled,
        // these will be cached. with AOT-resolved references to the lambda form invoker
        // classes. These references should still work when dynamic.jsa is loaded.
        var words = List.of("hello", "fuzzy", "world");
        var greeting = words.stream()
            .filter(w -> !w.contains("z"))
            .collect(Collectors.joining(", "));
        System.out.println(greeting);  // hello, world
        if (args[0].equals("top")) {
            CachedInDynamic.func();
            CachedInDynamic.func3();
            CachedInDynamic.func2();
        }
    }
}

interface MyInterface {
    public Object doit(Object a, long b, Object c, int d, double e);
}

class CachedInDynamic extends TestApp {
    static void func() {
        System.out.println("CachedInDynamic.func(): start");
        MyInterface m = (a, b, c, d, e) -> {
            return "" + a + b + c + d + e;
        };
        System.out.println(m.doit("Hello ", 1, " in ", 2, 0.0));
        System.out.println("CachedInDynamic.func(): done");
    }
    
    TestApp[][] array2;

    static TestApp[][] array3;

    static void func2() {
        // Create the TestApp[] array class in the dynamic archive
        CachedInDynamic c = new CachedInDynamic();
        c.array2 = new TestApp[1][1];
        Module m0 = TestApp.class.getModule();
        Module m1 = c.array2.getClass().getModule();
        System.out.println(c.array2.getClass());
        System.out.println(m0);
        System.out.println(m1);
        if (m0 != m1) {
            throw new RuntimeException("Module of TestApp and TestApp[] classes do not match");
        }
    }

    static void func3() {
        array3 = new TestApp[1][1];
        Module m0 = TestApp.class.getModule();
        Module m1 = array3.getClass().getModule();
        System.out.println(array3.getClass());
        System.out.println(m0);
        System.out.println(m1);
        if (m0 != m1) {
            throw new RuntimeException("Module of TestApp and TestApp[] classes do not match");
        }
    }
}
