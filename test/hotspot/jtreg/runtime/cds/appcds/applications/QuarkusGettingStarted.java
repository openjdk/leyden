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
 * @summary run QuarkusGettingStarted with the classic static archive workflow
 * @library /test/lib
 * @run driver/timeout=120 QuarkusGettingStarted STATIC
 */

/*
 * @test id=dynamic
 * @key external-dep
 * @requires vm.cds
 * @summary run QuarkusGettingStarted with the classic dynamic archive workflow
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm/timeout=120 -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xbootclasspath/a:. QuarkusGettingStarted DYNAMIC
 */

/*
 * @test id=aot
 * @key external-dep
 * @requires vm.cds
 * @requires vm.cds.write.archived.java.heap
 * @summary run QuarkusGettingStarted with the JEP 483 workflow
 * @library /test/lib
 * @run driver/timeout=120 QuarkusGettingStarted AOT
 */

/*
 * @test id=leyden
 * @key external-dep
 * @requires vm.cds
 * @requires vm.cds.write.archived.java.heap
 * @summary run QuarkusGettingStarted with the Leyden workflow
 * @library /test/lib
 * @run driver/timeout=120 QuarkusGettingStarted LEYDEN
 */

// Test CDS with the example program in https://quarkus.io/guides/getting-started
//
// NOTE: if you have not set up an artifactory, you can create quarkus-getting-started-1.0.0b.zip by:
//
// - Make a clone of https://github.com/openjdk/leyden/tree/premain
// - Change to the directory test/hotspot/jtreg/premain/quarkus-getting-started
// - Edit the Makefile and ../lib/DemoSupport.gmk as necessary
// - Run the command "make artifact"
//
// Then, you can add the following to your jtreg command-line to run the test cases in this directory:
// -vmoption:-Djdk.test.lib.artifacts.quarkus-getting-started=/my/repo/test/hotspot/jtreg/premain/quarkus-getting-started/getting-started/target

@Artifact(organization = "io.quarkus", name = "quarkus-getting-started", revision = "1.0.0b", extension = "zip")
public class QuarkusGettingStarted {
    public static void main(String args[]) throws Exception {
        String cp = getArtifact();
        QuarkusGettingStartedTester tester = new QuarkusGettingStartedTester(cp);
        tester.run(args);
        System.out.println(cp);
    }

    private static String getArtifact() throws Exception {
        Map<String, Path> artifacts = ArtifactResolver.resolve(QuarkusGettingStarted.class);
        Path path = artifacts.get("io.quarkus.quarkus-getting-started-1.0.0b");
        return path.toString() + File.separator + "getting-started-1.0.0-SNAPSHOT-runner.jar";
    }

    static class QuarkusGettingStartedTester extends CDSAppTester {
        String cp;

        QuarkusGettingStartedTester(String cp) {
            super("QuarkusGettingStarted");
            this.cp = cp;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-DautoQuit=true",
                "-Dquarkus.http.port=0", // use system-assigned port

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
                "Main", 
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
