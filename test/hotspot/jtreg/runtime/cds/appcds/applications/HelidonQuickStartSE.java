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

import java.io.File;
import java.nio.file.Path;
import java.util.Map;
import jdk.test.lib.StringArrayUtils;
import jdk.test.lib.artifacts.Artifact;
import jdk.test.lib.artifacts.ArtifactResolver;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.process.OutputAnalyzer;

/*
 * @test id=static
 * @key external-dep
 * @requires vm.cds
 * @summary run HelidonQuickStartSE with the classic static archive workflow
 * @library /test/lib
 * @run driver/timeout=120 HelidonQuickStartSE STATIC
 */

/*
 * @test id=dynamic
 * @key external-dep
 * @requires vm.cds
 * @summary run HelidonQuickStartSE with the classic dynamic archive workflow
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm/timeout=120 -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xbootclasspath/a:. HelidonQuickStartSE DYNAMIC
 */

/*
 * @test id=aot
 * @key external-dep
 * @requires vm.cds
 * @requires vm.cds.write.archived.java.heap
 * @summary run HelidonQuickStartSE with the JEP 483 workflow
 * @library /test/lib
 * @run driver/timeout=120 HelidonQuickStartSE AOT
 */

// Test CDS with the example program in https://helidon.io/docs/v4/se/guides/quickstart
//
// NOTE: if you have not set up an artifactory, you can create helidon-quickstart-se-4.0.7.zip by:
//
// - Make a clone of https://github.com/openjdk/leyden/tree/premain
// - Change to the directory test/hotspot/jtreg/premain/helidon-quickstart-se
// - Edit the Makefile and ../lib/DemoSupport.gmk as necessary
// - Run the command "make artifact"
//
// Then, you can add the following to your jtreg command-line to run the test cases in this directory:
// -vmoption:-Djdk.test.lib.artifacts.helidon-quickstart-se=/my/repo/test/hotspot/jtreg/premain/helidon-quickstart-se/helidon-quickstart-se

@Artifact(organization = "io.helidon", name = "helidon-quickstart-se", revision = "4.0.7", extension = "zip")
public class HelidonQuickStartSE {
    public static void main(String args[]) throws Exception {
        String cp = getArtifact();
        HelidonQuickStartSETester tester = new HelidonQuickStartSETester(cp);
        tester.run(args);
        System.out.println(cp);
    }

    private static String getArtifact() throws Exception {
        String sep = File.separator;
        Map<String, Path> artifacts = ArtifactResolver.resolve(HelidonQuickStartSE.class);
        Path path = artifacts.get("io.helidon.helidon-quickstart-se-4.0.7");
        return path.toString() + sep + "target" + sep + "helidon-quickstart-se.jar";
    }

    static class HelidonQuickStartSETester extends CDSAppTester {
        String cp;

        HelidonQuickStartSETester(String cp) {
            super("HelidonQuickStartSE");
            this.cp = cp;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-DautoQuit=true",
                "-Dserver.port=0", // use system-assigned port

                // This program may run very slowly in debug builds if VerifyDependencies is enabled.
                "-XX:+IgnoreUnrecognizedVMOptions", "-XX:-VerifyDependencies",
            };
        }

        @Override
        public String classpath(RunMode runMode) {
            return cp;
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            String cmdLine[] = new String[] {
                "io.helidon.examples.quickstart.se.Main", 
            };

            if (runMode.isProductionRun()) {
                cmdLine = StringArrayUtils.concat("-Xlog:aot+codecache=error", cmdLine);
            }
            return cmdLine;
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            if (runMode.isApplicationExecuted()) {
                out.shouldContain("Booted and returned in ");
            }
        }
    }
}
