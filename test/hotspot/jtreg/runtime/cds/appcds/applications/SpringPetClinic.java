/*
 * Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
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
 * @test id=static
 * @key external-dep
 * @requires vm.cds
 * @summary run Spring Pet Clinic demo with the classic static archive workflow
 * @library /test/lib
 * @run driver/timeout=120 SpringPetClinic STATIC
 */

/*
 * @test id=dynamic
 * @key external-dep
 * @requires vm.cds
 * @summary run Spring Pet Clinic demo with the classic dynamic archive workflow
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm/timeout=120 -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xbootclasspath/a:. SpringPetClinic DYNAMIC
 */

/*
 * @test id=aot
 * @key external-dep
 * @requires vm.cds
 * @requires vm.cds.write.archived.java.heap
 * @summary run Spring Pet Clinic demo with JEP 483 workflow
 * @library /test/lib
 * @run driver/timeout=120 SpringPetClinic AOT
 */

/*
 * @test id=leyden
 * @key external-dep
 * @requires vm.cds
 * @requires vm.cds.write.archived.java.heap
 * @summary run Spring Pet Clinic demo with leyden-premain "new workflow"
 * @library /test/lib
 * @run driver/timeout=120 SpringPetClinic LEYDEN
 */

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Enumeration;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import jdk.test.lib.StringArrayUtils;
import jdk.test.lib.artifacts.Artifact;
import jdk.test.lib.artifacts.ArtifactResolver;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.process.OutputAnalyzer;

// NOTE: if you have not set up an artifactory, you can create spring-petclinic-3.2.0.zip by:
//
// - Make a clone of https://github.com/openjdk/leyden/tree/premain
// - Change to the directory test/hotspot/jtreg/premain/spring-petclinic
// - Edit the Makefile
// - Run the command "make unpack"
//
// Then, you can add the following to your jtreg command-line to run the test cases in this directory:
// -vmoption:-Djdk.test.lib.artifacts.spring-petclinic=/repo/test/hotspot/jtreg/premain/spring-petclinic/petclinic-snapshot/target/spring-petclinic-3.2.0.zip

@Artifact(organization = "org.springframework.samples", name = "spring-petclinic", revision = "3.2.0", extension = "zip", unpack = false)
public class SpringPetClinic {
    public static void main(String args[]) throws Exception {
        String cp = getArtifact();
        SpringPetClinicTester tester = new SpringPetClinicTester(cp);
        tester.run(args);
    }

    private static String getArtifact() throws Exception {
        File cpFile = new File("petclinic-snapshot/target/unpacked/classpath");
        if (!cpFile.exists()) {
            long started = System.currentTimeMillis();
            System.out.println("Download and extract spring-petclinic");

            Map<String, Path> artifacts = ArtifactResolver.resolve(SpringPetClinic.class);
            System.out.println(artifacts);
            Path zip = artifacts.get("org.springframework.samples.spring-petclinic-3.2.0");

            long elapsed = System.currentTimeMillis() - started;
            System.out.println("Resolved artifacts in " + elapsed + " ms");

            extractZip(zip.toString(), new File("."));
        }

        // Copy the classpath file and edit its path separator if necessary.
        String cpFileCopy = "petclinic-classpath.txt";
        String cp = Files.readString(cpFile.toPath());
        if (File.pathSeparatorChar == ';') {
            // This file was generated with ":" as path separator. Change it
            // to ';' for Windows.
            cp = cp.replace(':', ';');
        }
        System.out.println("\nClasspath = \"" + cp + "\"\n");
        Files.writeString(Paths.get(cpFileCopy), cp);
        return "@" + cpFileCopy;
    }

    static void extractZip(String srcFile, File outDir) throws Exception {
        long zipSize = (new File(srcFile)).length();
        System.out.println("Extracting from " + srcFile + " (" + zipSize + " bytes) to " + outDir);

        long started = System.currentTimeMillis();
        try (ZipFile zipFile = new ZipFile(srcFile)) {
            int numFiles = 0;
            long numBytes = 0;
            Enumeration<? extends ZipEntry> e = zipFile.entries();

            byte buffer[] = new byte[1024];
            while (e.hasMoreElements()) {
                ZipEntry entry = e.nextElement();
                if (!entry.isDirectory()) {
                    File destinationPath = new File(outDir, entry.getName());
                    destinationPath.getParentFile().mkdirs();

                    numFiles ++;
                    try (
                         BufferedInputStream bis = new BufferedInputStream(zipFile.getInputStream(entry));
                         FileOutputStream fos = new FileOutputStream(destinationPath);
                         BufferedOutputStream bos = new BufferedOutputStream(fos, 1024)) {
                        int n;
                        while ((n = bis.read(buffer, 0, 1024)) != -1) {
                            bos.write(buffer, 0, n);
                            numBytes += n;
                        }
                    }
                }
            }
            long elapsed = System.currentTimeMillis() - started;
            System.out.println("Extracted " + numFiles + " file(s) = " + numBytes + " bytes in " + elapsed + " ms");
        }
    }


    static class SpringPetClinicTester extends CDSAppTester {
        String cp;

        SpringPetClinicTester(String cp) {
            super("SpringPetClinic");
            this.cp = cp;
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-Xlog:init,cds",
                "-DautoQuit=true",
                "-Dspring.output.ansi.enabled=NEVER",
                "-Dspring.aot.enabled=true",
                "-Dserver.port=0", // use system-assigned port
                "--enable-native-access=ALL-UNNAMED",

                // PetClinic runs very slowly in debug builds if VerifyDependencies is enabled.
                "-XX:+IgnoreUnrecognizedVMOptions", "-XX:-VerifyDependencies",
              //These don't seem necessary when pet-clinic is run in "Spring AOT" mode
              //"--add-opens", "java.base/java.io=ALL-UNNAMED",
              //"--add-opens", "java.base/java.lang=ALL-UNNAMED",
              //"--add-opens", "java.rmi/sun.rmi.transport=ALL-UNNAMED"
            };
        }

        @Override
        public String classpath(RunMode runMode) {
            return cp;
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            String cmdLine[] = new String[] {
                "org.springframework.samples.petclinic.PetClinicApplication"
            };
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
