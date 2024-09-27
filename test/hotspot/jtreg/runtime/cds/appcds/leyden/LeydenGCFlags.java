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
 * @summary CDS should fail to load if production time GC flags do not match training run.
 * @requires vm.flagless
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenGCFlags
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar HelloApp
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI LeydenGCFlags
 */

import jdk.test.whitebox.WhiteBox;
import jdk.test.whitebox.gc.GC;

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.StringArrayUtils;

public class LeydenGCFlags {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass = "HelloApp";
    static final String ERROR_GC_SUPPORTED = "Cannot create the CacheDataStore: UseCompressedClassPointers must be enabled, and collector must be G1, Parallel, Serial, Epsilon, or Shenandoah";
    static final String ERROR_GC_MISMATCH = "CDS archive has aot-linked classes. It cannot be used because GC used during dump time (.*) is not the same as runtime (.*)";
    static final String ERROR_COOP_MISMATCH = "Disable Startup Code Cache: 'HelloApp.cds.code' was created with CompressedOops::shift.. = .* vs current .*";

    static String trainingArgs[];
    static String productionArgs[];
    static boolean shouldFailDump;
    static boolean shouldFailRun;
    static String productFailPattern;

    public static void main(String[] args) throws Exception {
        if (GC.Z.isSupported()) {
            // ZGC not supported for now
            fail_dump("-XX:+UseZGC",  "-Xmx8g",  ERROR_GC_SUPPORTED);
        }

        // Serial, Parallel and Shenandoah collectors are allowed to be used,
        // as long as the same one is used between training and production
        if (GC.Serial.isSupported()) {
            good("-XX:+UseSerialGC",   "-XX:+UseSerialGC");
        }
        if (GC.Parallel.isSupported()) {
            good("-XX:+UseParallelGC", "-XX:+UseParallelGC");
        }
        if (GC.Shenandoah.isSupported()) {
            good("-XX:+UseShenandoahGC", "-XX:+UseShenandoahGC");
        }

        // Fail if production uses a different collector than training
        if (GC.Parallel.isSupported() && GC.G1.isSupported()) {
            fail_run("-XX:+UseParallelGC", "-XX:+UseG1GC",        ERROR_GC_MISMATCH );
        }
        if (GC.Parallel.isSupported()) {
            fail_run(null,                 "-XX:+UseParallelGC",  ERROR_GC_MISMATCH );
        }

       if (false) { // Disabled for now, as on MacOS we cannot guarantee to get differnt coop encodings
        // Different oop encodings
        fail_run(array("-XX:-UseCompatibleCompressedOops", "-Xmx128m"), 
                 array("-XX:-UseCompatibleCompressedOops","-Xmx8g"),
                 ERROR_COOP_MISMATCH);
        fail_run(array("-XX:-UseCompatibleCompressedOops", "-Xmx8g"),
                 array("-XX:-UseCompatibleCompressedOops","-Xmx128m"),
                 ERROR_COOP_MISMATCH);
       }

        // FIXME -- this causes
        // java.lang.invoke.WrongMethodTypeException: handle's method type ()Object but found ()Object
     /* fail_run(null,                 "NoSuchApp"); */
    }

    static String[] array(String... strings) {
        return strings;
    }
    static void good(Object t, Object p) throws Exception {
        shouldFailDump = false;
        shouldFailRun = false;
        run(t, p);
    }

    static void fail_dump(Object t, Object p, String regexp) throws Exception {
        shouldFailDump = true;
        shouldFailRun  = true;
        productFailPattern = regexp;
        trainingArgs = makeArgs(t);
        productionArgs = makeArgs(p);
        Tester tester = new Tester();
        tester.setCheckExitValue(false);
        tester.run(new String[] {"LEYDEN_TRAINONLY"} );
    }

    static void fail_run(Object t, Object p, String regexp) throws Exception {
        shouldFailDump = false;
        shouldFailRun  = true;
        productFailPattern = regexp;
        run(t, p);
    }

    static void run(Object t, Object p) throws Exception {
        trainingArgs = makeArgs(t);
        productionArgs = makeArgs(p);
        Tester tester = new Tester();
        tester.run(new String[] {"LEYDEN"} );
    }

    static String[] makeArgs(Object o) {
        if (o == null) {
            return new String[0];
        }
        if (o instanceof String[]) {
            return (String[])o;
        } else {
            return new String[] { (String)o };
        }
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
        public String[] appCommandLine(RunMode runMode) {
            if (runMode.isProductionRun()) {
                return StringArrayUtils.concat(productionArgs, "-Xshare:auto", mainClass);
            } else {
                return StringArrayUtils.concat(trainingArgs, mainClass);
            }
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            if (shouldFailDump) {
                if (runMode != RunMode.PRODUCTION) {
                    out.shouldMatch(productFailPattern);
                    out.shouldHaveExitValue(1);
                }
            } else if (shouldFailRun) {
                if (runMode == RunMode.PRODUCTION) {
                    out.shouldMatch(productFailPattern);
                    //out.shouldHaveExitValue(1); TODO VM should enable -Xshare:on by default
                }
            } else {
                if (runMode != RunMode.TRAINING1) {
                    out.shouldContain("Hello Leyden");
                }

                if (runMode == RunMode.TRAINING || runMode == RunMode.TRAINING1) {
                    // We should dump the heap even if the collector is not G1
                    out.shouldContain("Shared file region (hp)");
                }

            }
        }
    }
}

class HelloApp {
    public static void main(String args[]) {
        System.out.println("Hello Leyden");
    }
}
