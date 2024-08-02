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

// AOT-linked classes are loaded into the VM early, before the module system is initialized. Make sure
// that the Module, Package, CodeSource and ProtectionDomain of these classes are set up properly.

/*
 * @test id=static
 * @requires vm.cds.supports.aot.class.linking
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build EarlyClassLoading
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar EarlyClassLoadingApp.jar EarlyClassLoadingApp
 * @run driver EarlyClassLoading STATIC
 */

/*
 * @test id=dynamic
 * @requires vm.cds.supports.aot.class.linking
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build EarlyClassLoading
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar EarlyClassLoadingApp.jar EarlyClassLoadingApp
 * @run driver EarlyClassLoading DYNAMIC
 */

import java.io.File;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class EarlyClassLoading {
    static final String appJar = ClassFileInstaller.getJarPath("EarlyClassLoadingApp.jar");
    static final String mainClass = "EarlyClassLoadingApp";

    public static void main(String[] args) throws Exception {
        test(args, true);
        if (args[0].equals("STATIC")) {
            test(args, false);
        }
    }

    static void test(String[] args, boolean archivePackageOopssAndProtectionDomains) throws Exception {
        Tester t = new Tester(archivePackageOopssAndProtectionDomains);

        // Run with archived FMG loaded
        t.run(args);

        // Run with an extra classpath -- archived FMG can still load.
        {
            String extraVmArgs[] = {
                "-cp",
                appJar + File.pathSeparator + "foobar.jar"
            };
            OutputAnalyzer out = t.productionRun(extraVmArgs);
            out.shouldHaveExitValue(0);
        }

        // Run without archived FMG -- fail to load
        {
            String extraVmArgs[] = {
                "-Xshare:on",
                "-Djdk.module.showModuleResolution=true"
            };
            t.setCheckExitValue(false);
            OutputAnalyzer out = t.productionRun(extraVmArgs);
            out.shouldHaveExitValue(1);
            out.shouldContain("CDS archive has preloaded classes. It cannot be used when archived full module graph is not used.");
            t.setCheckExitValue(true);
        }
    }

    static class Tester extends CDSAppTester {
        boolean archivePackageOopssAndProtectionDomains;
        public Tester(boolean archivePackageOopssAndProtectionDomains) {
            super(mainClass);
            this.archivePackageOopssAndProtectionDomains = archivePackageOopssAndProtectionDomains;
        }

        @Override
        public String classpath(RunMode runMode) {
            return appJar;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            String which =  archivePackageOopssAndProtectionDomains ? "+" : "-";
            return new String[] {
                "-XX:+PreloadSharedClasses",
                "-XX:" + which + "ArchivePackages",
                "-XX:" + which + "ArchiveProtectionDomains"
            };
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                mainClass,
            };
        }
    }
}

// FIXME -- test hidden classes in boot2 (with a lambda expr in boot classpath)
// FIXME -- test hidden classes in app (with a lambda expr in app classpath)

class EarlyClassLoadingApp {
    static String allPerms = "null.*<no principals>.*java.security.Permissions.*,*java.security.AllPermission.*<all permissions>.*<all actions>";

    public static void main(String args[]) throws Exception {
        check(String.class,
              "null",  // loader
              "module java.base",
              "package java.lang",
              "null",
              allPerms);

        check(Class.forName("sun.util.logging.internal.LoggingProviderImpl"),
              "null",
              "module java.logging",
              "package sun.util.logging.internal",
              "null",
              allPerms);


        check(javax.tools.FileObject.class,
              "^jdk.internal.loader.ClassLoaders[$]PlatformClassLoader@",
              "module java.compiler",
              "package javax.tools",
              "jrt:/java.compiler <no signer certificates>",
              "jdk.internal.loader.ClassLoaders[$]PlatformClassLoader.*<no principals>.*java.security.Permissions.*"
              + "java.lang.RuntimePermission.*accessSystemModules");

        check(EarlyClassLoadingApp.class,
              "jdk.internal.loader.ClassLoaders[$]AppClassLoader@",
              "^unnamed module @",
              "package ",
              "file:.*EarlyClassLoadingApp.jar <no signer certificates>",
              "jdk.internal.loader.ClassLoaders[$]AppClassLoader.*<no principals>.*java.security.Permissions.*"
              + "java.io.FilePermission.*EarlyClassLoadingApp.jar.*read");

        check(Class.forName("com.sun.tools.javac.Main"),
              "jdk.internal.loader.ClassLoaders[$]AppClassLoader@",
              "module jdk.compiler",
              "package com.sun.tools.javac",
              "jrt:/jdk.compiler <no signer certificates>",
              "jdk.internal.loader.ClassLoaders[$]AppClassLoader.*<no principals>.*java.security.Permissions.*"
              + "java.lang.RuntimePermission.*accessSystemModules");
    }

    static void check(Class c, String loader, String module, String pkg, String codeSource, String protectionDomain) {
        System.out.println("====================================================================");
        System.out.println(c.getName() + ", loader  = " + c.getClassLoader());
        System.out.println(c.getName() + ", module  = " + c.getModule());
        System.out.println(c.getName() + ", package = " + c.getPackage());
        System.out.println(c.getName() + ", CS      = " + c.getProtectionDomain().getCodeSource());
        System.out.println(c.getName() + ", PD      = " + c.getProtectionDomain());

        expectMatch("" + c.getClassLoader(), loader);
        expectMatch("" + c.getModule(), module);
        expectSame("" + c.getPackage(), pkg);
        expectMatch("" + c.getProtectionDomain().getCodeSource(), codeSource);
        expectMatch("" + c.getProtectionDomain(), protectionDomain);
    }

    static void expectSame(String a, String b) {
        if (!a.equals(b)) {
            throw new RuntimeException("Expected \"" + b + "\" but got \"" + a + "\"");
        }
    }
    static void expectMatch(String string, String pattern) {
        Matcher matcher = Pattern.compile(pattern, Pattern.DOTALL).matcher(string);
        if (!matcher.find()) {
            throw new RuntimeException("Expected pattern \"" + pattern + "\" but got \"" + string + "\"");
        }
    }
}
