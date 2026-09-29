/*
 * Copyright 2017-2024 original authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package example.micronaut;

import java.lang.management.ManagementFactory;
import java.lang.management.RuntimeMXBean;

import io.micronaut.runtime.Micronaut;

public class Application {

    public static void main(String[] args) {
 	long mainStart = System.currentTimeMillis();
 	RuntimeMXBean runtimeMXBean = ManagementFactory.getRuntimeMXBean();
 	// This includes all the time spent inside the JVM before main() is reached
 	// (since os::Posix::init is called and initial_time_count is initialized).
 	long vmStart = runtimeMXBean.getStartTime();
 	long maxBeanOverHead = System.currentTimeMillis() - mainStart;

        Micronaut.run(Application.class, args);

        if (Boolean.getBoolean("autoQuit")) {
            long end = System.currentTimeMillis();
            System.out.println("#### Booted and returned in " + (end - vmStart - maxBeanOverHead) + "ms");
            System.out.println("#### (debug) mainStart = " + mainStart);
            System.out.println("#### (debug) vmStart = " + vmStart);
            System.out.println("#### (debug) before main (mainStart - vmStart) = " + (mainStart - vmStart));
            System.out.println("#### (debug) maxBeanOverHead = " + maxBeanOverHead);
            System.out.println("#### (debug) end = " + end);

            System.out.println("Done .. Exiting");
            System.exit(0);
        }
    }
}
