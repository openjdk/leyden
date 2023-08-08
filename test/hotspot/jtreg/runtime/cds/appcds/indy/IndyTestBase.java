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

import java.io.FileWriter;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import jdk.test.lib.cds.CDSOptions;
import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;


public class IndyTestBase {
    static Pattern testOnlyPattern = null; // matches testNumber
    static Pattern testSkipPattern = null; // matches testNumber    
    static Pattern forceSkipPattern = null; // Force some tests to be disabled during development.
    static int testNumber = 0;
    static String appJar;
    static OutputAnalyzer output = null;
    static String jtregTestCase = "testcase";

    // bits for runMode
    static final int RUN_STATIC = 1;   // Run with static archive only (no AOT)
    static final int RUN_AOT    = 2;   // Run with AOT (which uses dynamic archive)
    static final int RUN_BENCH  = 4;   // Run in benchmark mode (no logs)

    static void setup(String appJar) throws Exception {
      setup(null, appJar);
    }

    static void setup(String forceSkip, String appJar) throws Exception {
        String testName = System.getProperty("test.name");
        if (testName != null) {
            Path p = Paths.get(testName);
            String file = p.getFileName().toString();
            int i = file.indexOf(".");
            jtregTestCase = file.substring(0, i);
        }
        IndyTestBase.appJar = appJar;
        String testOnly = System.getProperty(jtregTestCase + ".test.only");
        if (testOnly != null) {
            testOnlyPattern = Pattern.compile("^(" + testOnly + ")$");
        }
        String testSkip = System.getProperty(jtregTestCase + ".test.skip");
        if (testSkip != null) {
            testSkipPattern = Pattern.compile("^(" + testSkip + ")$");
        }

        if (forceSkip != null) {
            forceSkipPattern = Pattern.compile("^(" + forceSkip + ")$");
        }
    }

    static boolean shouldTest(String s) {
        if (testOnlyPattern != null) {
            Matcher matcher = testOnlyPattern.matcher(s);
            if (!matcher.find()) {
                return false;
            }
        }
        if (testSkipPattern != null) {
            Matcher matcher = testSkipPattern.matcher(s);
            if (matcher.find()) {
                return false;
            }
        }

        return true;
    }

    // Run the test program with the same arg for both training run and production run
    static void test(String testNote, String mainClass, String arg) throws Exception {
        test(testNote, mainClass, arg, arg, RUN_STATIC);
    }

    static void test(String testNote, String mainClass, String trainingArg, String productionArg) throws Exception {
        test(testNote, mainClass, trainingArg, productionArg, RUN_STATIC);
    }

