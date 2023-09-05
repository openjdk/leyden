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
 * @build OldInf
 * @build IndyMiscTests
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LambdaWithOldIntf OldInf
 * @run driver IndyMiscTests
 */


// NOTE: to run a subset of the tests, use something like
//
// jtreg .... -vmoptions:-DIndyMiscTests.test.only='(1)|(2)' \
//            -vmoptions:-DIndyMiscTests.test.skip='2' IndyMiscTests.java
//
// A regexp can be specified in these two properties. Note that the specified regexp must be a full match.
// E.g., -DIndyMiscTests.test.only='1.*' matches "1" and "12", but does NOT match "21".
// (Search for ")$" in the code below)
//
// Also, some tests may be forced to be skipped. To run them, edit the variable forceSkip below.

import jdk.test.lib.helpers.ClassFileInstaller;

public class IndyMiscTests extends IndyTestBase {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass_LambdaWithOldIntf = LambdaWithOldIntf.class.getName();

    // Edit the following to disable some tests during development.
    static String forceSkip = null;

    public static void main(String[] args) throws Exception {
        setup(forceSkip, appJar);

        // ------------------------------------------------------------
        test("LambdaWithOldIntf", mainClass_LambdaWithOldIntf, "");
        checkExec("xxMy StringXX");
    }
}


class LambdaWithOldIntf {
    public static void main(String args[]) throws Exception {
        test(() -> "My String");
    }

    static void test(OldInf i) {
        System.out.print("OUTPUT = xx");
        System.out.print(i.doit());
        System.out.println("XX");
    }
}

