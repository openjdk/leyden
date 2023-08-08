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
 * @summary Test for archiving resolved invokedynamic call sites
 * @requires vm.cds.write.archived.java.heap
 * @modules java.base/sun.invoke.util java.logging
 * @library /test/jdk/lib/testlibrary /test/lib
 *          /test/hotspot/jtreg/runtime/cds/appcds
 *          /test/hotspot/jtreg/runtime/cds/appcds/test-classes
 * @build IndyStringConcat
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar ConcatA ConcatA$DummyClass ConcatB
 * @run driver IndyStringConcat
 */


// NOTE: to run a subset of the tests, use something like
//
// jtreg .... -vmoptions:-DIndyStringConcat.test.only='(1)|(2)' \
//            -vmoptions:-DIndyStringConcat.test.skip='2' IndyStringConcat.java
//
// A regexp can be specified in these two properties. Note that the specified regexp must be a full match.
// E.g., -DIndyStringConcat.test.only='1.*' matches "1" and "12", but does NOT match "21".
// (Search for ")$" in the code below)
//
// Also, some tests may be forced to be skipped. To run them, edit the variable forceSkip below.

import jdk.test.lib.helpers.ClassFileInstaller;

public class IndyStringConcat extends IndyTestBase {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass_ConcatA = ConcatA.class.getName();
    static final String mainClass_ConcatB = ConcatB.class.getName();

    // Force some tests to be disabled during development.
    static String forceSkip = null;

    public static void main(String[] args) throws Exception {
        setup(forceSkip, appJar);

        // ------------------------------------------------------------
        test("\"LIT\" + (String)b", mainClass_ConcatA, "a");
        checkExec("LIT222");

        // ------------------------------------------------------------
        test("(String)a + (String)b", mainClass_ConcatA, "b");
        checkExec("aaa222");

        // ------------------------------------------------------------
        test("(String)a + (int)b", mainClass_ConcatA, "c");
        checkExec("aaa333");

        // ------------------------------------------------------------
        test("Test with ConcatB", mainClass_ConcatB, "B1");
        checkExec("ConcatBLIT333");

        // ------------------------------------------------------------
        test("Run ConcatB.foo() without dump-time resolution of its invokedynamic callsite", mainClass_ConcatB, "", "B1");
        checkExec("ConcatBLIT333", /* lambdaFormsMustBeArchived*/ false);

        // ------------------------------------------------------------
        test("WithAOT (no loop) for \"LIT\" + (String)b", mainClass_ConcatA, "a", "a",  RUN_AOT);
        checkExec("LIT222");
        shouldUseDynamicArchive();

        // ------------------------------------------------------------
        test("WithAOT (with loop) for \"LIT\" + (String)b", mainClass_ConcatA, "loopa", "loopa",  RUN_STATIC | RUN_AOT);
        checkExec("LITL");
        shouldUseDynamicArchive();
    }

    static void shouldUseDynamicArchive() throws Exception {
        shouldMatch("Opened archive IndyStringConcat-[0-9]+-dyn.jsa");
    }
}

class ConcatA {
    public static void main(String args[]) throws Exception {
        if (args[0].equals("a")) {
            foo("222");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("b")) {
            bar("aaa", "222");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("c")) {
            baz("aaa", 333);
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("loopa")) {
            loopa();
            loopa();
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        }

        if (args.length > 1 && args[1].equals("load-extra-class")) {
            // Work around "There is no class to be included in the dynamic archive." problem, where the
            // dynamic archive is not generated.
            DummyClass.doit();
        }
    }

    static void loopa() {
        for (int i = 0; i < 100000; i++) {
            foo("L");
        }
    }

    static String x;
    static void foo(String b) {
        x = "LIT" + b;
    }
    static void bar(String a, String b) {
        x = a + b;
    }
    static void baz(String a, int b) {
        x = a + b;
    }

    static class DummyClass {
        static void doit() {}
    }
}


class ConcatB {
    public static void main(String args[]) throws Exception {
        if (args[0].equals("B1")) {
            foo("333");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        }
    }

    static String x;
    static void foo(String b) {
        x = "ConcatBLIT" + b;
    }
}
