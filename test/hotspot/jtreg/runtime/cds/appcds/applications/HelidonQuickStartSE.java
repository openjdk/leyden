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
 * @requires vm.cds
 * @summary run HelidonQuickStartSE with the classic static archive workflow
 * @library /test/lib
 * @run driver/timeout=120 HelidonQuickStartSE STATIC
 */

/*
 * @test id=dynamic
 * @requires vm.cds
 * @summary run HelidonQuickStartSE with the classic dynamic archive workflow
 * @library /test/lib
 * @run driver/timeout=120 HelidonQuickStartSE DYNAMIC
 */

/*
 * @test id=leyden
 * @requires vm.cds
 * @summary un HelidonQuickStartSE with the Leyden workflow
 * @library /test/lib
 * @run driver/timeout=120 HelidonQuickStartSE LEYDEN
 */

/*
 * @test id=leyden_old
 * @requires vm.cds
 * @summary un HelidonQuickStartSE with the "OLD" Leyden workflow
 * @library /test/lib
 * @run driver/timeout=120 HelidonQuickStartSE LEYDEN_OLD
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
        Map<String, Path> artifacts = ArtifactResolver.resolve(HelidonQuickStartSE.class);
        Path path = artifacts.get("io.helidon.helidon-quickstart-se-4.0.7");
        return path.toString() + "/target/helidon-quickstart-se.jar";
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
                cmdLine = StringArrayUtils.concat("-Xlog:scc=error", cmdLine);
            }
            return cmdLine;
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            if (!runMode.isStaticDump()) {
                out.shouldContain("Booted and returned in ");
            }
        }
    }
}
