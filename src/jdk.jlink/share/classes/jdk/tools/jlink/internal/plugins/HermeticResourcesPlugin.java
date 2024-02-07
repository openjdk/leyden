/*
 * Copyright 2024 Google, Inc.  All Rights Reserved.
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

import java.util.EnumSet;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Predicate;

import jdk.tools.jlink.internal.Utils;
import jdk.tools.jlink.plugin.ResourcePool;
import jdk.tools.jlink.plugin.ResourcePoolBuilder;
import jdk.tools.jlink.plugin.ResourcePoolEntry;

/**
 * A plugin to add hermetic packaged JDK resources in 'modules' (jimage) for
 * specified CONFIG entries. The original CONFIG entries are also included
 * in the output runtime image.
 *
 * To enable the plugin with jlink option:
 *
 *   -- hermetic-resources <config_file_path1>=<module_resource_path1>,<config_file_path2>=<module_resource_path2>,...
 */
public final class HermeticResourcesPlugin extends AbstractPlugin {

    private final Map<String, String> hermeticResources = new HashMap<>();

    public HermeticResourcesPlugin() {
        super("hermetic-resources");
    }

    @Override
    public void configure(Map<String, String> config) {
        List<String> resources = Utils.parseList(config.get(getName()));

        for (String res : resources) {
           String[] paths = res.split("=");
           if (paths.length != 2) {
               throw new IllegalArgumentException(getName() + ": " + config.get(getName()));
           }
           hermeticResources.put(paths[0], paths[1]);
        }
    }

    @Override
    public ResourcePool transform(ResourcePool in, ResourcePoolBuilder out) {
        in.transformAndCopy(res -> {
            if (res.type().equals(ResourcePoolEntry.Type.CONFIG)) {
                String hermeticResource = hermeticResources.get(res.path());
                if (hermeticResource != null) {
                    out.add(ResourcePoolEntry.create(hermeticResource,
                                                     ResourcePoolEntry.Type.CLASS_OR_RESOURCE,
                                                     res.contentBytes()));
                    return res;
                }
            }
            return res;
        }, out);
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
