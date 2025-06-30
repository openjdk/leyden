/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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
 * @test id=aot
 * @requires vm.cds.supports.aot.class.linking
 * @comment work around JDK-8345635
 * @requires !vm.jvmci.enabled
 * @library /test/jdk/lib/testlibrary /test/lib
 * @modules java.management
 * @build EndTrainingWithAOTCacheMXBean
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar MyTestApp ShouldBeCached ShouldNotBeCached
 * @run driver EndTrainingWithAOTCacheMXBean AOT
 */

/*
 * @test id=leyden
 * @requires vm.cds.supports.aot.class.linking
 * @comment work around JDK-8345635
 * @requires !vm.jvmci.enabled
 * @library /test/jdk/lib/testlibrary /test/lib
 * @modules jdk.management
 * @build EndTrainingWithAOTCacheMXBean
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar MyTestApp ShouldBeCached ShouldNotBeCached
 * @run driver EndTrainingWithAOTCacheMXBean LEYDEN
 */

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;
import java.lang.management.ManagementFactory;
import jdk.management.AOTCacheMXBean;

public class EndTrainingWithAOTCacheMXBean {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass = "MyTestApp";

    public static void main(String[] args) throws Exception {
        // We want to test the entry count implementation in both interpreter and compiler.
        new Tester().run(args);
    }

    static class Tester extends CDSAppTester {

        public Tester() {
            super(mainClass);
        }

        @Override
        public String classpath(RunMode runMode) {
            return appJar;
        }

        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-Xlog:cds+class=debug",
                "-Xlog:aot+class=debug",
                "--add-modules=jdk.management"
            };
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                mainClass, runMode.name(),
            };
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            var name = runMode.name();
            if (runMode.isApplicationExecuted()) {
                out.shouldContain("Hello Leyden " + name);
                out.shouldContain("ShouldBeCached.dummy()");
                out.shouldContain("ShouldNotBeCached.dummy()");
                if(runMode == RunMode.TRAINING || runMode == RunMode.TRAINING0) {
                    if (isLeydenWorkflow()) {
                        out.shouldContain("AOTMode = auto");
                    } else {
                        out.shouldContain("AOTMode = record");
                    }
                    out.shouldContain("Confirmed is recording");
                    out.shouldContain("Confirmed recording duration > 0");
                    out.shouldContain("Stopped recording successfully after an additional 10ms");
                    out.shouldContain("Last recording duration > than previous duration");
                    out.shouldContain("Confirmed recording stopped");
                    out.shouldContain("Confirmed recording duration has not changed after 10ms");
                } else if (runMode == RunMode.TRAINING1 || runMode == RunMode.ASSEMBLY) {
                    out.shouldNotContain("Hello Leyden ");
                } else if (runMode == RunMode.PRODUCTION) {
                    if (isLeydenWorkflow()) {
                        out.shouldContain("AOTMode = auto");
                    } else {
                        out.shouldContain("AOTMode = on");
                    }
                    out.shouldContain("Confirmed is not recording");
                    out.shouldContain("Confirmed recording duration == 0");
                }
                out.shouldNotContain("Thread interrupted");
                out.shouldNotContain("Failed to stop recording");
            }
            if (isDumping(runMode)) {
                if (isAOTWorkflow()) {
                    out.shouldMatch("aot,class.* ShouldBeCached");
                    out.shouldNotMatch("aot,class.* ShouldNotBeCached");
                } else {
                    out.shouldMatch("cds,class.* ShouldBeCached");
                    out.shouldNotMatch("cds,class.* ShouldNotBeCached");
                }
            }
        }
    }
}

class MyTestApp {
    public static void main(String args[]) throws Exception {
        System.out.println("Hello Leyden " + args[0]);
        var aotBean = ManagementFactory.getPlatformMXBean(AOTCacheMXBean.class);
        if (aotBean == null) {
            System.out.println("AOTCacheMXBean is not available");
            return;
        }
        ShouldBeCached.dummy();
        System.out.println("AOTMode = " + aotBean.getMode());
        if (aotBean.isRecording()) {
            try {
                System.out.println("Confirmed is recording");
                var initialDuration = aotBean.getRecordingDuration();
                System.out.println("Confirmed recording duration > 0");
                Thread.sleep(10);
                if (aotBean.endRecording()) {
                    System.out.println("Stopped recording successfully after an additional 10ms");
                    if (!aotBean.isRecording()) {
                        System.out.println("Confirmed recording stopped");
                    }
                    var recordingDuration = aotBean.getRecordingDuration();
                    if (recordingDuration > initialDuration) {
                        System.out.println("Last recording duration > than previous duration");
                    }
                    Thread.sleep(10);
                    var lastDuration = aotBean.getRecordingDuration();
                    if (lastDuration == recordingDuration) {
                        System.out.println("Confirmed recording duration has not changed after 10ms");
                    }
                } else {
                    System.out.println("Failed to stop recording");
                }
            } catch (InterruptedException e) {
                System.out.println("Thread interrupted");
            }
        } else {
            System.out.println("Confirmed is not recording");
            var recordingDuration = aotBean.getRecordingDuration();
            if (recordingDuration == 0) {
                System.out.println("Confirmed recording duration == 0");
            }
        }
        ShouldNotBeCached.dummy();
    }
}

class ShouldBeCached {
    static void dummy() {
        System.out.println("ShouldBeCached.dummy()");
    }
}

class ShouldNotBeCached {
    static void dummy() {
        System.out.println("ShouldNotBeCached.dummy()");
    }
}