    static void test(String testNote, String mainClass, String trainingArg, String productionArg, int runMode) throws Exception {
        output = null;
        testNumber ++;
        String skipBy = null;

        if (forceSkipPattern != null) {
            Matcher matcher = forceSkipPattern.matcher(testNote);
            if (matcher.find()) {
                skipBy = " ***** (hard coded) Skipped by test note";
            }
        }
        if (skipBy == null && !shouldTest("" + testNumber)) {
            skipBy = " ***** Skipped by test number";
        }

        if (skipBy != null) {
            System.out.println("         Test : #" + testNumber + ", " + testNote + skipBy);
            return;
        }

        System.out.println("==================================================================");
        System.out.println("         Test : #" + testNumber + ", " + testNote);
        System.out.println("  trainingArg : " + trainingArg);
        System.out.println("productionArg : " + productionArg);
        System.out.println("      runMode : " + runModeString(runMode));
        System.out.println("vvvv==========================================================vvvv");
        String s = jtregTestCase + "-" + testNumber;
        String classList = s + ".classlist";
        String archiveName = s + ".jsa";

        // Create classlist
        System.out.println("#" + testNumber + ": Create classlist");
        CDSTestUtils.dumpClassList(classList, "-cp", appJar, mainClass, trainingArg);

        // Dump archive
        System.out.println("#" + testNumber + ": Dump static archive");
        CDSOptions opts = (new CDSOptions())
            .addPrefix("-XX:SharedClassListFile=" + classList,
                       "-XX:+ArchiveInvokeDynamic",
                       "-cp", appJar,
                       "-Xlog:cds+heap",
                       "-Xlog:cds+resolve=trace",
                       "-Xlog:cds,cds+class=debug")
            .setArchiveName(archiveName);
        output = CDSTestUtils.createArchiveAndCheck(opts);
        TestCommon.checkExecReturn(output, 0, true);

        if ((runMode & RUN_STATIC) != 0) {
            // Run with static archive
            System.out.println("#" + testNumber + ": Run with static archive (no AOT)");
            CDSOptions runOpts = (new CDSOptions())
                .addPrefix("-cp", appJar)
                .setArchiveName(archiveName)
                .setUseVersion(false)
                .addSuffix(mainClass)
                .addSuffix(productionArg);
            if ((runMode & RUN_BENCH) != 0) {
                runOpts.setBenchmarkMode(true);
            } else {
                runOpts.addPrefix("-Xlog:class+load", "-Xlog:cds=debug", "-Xlog:cds+heap=debug",
                                  "-Xlog:methodhandles");
            }
            output = CDSTestUtils.runWithArchive(runOpts);
            TestCommon.checkExecReturn(output, 0, true);
        }

        if ((runMode & RUN_AOT) != 0) {
            System.out.println("#" + testNumber +
                               ": (STEP 3 of 5) Run with static archive and dump profile in dynamic archive (With Training Data Replay)");
            String dynamicArchiveName = s + "-dyn.jsa";
            CDSOptions dynDumpOpts = (new CDSOptions())
                .addPrefix("-cp", appJar)
                .setArchiveName(archiveName)
                .setUseVersion(false)
                .addSuffix("-XX:ArchiveClassesAtExit=" + dynamicArchiveName)
                .addSuffix("-XX:+RecordTraining");
            if ((runMode & RUN_BENCH) != 0) {
                dynDumpOpts.setBenchmarkMode(true);
                dynDumpOpts.addPrefix("-Xlog:cds=debug", "-Xlog:sca");
            } else {
                dynDumpOpts.addPrefix("-Xlog:class+load", "-Xlog:cds=debug",
                                      "-Xlog:cds+class=debug");
            }
            // The main class name and arguments
            dynDumpOpts
                .addSuffix(mainClass)
                .addSuffix(productionArg);
            output = CDSTestUtils.runWithArchive(dynDumpOpts);
            TestCommon.checkExecReturn(output, 0, true);

            //======================================================================
            System.out.println("#" + testNumber +
                               ": (STEP 4 of 5) Run with dynamic archive and generate AOT code");
            String sharedCodeArchive = s + ".sca";
            CDSOptions aotOpts = (new CDSOptions())
                .addPrefix("-cp", appJar)
                .setArchiveName(archiveName)
                .setUseVersion(false)
                .addSuffix("-XX:+StoreSharedCode")
                .addSuffix("-XX:SharedCodeArchive=" + sharedCodeArchive)
                .addSuffix("-XX:ReservedSharedCodeSize=100M");

            if ((runMode & RUN_BENCH) != 0) {
                aotOpts.setBenchmarkMode(true);
                aotOpts.addPrefix("-Xlog:cds=debug", "-Xlog:sca");
            } else {
                // Tell CDSTestUtils to not add the -XX:VerifyArchivedFields=1 flag, which seems to be causing a crash
                // in the AOT code.
                aotOpts.setBenchmarkMode(true);

                aotOpts.addPrefix("-Xlog:class+load", "-Xlog:cds=debug", "-Xlog:sca*=trace");
                if (mainClass.equals("ConcatA")) {
                    // Hard-code the printing of loopA for now
                    dynDumpOpts
                        .addSuffix("-XX:CompileCommand=print,*::loopA")
                        .addSuffix("-XX:+PrintAssembly");
                }
            }
            // The main class name and arguments
            aotOpts
                .addSuffix(mainClass)
                .addSuffix(productionArg);
            output = CDSTestUtils.runWithArchive(aotOpts);
            TestCommon.checkExecReturn(output, 0, true);

            //======================================================================
            System.out.println("#" + testNumber + ": (STEP 5 of 5) Run with dynamic archive and AOT cache");
            CDSOptions runOpts = (new CDSOptions())
                .addPrefix("-cp", appJar)
                .setArchiveName(dynamicArchiveName)
                .addSuffix("-XX:+LoadSharedCode")
                .addSuffix("-XX:SharedCodeArchive=" + sharedCodeArchive)
                .setUseVersion(false)
                .addSuffix(mainClass)
                .addSuffix(productionArg);

            if ((runMode & RUN_BENCH) != 0) {
                runOpts.setBenchmarkMode(true);
            } else {
                // Tell CDSTestUtils to not add the -XX:VerifyArchivedFields=1 flag, which seems to be causing a crash
                // in the AOT code.
                runOpts.setBenchmarkMode(true);

                runOpts.addPrefix("-Xlog:class+load", "-Xlog:cds=debug");
                if (mainClass.equals("ConcatA")) {
                    // Hard-code the printing of loopA for now
                    dynDumpOpts
                        .addSuffix("-XX:CompileCommand=print,*::loopA")
                        .addSuffix("-XX:+PrintAssembly");
                }
            }

            output = CDSTestUtils.runWithArchive(runOpts);
            TestCommon.checkExecReturn(output, 0, true);
        }
    }

    static void checkExec(String expectedOutput) throws Exception {
        checkExec(expectedOutput, /* lambdaFormsMustBeArchived*/ true);
    }

    static void checkExec(String expectedOutput, boolean lambdaFormsMustBeArchived)  throws Exception {
        if (output == null) { // test may be skipped
            return;
        }
        if (expectedOutput != null) {
            TestCommon.checkExecReturn(output, 0, true, "OUTPUT = " + expectedOutput);
        }
        if (lambdaFormsMustBeArchived) {
            output.shouldMatch("LambdaForm[$]((MH)|(DMH))/0x[0-9]+ source: shared objects file");
            output.shouldNotMatch("LambdaForm[$]MH/0x[0-9]+ source: __JVM_LookupDefineClass__");
            output.shouldNotMatch("LambdaForm[$]DMH/0x[0-9]+ source: __JVM_LookupDefineClass__");
        }
    }

    static void shouldMatch(String pattern) throws Exception {
        if (output != null) { // test may be skipped
            output.shouldMatch(pattern);
        }
    }

    static String runModeString(int runMode) {
        String prefix = "";
        String s = "";
        if ((runMode & RUN_STATIC) != 0) {
            s += "RUN_STATIC";
            prefix = " | ";
        }
        if ((runMode & RUN_AOT) != 0) {
            s += prefix;
            s += "RUN_AOT";
            prefix = " | ";
        }
        return s;
    }
}
