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
 */

import java.lang.management.ManagementFactory;
import java.lang.management.RuntimeMXBean;

import io.quarkus.runtime.Quarkus;
import io.quarkus.runtime.QuarkusApplication;
import io.quarkus.runtime.annotations.QuarkusMain;

@QuarkusMain
public class Main {
    public static void main(String... args) throws Exception {
 	long mainStart = System.currentTimeMillis();
 	RuntimeMXBean runtimeMXBean = ManagementFactory.getRuntimeMXBean();
 	// This includes all the time spent inside the JVM before main() is reached
 	// (since os::Posix::init is called and initial_time_count is initialized).
 	long vmStart = runtimeMXBean.getStartTime();
 	long maxBeanOverHead = System.currentTimeMillis() - mainStart;

        if (Boolean.getBoolean("autoQuit")) {
            Quarkus.manualInitialize();
            Quarkus.manualStart();

            long end = System.currentTimeMillis();
            System.out.println("#### Booted and returned in " + (end - vmStart - maxBeanOverHead) + "ms");
            System.out.println("#### (debug) mainStart = " + mainStart);
            System.out.println("#### (debug) vmStart = " + vmStart);
            System.out.println("#### (debug) before main (mainStart - vmStart) = " + (mainStart - vmStart));
            System.out.println("#### (debug) maxBeanOverHead = " + maxBeanOverHead);
            System.out.println("#### (debug) end = " + end);

            System.out.println("Done .. Exiting");
            System.exit(0);
        } else {
            Quarkus.run(MyApp.class, args);
        }
    }

    public static class MyApp implements QuarkusApplication {
        @Override
        public int run(String... args) throws Exception {
            System.out.println("Do startup logic here");
            Quarkus.waitForExit();
            System.out.println("Done");
            Thread.sleep(1000);
            System.out.println("Exiting");
            Quarkus.asyncExit(0);
            return 0;
        }
    }
}
