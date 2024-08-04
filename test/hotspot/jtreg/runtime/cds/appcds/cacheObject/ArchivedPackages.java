/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 *          /test/hotspot/jtreg/runtime/cds/appcds/test-classes
 * @build ArchivedPackages
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar ArchivedPackagesApp pkg3.Package3A pkg3.Package3B
 * @run driver ArchivedPackages STATIC
 */

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

import pkg3.Package3A;
import pkg3.Package3B;

public class ArchivedPackages {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass = "ArchivedPackagesApp";

    public static void main(String[] args) throws Exception {
        Tester t = new Tester();
        t.run(args);
    }

    static class Tester extends CDSAppTester {
        public Tester() {
            super(mainClass);
        }

        @Override
        public String classpath(RunMode runMode) {
            return appJar;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-Dcds.debug.archived.packages=true",
                "-XX:+AOTClassLinking",
                "-XX:+ArchivePackages"
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
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            if (runMode.isStaticDump()) {
                out.shouldContain("Archiving NamedPackage \"\" for jdk.internal.loader.ClassLoaders$AppClassLoader");
                out.shouldContain("Archiving Package \"pkg3\" for jdk.internal.loader.ClassLoaders$AppClassLoader");
                out.shouldContain("Archiving NamedPackage \"com.sun.tools.javac\" for jdk.internal.loader.ClassLoaders$AppClassLoader");
            } else if (runMode.isProductionRun()) {
                out.shouldContain(ArchivedPackagesApp.msg);
            }
        }
    }
}

class ArchivedPackagesApp {
    public static String msg = "Package for archived class equals to that of non-archived class";
    public static void main(String args[]) throws Exception {
        Class.forName("com.sun.tools.javac.Main");
        Package x = Package3A.class.getPackage();
        if (args[0].equals("PRODUCTION")) {
            Package y = Package3B.class.getPackage();
            System.out.println("Package for archived class: " + x);
            System.out.println("Package for non-archived class: " + y);
            if (x == y) {
                System.out.println(msg);
            } else {
                throw new RuntimeException("Unexpected for archived package");
            }
        }
    }
}
