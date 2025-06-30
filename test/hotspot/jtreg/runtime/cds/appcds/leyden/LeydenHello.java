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
 * @test id=aot
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenHello
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenHelloApp
 * @run driver LeydenHello AOT
 */

/*
 * @test id=leyden
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build LeydenHello
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar LeydenHelloApp
 * @run driver LeydenHello LEYDEN
 */

import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class LeydenHello {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass = "LeydenHelloApp";

    public static void main(String[] args) throws Exception {
        Tester t = new Tester();
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
        public void checkExecution(OutputAnalyzer out, RunMode runMode) {
            if (runMode.isApplicationExecuted()) {
                out.shouldContain("Hello Leyden " + runMode.name());
            }
        }
    }
}

class LeydenHelloApp {
    public static void main(String args[]) {
        System.out.println("Hello Leyden " + args[0]);
    }
}
