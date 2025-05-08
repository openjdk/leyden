/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
package jdk.management;

import java.lang.management.ManagementFactory;
import java.lang.management.PlatformManagedObject;
import java.util.concurrent.ForkJoinPool;
import javax.management.MBeanServer;
import javax.management.ObjectName;

/**
 * Management interface for the JDK's AOT system.
 *
 * <p> {@code AOTCacheMXBean} supports inspection of the current AOT mode, as well as monitoring
 * the current recording length. It also supports dynamically ending the current recording.
 *
 * <p> The management interface is registered with the platform {@link MBeanServer
 * MBeanServer}. The {@link ObjectName ObjectName} that uniquely identifies the management
 * interface within the {@code MBeanServer} is: "jdk.management:type=AOTCache".
 *
 * <p> Direct access to the MXBean interface can be obtained with
 * {@link ManagementFactory#getPlatformMXBean(Class)}.
 *
 * @since 25
 */
public interface AOTCacheMXBean extends PlatformManagedObject {
     /**
      * Returns the string representing the current AOT mode of
      * operation.
      *
      * @return the string representing the current AOT mode.
      */
      public String getMode();

      /**
       * Tests if a recording is in progress.
       *
       * @return {@code true} if a recording is in progress; {@code false} otherwise.
       */
      public boolean isRecording();

      /**
       * If a recording is in progress or has been completed, then returns the duration in milliseconds
       *
       * @return duration of the recording in milliseconds.
       */
      public long getRecordingDuration();

      /**
       * If a recording is in progress, then ends the recording.
       *
       * @return {@code true} if a recording was stopped; {@code false} otherwise.
       */
      public boolean endRecording();
}