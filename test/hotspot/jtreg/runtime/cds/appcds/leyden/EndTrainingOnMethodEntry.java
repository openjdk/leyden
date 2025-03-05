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
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build EndTrainingOnMethodEntry
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar MyTestApp ShouldBeCached ShouldNotBeCached
 * @run driver EndTrainingOnMethodEntry AOT
 */

/*
 * @test id=leyden
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build EndTrainingOnMethodEntry
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar MyTestApp ShouldBeCached ShouldNotBeCached
 * @run driver EndTrainingOnMethodEntry LEYDEN
 */

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class EndTrainingOnMethodEntry {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass = "MyTestApp";

    public static void main(String[] args) throws Exception {
        (new Tester(false)).run(args);
        (new Tester(true)).run(args);
    }

    static class Tester extends CDSAppTester {
        boolean useCount;

        public Tester(boolean useCount) {
            super(mainClass);
            this.useCount = useCount;
        }

        @Override
        public String classpath(RunMode runMode) {
            return appJar;
        }

        public String[] vmArgs(RunMode runMode) {
            String stop = useCount ? ("stopTrainingOnMeWithCount,count=" + MyTestApp.COUNT) : "stopTrainingOnMe";
            return new String[] {
                "-Xlog:cds+class=debug",
                "-XX:AOTEndTrainingOnMethodEntry=MyTestApp." + stop,
            };
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                mainClass, runMode.name(), Boolean.toString(useCount),
            };
        }

        @Override
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            if (runMode.isApplicationExecuted()) {
                out.shouldContain("Hello Leyden " + runMode.name());
            }
            if (isDumping(runMode)) {
                out.shouldMatch("cds,class.* ShouldBeCached");
                out.shouldNotMatch("cds,class.* ShouldNotBeCached");
            }
        }
    }
}

class MyTestApp {
    public static final int COUNT = 10; // FIXME (JDK-8351100): debug build crashes with COUNT = 10000;
    public static void main(String args[]) {
        System.out.println("Hello Leyden " + args[0] + ", useCount = " + args[1]);
        if (args[1].equals("true")) {
            for (int i = 0; i < COUNT + 10; i++) {
                stopTrainingOnMeWithCount(i);
            }
        } else {
            ShouldBeCached.dummy();
            stopTrainingOnMe();
        }
    }

    static void stopTrainingOnMe() {
        // The AOT configuration file should have been recorded before the body
        // of this method is executed, so the ShouldNotBeCached class should not be
        // recorded in the config.
        ShouldNotBeCached.dummy();
    }

    static void stopTrainingOnMeWithCount(int i) {
        if (i >= COUNT - 2) {
            System.out.println("COUNT = " + COUNT + ", i = " + i);
            ShouldBeCached.dummy();
        }
        if (i >= COUNT) {
            // The AOT configuration file should have been recorded before this block is entered,
            // so the ShouldNotBeCached class should not be recorded in the config.
            ShouldNotBeCached.dummy();
        }
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
