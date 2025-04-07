/*
 * Copyright 2025 Google, Inc.  All Rights Reserved.
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
package jdk.tools.jlink.internal.plugins;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.function.Function;
import java.util.function.Predicate;

import jdk.tools.jlink.internal.Utils;
import jdk.tools.jlink.plugin.PluginException;
import jdk.tools.jlink.plugin.ResourcePool;
import jdk.tools.jlink.plugin.ResourcePoolBuilder;
import jdk.tools.jlink.plugin.ResourcePoolEntry;

/**
 * Link executable using the native linker specified by --link-hermetic-image.
 */
public final class LinkHermeticImagePlugin extends AbstractPlugin {

    private String executable;

    public LinkHermeticImagePlugin() {
        super("link-hermetic-image");
    }

    @Override
    public void configure(Map<String, String> config) {
        // FIXME: Handle the config as the executable path only for now.
        executable = config.get(getName());
    }

    private byte[] link() {
        // TODO: Perform native linking. 
        byte[] data = null;
        try {
            Path p = Paths.get(executable);
            data = Files.readAllBytes(p);
        } catch (IOException e) {
            throw new PluginException("Cannot read " + executable);
        }
        return data;
    }

    @Override
    public ResourcePool transform(ResourcePool in, ResourcePoolBuilder out) {
        in.transformAndCopy(Function.identity(), out);

        byte[] data = link();
        out.add(ResourcePoolEntry.create("/java.base/bin/static-java",
                                         ResourcePoolEntry.Type.NATIVE_CMD,
                                         data));
        return out.build();
    }

    @Override
    public Category getType() {
        return Category.PROCESSOR;
    }


    @Override
    public boolean hasArguments() {
        return true;
    }
}
