/*
 * Copyright (c) 2003, 2024, Oracle and/or its affiliates. All rights reserved.
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

 package java.lang.management;
 import java.io.IOException;
 import java.util.List;

 /**
  * The management interface for the AOT system.
  *
  * <p> A Java virtual machine has a single instance of the implementation
  * class of this interface.  This instance implementing this interface is
  * an <a href="ManagementFactory.html#MXBean">MXBean</a>
  * that can be obtained by calling
  * the {@link ManagementFactory#getAOTMXBean} method or
  * from the {@link ManagementFactory#getPlatformMBeanServer
  * platform MBeanServer} method.
  *
  * <p>The {@code ObjectName} for uniquely identifying the MXBean for
  * the runtime system within an MBeanServer is:
  * <blockquote>
  *    {@link ManagementFactory#RUNTIME_MXBEAN_NAME
  *           java.lang:type=AOT}
  * </blockquote>
  *
  * It can be obtained by calling the
  * {@link PlatformManagedObject#getObjectName} method.
  *
  * <p> This interface defines several convenient methods for accessing
  * system properties about the Java virtual machine.
  *
  * @see ManagementFactory#getPlatformMXBeans(Class)
  * @see <a href="../../../javax/management/package-summary.html">
  *      JMX Specification.</a>
  * @see <a href="package-summary.html#examples">
  *      Ways to Access MXBeans</a>
  *
  * @author  Mandy Chung
  * @since   1.5
  */
 public interface AOTMXBean extends PlatformManagedObject {

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
      * If a recording is in progress, then stops the recording.
      *
      * @return {@code true} if a recording was stopped; {@code false} otherwise.
      */
     public boolean endRecording();
 }
