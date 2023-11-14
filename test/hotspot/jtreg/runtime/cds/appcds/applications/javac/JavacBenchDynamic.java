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
 * @summary Run JavacBenchApp with static CDS archive
 * @requires vm.cds
 * @library /test/lib
 * @build JavacBenchApp 
 *
 * @run driver JavacBenchDynamic
 */

import java.util.ArrayList;
import java.util.List;
import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

public class JavacBenchDynamic extends JavacBenchTestBase {
    static String appJar;
    public static void main(String[] args) throws Exception {
        appJar = getAppJar();
        List<String> empty = List.of();
        run(empty, empty);
        run(List.of("-XX:+RecordTraining"), List.of("-XX:+ReplayTraining"));
    }

    static void run(List<String> dumpArgs, List<String> execArgs) throws Exception {
        String JSA = "JavacBenchDynamic.jsa";
        String mainClass = "JavacBenchApp";
        String count = "30";

        { /* dump */
            ArrayList<String> args = new ArrayList<String>();
            args.add("-XX:ArchiveClassesAtExit=" + JSA);
            args.add("-Xlog:cds=debug");
            args.add("-cp");
            args.add(appJar);
            for (String arg : dumpArgs) {
                args.add(arg);
            }
            args.add(mainClass);
            args.add(count);

            ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(args);
            OutputAnalyzer output = CDSTestUtils.executeAndLog(pb, "dump");
            output.shouldHaveExitValue(0);
        }

        { /* exec */
            ArrayList<String> args = new ArrayList<String>();
            args.add("-XX:SharedArchiveFile=" + JSA);
            args.add("-Xlog:cds");
            args.add("-cp");
            args.add(appJar);
            for (String arg : execArgs) {
                args.add(arg);
            }
            args.add(mainClass);
            args.add(count);

            ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(args);
            OutputAnalyzer output = CDSTestUtils.executeAndLog(pb, "exec");
            output.shouldHaveExitValue(0);
        }
    }
}
