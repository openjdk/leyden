/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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
 * @test id=no_trigger
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenEndTrainingTrigger
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenEndTrainingTriggerApp
 * @run driver LeydenEndTrainingTrigger LEYDEN
 */

/*
 * @test id=trigger
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenEndTrainingTrigger
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenEndTrainingTriggerApp
 * @run driver LeydenEndTrainingTrigger LeydenEndTrainingTriggerApp.triggerMethod LEYDEN
 */

/*
 * @test id=trigger_count_1
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenEndTrainingTrigger
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenEndTrainingTriggerApp
 * @run driver LeydenEndTrainingTrigger LeydenEndTrainingTriggerApp.triggerMethod 1 LEYDEN
 */

/*
 * @test id=trigger_count_2
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenEndTrainingTrigger
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenEndTrainingTriggerApp
 * @run driver LeydenEndTrainingTrigger LeydenEndTrainingTriggerApp.triggerMethod 2 LEYDEN
 */

/*
 * @test id=trigger_count_3
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenEndTrainingTrigger
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenEndTrainingTriggerApp
 * @run driver LeydenEndTrainingTrigger LeydenEndTrainingTriggerApp.triggerMethod 3 LEYDEN
 */

/*
 * @test id=trigger_count_99
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenEndTrainingTrigger
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenEndTrainingTriggerApp
 * @run driver LeydenEndTrainingTrigger LeydenEndTrainingTriggerApp.triggerMethod 99 LEYDEN
 */

import java.util.Arrays;

import jdk.test.lib.Asserts;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class LeydenEndTrainingTrigger {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass = "LeydenEndTrainingTriggerApp";
    static final String vmCmdPrefix = "-XX:AOTEndTrainingOnMethodEntry=";
    static String triggerMethod = "";
    static int count = 0;

    public static void main(String[] args) throws Exception {
        Tester t = new Tester();
        if (args.length > 1) {
            triggerMethod = vmCmdPrefix + args[0];
            args = Arrays.copyOfRange(args, 1, args.length);
        }
        if (args.length == 2) {
            triggerMethod += ",count=" + args[0];
            count = Integer.parseInt(args[0]);
            args = Arrays.copyOfRange(args, 1, args.length);
        }
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
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                mainClass, runMode.name()
            };
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            if (runMode == RunMode.TRAINING && triggerMethod.length() > 0) {
                return new String[] { triggerMethod };
            }
            return new String[] {};
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            if (!runMode.isStaticDump()) {
                // always expect this output
                out.shouldContain("LeydenEndTrainingTriggerApp_line_1");
                out.shouldContain("LeydenEndTrainingTriggerApp_line_2");
                out.shouldContain("LeydenEndTrainingTriggerApp_line_3");
            }
            if (runMode == RunMode.TRAINING) {
                // in training mode we expect the dump to be triggered
                // count controls where in the application output the dump occurs
                // so we test that the counts worked as expected (even the case where no trigger was specified)
                var lines = out.asLines();
                // find the line number that contains "CacheDataStore dumping is complete"
                var dumpComplete = lines.indexOf("CacheDataStore dumping is complete");
                Asserts.assertNE(dumpComplete, -1, "CacheDataStore dumping is complete not found");
                var line1 = lines.indexOf("LeydenEndTrainingTriggerApp_line_1");
                var line2 = lines.indexOf("LeydenEndTrainingTriggerApp_line_2");
                var line3 = lines.indexOf("LeydenEndTrainingTriggerApp_line_3");

                if (triggerMethod.length() == 0) {
                    // no trigger, so dump should run after execution
                    Asserts.assertGT(dumpComplete, line3, "dump ");
                } else {
                    switch(count) {
                        case 0:
                            // no count specified, default is 1 (ie first trigger)
                            Asserts.assertLT(dumpComplete, line1, "dumpComplete should be before line 1 of application output");
                            break;
                        case 1:
                            Asserts.assertLT(dumpComplete, line1, "dumpComplete should be before line 1 of application output");
                            break;
                        case 2:
                            Asserts.assertGT(dumpComplete, line1, "dumpComplete should be after line 1 of application output");
                            Asserts.assertLT(dumpComplete, line2, "dumpComplete should be before line 2 of application output");
                            break;
                        case 3:
                            Asserts.assertGT(dumpComplete, line2, "dumpComplete should be after line 2 of application output");
                            Asserts.assertLT(dumpComplete, line3, "dumpComplete should be before line 3 of application output");
                            break;
                        default:
                            Asserts.assertGT(dumpComplete, line3, "dumpComplete should be after line 3 of application output");
                            break;
                    }
                }
            }
        }
    }
}

class LeydenEndTrainingTriggerApp {
    public static void triggerMethod(String text) {
        System.out.println("LeydenEndTrainingTriggerApp_line_" + text);
    }
    public static void main(String args[]) {
        triggerMethod("1");
        triggerMethod("2");
        triggerMethod("3");
    }
}
