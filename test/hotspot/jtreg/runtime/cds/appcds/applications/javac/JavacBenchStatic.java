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
 * @run driver JavacBenchStatic
 */

import java.util.List;
import jdk.test.lib.cds.CDSOptions;
import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.helpers.ClassFileInstaller;

public class JavacBenchStatic extends JavacBenchTestBase {
    static String appJar;
    public static void main(String[] args) throws Exception {
        appJar = getAppJar();
        List<String> empty = List.of();
        run(List.of("-XX:+ArchiveInvokeDynamic"), empty);
        run(List.of("-XX:+PreloadSharedClasses"), empty);
        run(List.of("-XX:-PreloadSharedClasses"), empty);
    }

    static void run(List<String> dumpArgs, List<String> execArgs) throws Exception {
        String classList = "JavacBenchApp.classlist";
        String commandLine[] = {
            "-cp", appJar,
            "JavacBenchApp",
            "30",
        };

        // Get classlist
        CDSTestUtils.dumpClassList(classList, commandLine)
            .assertNormalExit();

        // Dump the static archive
        CDSOptions opts = (new CDSOptions())
            .addPrefix("-cp", appJar,
                       "-XX:SharedClassListFile=" + classList);
        for (String arg : dumpArgs) {
            opts.addPrefix(arg);
        }
        opts.addSuffix("-Xlog:cds=debug,cds+class=debug,cds+resolve=debug:file=dump.log");
        CDSTestUtils.createArchiveAndCheck(opts);

        // Use the dumped static archive
        opts = (new CDSOptions())
            .setUseVersion(false)
            .addPrefix("-cp", appJar)
            .addSuffix(commandLine);
        for (String arg : execArgs) {
            opts.addPrefix(arg);
        }
        opts.addSuffix("-Xlog:cds");
        CDSTestUtils.run(opts)
            .assertNormalExit();
    }
}
