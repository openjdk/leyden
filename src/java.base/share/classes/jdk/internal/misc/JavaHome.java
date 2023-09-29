/*
 * Copyright 2022 Google, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  This particular file is
 * subject to the "Classpath" exception as provided in the LICENSE file
 * that accompanied this code.
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
package jdk.internal.misc;

import java.io.File;
import java.io.InputStream;
import java.io.IOException;
import java.net.URI;
import java.nio.file.FileSystem;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Collections;
import java.util.Properties;
import sun.security.action.GetPropertyAction;

/**
 * This class provides support for accessing the JDK resource files reside in
 * locations that are relative to {@code java.home}, for both traditional
 * execution mode and hermetic Java execution mode.
 *
 * <p>In traditional execution mode, the JDK runtime files are located in
 * sub-directories under the directory specified by {@code java.home} system
 * property.
 *
 * <p>With hermetic Java, the JDK runtime files are packaged in a hermetic JAR
 * file. The {@code java.home} is no longer the conventional directory, instead
 * it is the hermetic JAR file itself. The JavaHome class encapsulates hermetic
 * JAR file packaging details. Callers can use the public methods provided by
 * this class to access JDK resource files without knowing how they are
 * packaged.
 */
public class JavaHome {
    private static boolean DEBUG;

    private static final String JAVA_HOME;

    private static final String EXECUTABLE;

    private static final FileSystem jarFileSystem;

    private static final String HERMETIC_JAR_JDK_RESOURCES_HOME;

    static {
        Properties props = GetPropertyAction.privilegedGetProperties();
        DEBUG = Boolean.parseBoolean(
            props.getProperty("jdk.internal.misc.JavaHome.debug"));

        JAVA_HOME = props.getProperty("java.home");
        if (JAVA_HOME == null) {
            throw new Error("Can't find java.home");
        }

        if (JAVA_HOME.endsWith(".jar")) {
            // The JAVA_HOME is a jar file. We are dealing with hermetic Java.

            // JAVA_HOME is the hermetic executable JAR.
            EXECUTABLE = props.getProperty(
                "jdk.internal.misc.hermetic.executable", JAVA_HOME);

            try {
                jarFileSystem = FileSystems.newFileSystem(
                    URI.create("jar:file:" + JAVA_HOME), Collections.emptyMap());
                if (DEBUG) {
                    System.out.println(
                        "Create hermetic java.home file system: " + jarFileSystem);
                }
            } catch (IOException ex) {
                // The JAVA_HOME jar file should always exist.
                throw new IllegalStateException(ex);
            }

            String javaHome =
                props.getProperty("jdk.internal.misc.JavaHome.Directory", "");
            if (!javaHome.isEmpty() && !javaHome.endsWith("/")) {
                javaHome += "/";
            }
            HERMETIC_JAR_JDK_RESOURCES_HOME = javaHome;
            if (DEBUG) {
                System.out.println("Hermetic JAR JDK resource directory: " +
                                   HERMETIC_JAR_JDK_RESOURCES_HOME);
            }
        } else {
            jarFileSystem = null;
            HERMETIC_JAR_JDK_RESOURCES_HOME = "";
            EXECUTABLE = "";
        }
    }

    public static boolean isHermetic() {
        return jarFileSystem != null;
    }

    public static String hermeticExecutable() {
        if (!isHermetic()) {
            throw new IllegalStateException("Not hermetic Java");
        }
        if (EXECUTABLE.equals("")) {
            throw new IllegalStateException("Executable is not set");
        }
        return EXECUTABLE;
    }

    /**
     * Return a Path representation of the hermetic Java home (containing the
     * JDK resource files).
     */
    public static Path hermeticJavaHome() {
        if (!isHermetic()) {
            throw new IllegalStateException("Not hermetic Java");
        }

        Path p = jarFileSystem.getPath(HERMETIC_JAR_JDK_RESOURCES_HOME);
        if (DEBUG) {
            System.out.println("Hermetic java home path: " + p);
        }
        return p;
    }

    /**
     * Return a Path instance to the given java home directory.
     */
    private static Path nonHermeticJavaHome(String javaHome) {
        if (isHermetic()) {
            throw new IllegalStateException("Hermetic Java");
        }

        Path p = Paths.get(javaHome);
        if (DEBUG) {
           System.out.println(
                "non-Hermetic java home path: " + p);
        }
        return p;
    }

    /**
     * If the current executable image is a hermetic deploy JAR, return a Path
     * instance for the given resource within the hermetic deploy JAR.
     *
     * <p>Otherwise, return a Path instance representing the given resource within
     * the given java home directory.
     *
     * <p>This method has a {@code javaHome} parameter currently. It is ignored for
     * hermetic Java case. The caller passed {@code javaHome} value is used in
     * the non-hermetic Java case. That is to ensure we don't change any of the
     * existing semantics, since some caller use
     * {@code StaticProperty.javaHome()} and others use
     * {@code System.getProperty("java.home")}.
     */
    public static Path getJDKResource(String javaHome, String... path) {
        Path res;
        if (isHermetic()) {
            res = hermeticJavaHome();
        } else {
            res = nonHermeticJavaHome(javaHome);
        }

        for (String p : path) {
            res = res.resolve(p);
        }
        if (DEBUG) {
            System.out.println(
                "Get JDK resource Path: " + res);
        }
        return res;
    }
}
