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
import java.util.Arrays;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.List;
import java.util.ListIterator;
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
 * NOTE: Comma separated --link-hermetic-image options does not work well
 *       as the native linking flag may contain ",".
 *
 * Link launcher executable using the native linker. Supported options with
 * --link-hermetic-image:
 *
 *   link-command=<complete_linking_command>
 *
 *   extra-link-flags=<flags>
 *
 *   launcher-lib=<static_lib_with_launcher_main>
 *
 * Or use a pre-linked static launcher executable:
 *
 *   --link-hermetic-image pre-linked-exe=<pre_linked_launcher_executable>
 */
public final class LinkHermeticImagePlugin extends AbstractPlugin {

    private static String executable;
    private static String linkOutput;
    private static List<String> linkCommand;

    public LinkHermeticImagePlugin() {
        super("link-hermetic-image");
    }

    public static List<String> parseCommand(String cmd) {
        return Arrays.stream(cmd.split(" "))
                     .map((p) -> p.trim())
                     .filter((p) -> !p.isEmpty())
                     .toList();
    }

    @Override
    public void configure(Map<String, String> config) {
        //List<String> options = Utils.parseList(config.get(getName()));
        String option = config.get(getName());

        String[] opt = option.split("=");
        if (opt.length != 2) {
            throw new IllegalArgumentException(getName() + ": " + config.get(getName()));
        }

        switch (opt[0]) {
            case "pre-linked-exe": {
                // Check if the executable exists?
                executable = opt[1];
                return;
            }

            case "link-command": {
                // FIXME: Linking command is platform and linker specific. How
                // do we support different linkers on different platforms?
                // For now just support ld or lld on Linux.
                linkCommand = parseCommand(opt[1]);

                ListIterator<String> iter = linkCommand.listIterator();
                while (iter.hasNext()) {
                    String s = iter.next();
                    if (s.equals("-o")) {
                        linkOutput = iter.next();
                        return;
                    }
                }
                throw new IllegalArgumentException("No output specified in linking command: " + opt[1]);
            }

            case "launcher-lib": {
                // TODO
                break;
            }

            case "extra-link-flags": {
                // TODO
                break;
            }

            default: {
                throw new IllegalArgumentException("Unsupported option " + opt[0]);
            }
        }
    }

    private byte[] link() {
        if (executable == null || executable.equals("")) {
            if (linkCommand != null) {
                ProcessBuilder builder = new ProcessBuilder(linkCommand);
                int status = -1;
                try {
                    Process p = builder.inheritIO().start();
                    status = p.waitFor();
                } catch (InterruptedException | IOException e) {
                    throw new PluginException(e);
                }
                executable = linkOutput;
            }
        }

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
        // TODO: Collect the list of static native libraries and form the
        // command for native linking, if no pre-linked launcher executable
        // or native linking command is provided.
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
