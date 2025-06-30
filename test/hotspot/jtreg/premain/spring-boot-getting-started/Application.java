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

package com.example.springboot;

import java.util.Arrays;

import java.lang.management.ManagementFactory;
import java.lang.management.RuntimeMXBean;

import org.springframework.boot.CommandLineRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.ApplicationContext;
import org.springframework.context.annotation.Bean;

@SpringBootApplication
public class Application {
    static long mainStart, vmStart, maxBeanOverHead;

    public static void main(String[] args) {
 	mainStart = System.currentTimeMillis();
 	RuntimeMXBean runtimeMXBean = ManagementFactory.getRuntimeMXBean();
 	// This includes all the time spent inside the JVM before main() is reached
 	// (since os::Posix::init is called and initial_time_count is initialized).
 	vmStart = runtimeMXBean.getStartTime();
 	maxBeanOverHead = System.currentTimeMillis() - mainStart;

        SpringApplication.run(Application.class, args);
    }

    @Bean
    public CommandLineRunner commandLineRunner(ApplicationContext ctx) {
       if ("onRefresh".equals(System.getProperty("spring.context.exit"))) {
            long end = System.currentTimeMillis();
            System.out.println("#### Booted and returned in " + (end - vmStart - maxBeanOverHead) + "ms");
            System.out.println("#### (debug) mainStart = " + mainStart);
            System.out.println("#### (debug) vmStart = " + vmStart);
            System.out.println("#### (debug) before main (mainStart - vmStart) = " + (mainStart - vmStart));
            System.out.println("#### (debug) maxBeanOverHead = " + maxBeanOverHead);
            System.out.println("#### (debug) end = " + end);
       }

       return args -> {
           System.out.println("Let's inspect the beans provided by Spring Boot:");
           String[] beanNames = ctx.getBeanDefinitionNames();
           Arrays.sort(beanNames);
           for (String beanName : beanNames) {
               System.out.println(beanName);
           }
       };
    }
}
