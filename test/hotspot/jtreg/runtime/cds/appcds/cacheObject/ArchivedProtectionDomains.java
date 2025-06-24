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

/*
 * @test
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 *          /test/hotspot/jtreg/runtime/cds/appcds/test-classes
 * @build ArchivedProtectionDomains
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app1.jar ArchivedProtectionDomainsApp
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app2.jar pkg3.Package3A pkg3.Package3B
 * @run driver ArchivedProtectionDomains STATIC
 */

import java.io.File;
import java.security.ProtectionDomain;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.StringArrayUtils;

import pkg3.Package3A;
import pkg3.Package3B;

public class ArchivedProtectionDomains {
    static final String app1Jar = ClassFileInstaller.getJarPath("app1.jar");
    static final String app2Jar = ClassFileInstaller.getJarPath("app2.jar");
    static final String mainClass = "ArchivedProtectionDomainsApp";

    public static void main(String[] args) throws Exception {
        Tester t = new Tester();
        t.run(new String[] {args[0]});
    }

    static class Tester extends CDSAppTester {
        public Tester() {
            super(mainClass);
        }

        @Override
        public String classpath(RunMode runMode) {
            return app1Jar + File.pathSeparator + app2Jar;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            String[] args = StringArrayUtils.concat(
                "-Dcds.debug.archived.protection.domains=true",
                "-XX:+AOTClassLinking",
                "-XX:+ArchiveProtectionDomains",
                "-Xlog:cds+protectiondomain");
            return args;
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
            assertArchived(out, runMode);
        }

        void assertArchived(OutputAnalyzer out, RunMode runMode) {
            if (runMode.isStaticDump()) {
                out.shouldMatch("Archiving ProtectionDomain .jrt:/jdk.compiler .* for jdk.internal.loader.ClassLoaders.AppClassLoader");
                out.shouldMatch("Archiving ProtectionDomain .*app1.jar .* for jdk.internal.loader.ClassLoaders.AppClassLoader");
                out.shouldMatch("Archiving ProtectionDomain .*app2.jar .* for jdk.internal.loader.ClassLoaders.AppClassLoader");
            } else if (runMode.isProductionRun()) {
                out.shouldContain(ArchivedProtectionDomainsApp.msg);
            }
        }
    }
}

class ArchivedProtectionDomainsApp {
    public static String msg = "ProtectionDomain for archived class equals to that of non-archived class";
    public static void main(String args[]) throws Exception {
        Class jc = Class.forName("com.sun.tools.javac.Main");
        ProtectionDomain x = Package3A.class.getProtectionDomain(); // This class is in the AOT cache
        if (args[0].equals("PRODUCTION")) {
            ProtectionDomain y = Package3B.class.getProtectionDomain(); // This class is not in the AOT cache
            System.out.println("ProtectionDomain for com.sun.tools.javac.Main: " + jc.getProtectionDomain());
            System.out.println("ProtectionDomain for archived class: " + x);
            System.out.println("ProtectionDomain for non-archived class: " + y);
            if (x == y) {
                // Both classes should have the same pd (this is enforced by SecureClassLoader::pdcache)
                System.out.println(msg);
            } else {
                throw new RuntimeException("ProtectionDomain should be the same for classes in the same package, whether they are archived or not");
            }
        }
    }
}
