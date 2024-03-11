/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
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

package sun.security.util;

import java.io.IOException;
import java.io.FileInputStream;
import java.io.InputStream;

import jdk.internal.misc.JavaHome;
import jdk.internal.util.StaticProperty;

import java.io.File;

import sun.security.action.GetPropertyAction;

public class FilePaths {
    private static final String fileSep = File.separator;
    private static final String defaultStorePath =
        GetPropertyAction.privilegedGetProperty("java.home") +
        fileSep + "lib" + fileSep + "security";

    private static final String cacertsName = "cacerts";
    private static final String cacertsFilePath = JavaHome.isHermetic() ?
        cacertsName : defaultStorePath + fileSep + cacertsName;
    private static final File cacertsFile = JavaHome.isHermetic() ?
        null : new File(cacertsFilePath);

    public static InputStream cacertsStream() throws IOException {
        if (JavaHome.isHermetic()) {
            return FilePaths.class.getResourceAsStream(cacertsName);
        } else {
            if (!cacertsFile.exists()) {
                return null;
            }
            return new FileInputStream(cacertsFile);
        }
    }

    public static String cacerts() {
        return cacertsFilePath;
    }

    public static String defaultStore(String fileName) {
        if (JavaHome.isHermetic()) {
            return fileName;
        } else {
            return defaultStorePath + fileSep + fileName;
        }
    }
}
