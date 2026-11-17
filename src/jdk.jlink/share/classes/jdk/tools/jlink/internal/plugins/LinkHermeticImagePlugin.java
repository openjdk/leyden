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

import static jdk.tools.jlink.internal.LinkableRuntimeImage.STATIC_LAUNCHER_EXECUTABLE;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.List;
import java.util.ListIterator;
import java.util.Map;
import java.util.Set;
import java.util.function.Function;
import java.util.function.Predicate;

import jdk.tools.jlink.internal.JlinkTask;
import jdk.tools.jlink.internal.Utils;
import jdk.tools.jlink.plugin.PluginException;
import jdk.tools.jlink.plugin.ResourcePool;
import jdk.tools.jlink.plugin.ResourcePoolBuilder;
import jdk.tools.jlink.plugin.ResourcePoolEntry;
import jdk.tools.jlink.plugin.ResourcePoolModule;

// FIXME: Linking command is platform and linker specific. Support
// different linkers on different platforms, in addition to ld/lld
// on Linux.

/**
 * NOTE: Comma separated --link-hermetic-image options does not work well
 *       as the native linking flag may contain ",".
 *       Each --link-hermetic-image occurrence supports one key=value sub-option,
 *       e.g.
 *
 *       --link-hermetic-image linker=<linker_path> \
 *       --link-hermetic-image "extra-link-flags=<flags>"
 *
 * Links launcher executable using the native linker, or uses a pre-linked
 * launcher executable.
 *
 * When performing native linking, the plugin builds the link command from
 * jmod packaged artifacts:
 *   - Java launcher linker flags in /java.base/bin/java.ldflags
 *   - /<module>/lib/<module>.ldflags of all linked modules
 *   - static libraries of linked modules, the java.base module includes
 *     liblauncher.a in addition to the JDK and VM's static libraries.
 *
 * Supported sub-options with --link-hermetic-image:
 *
 *   linker=<path>
 *       Native linker path, e.g. /usr/bin/g++. Currently, only ld/lld
 *       on Linux are supported.
 *
 *   link-command=<complete_linking_command>
 *       When this sub-option is present, the provided command is used for
 *       linking the launcher executable without constructing the linking
 *       command using jmod packaged .ldflags.
 *
 *   extra-link-flags=<flags>
 *       Additional flags appended to the link command constructed for
 *       {@code linker}. If {@code -o <output>} is present, <output>
 *       is used as the linked executable's path, otherwise the output
 *       executable is written to a temporary directory.
 *       NOTE: Requires linker.
 *
 * Example:
 * $ jlink --hermetic \
 *     --link-hermetic-image linker=/usr/bin/g++ \
 *     --module-path $STATIC_JDK/jmods:$APP/mods \
 *     --add-modules <module_name> \
 *     --output <demo_image_name>
 *
 * Or use a pre-linked static launcher executable:
 *
 *   --link-hermetic-image pre-linked-exe=<pre_linked_launcher_executable>
 */
public final class LinkHermeticImagePlugin extends AbstractPlugin {

    private static String executable;
    private static String linker;
    private static String linkOutput;
    private static List<String> linkCommand;
    private static List<String> extraLinkFlags = new ArrayList<>();

    private static boolean link = true;
    private static boolean collectNativeLibs = true;

    // For ld and lld support.
    private static final String WHOLE_ARCHIVE = "-Wl,--whole-archive";
    private static final String NO_WHOLE_ARCHIVE = "-Wl,--no-whole-archive";
    private static final String OUT = "-o";
    private static final String EXPORT_DYNAMIC = "-Wl,--export-dynamic";

    public LinkHermeticImagePlugin() {
        super("link-hermetic-image");
    }

    public static List<String> parseCommand(String cmd) {
        return Arrays.stream(cmd.split(" "))
                     .map((p) -> p.trim())
                     .filter((p) -> !p.isEmpty())
                     .toList();
    }

    private String findLinkOutput(List<String> cmd) {
        ListIterator<String> iter = cmd.listIterator();
        while (iter.hasNext()) {
            String s = iter.next();
            if (s.equals(OUT)) {
                return iter.next();
            }
        }
        return null;
    }

    @Override
    public void configure(Map<String, String> config) {
        String option = config.get(getName());

        String[] opt = option.split("=");
        if (opt.length != 2) {
            throw new IllegalArgumentException(getName() + ": " + config.get(getName()));
        }

        switch (opt[0]) {
            case "pre-linked-exe": {
                // FIXME: Check if the executable exists?
                executable = opt[1];
                link = false;
                collectNativeLibs = false;
                return;
            }

            case "linker": {
                linker = opt[1];
                return;
            }

            case "link-command": {
                collectNativeLibs = false;
                linkCommand = parseCommand(opt[1]);
                linkOutput = findLinkOutput(linkCommand);
                if (linkOutput != null) {
                    return;
                }
                throw new IllegalArgumentException("No output specified in linking command: " + opt[1]);
            }

            case "extra-link-flags": {
                extraLinkFlags = parseCommand(opt[1]);
                // It's not fatal if output is not specified in the extra link flags.
                linkOutput = findLinkOutput(extraLinkFlags);
                return;
            }

            default: {
                throw new IllegalArgumentException("Unsupported option " + opt[0]);
            }
        }
    }

    private byte[] linkAndGetExecutable() {
        if (link) {
            if (collectNativeLibs) {
                if (linker == null) {
                    throw new PluginException(getName() + ": linker is missing");
                }
                collectedFlags = linker
                    + " " + launcherLinkerFlags
                    + " " + OUT + " " + linkOutput
                    + " " + EXPORT_DYNAMIC
                    + " " + linkLibs
                    + " " + libLinkerFlags;
                List<String> linkFlags = parseCommand(collectedFlags);
                linkCommand = new ArrayList<>(linkFlags.size() + extraLinkFlags.size());
                linkCommand.addAll(linkFlags);
                linkCommand.addAll(extraLinkFlags);
            }
            if (JlinkTask.DEBUG) {
                System.err.println("Native linking using command:");
                System.err.println("  " + String.join(" ", linkCommand));
            }
            ProcessBuilder builder = new ProcessBuilder(linkCommand);
            int status = -1;
            try {
                Process p = builder.inheritIO().start();
                status = p.waitFor();
            } catch (InterruptedException | IOException e) {
                throw new PluginException(e);
            }
            if (status != 0) {
                throw new PluginException("Native linking failed, exit code " + status
                                          + ": " + String.join(" ", linkCommand));
            }
            executable = linkOutput;
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

    private static String collectedFlags = "";
    private static String launcherLinkerFlags = "";
    private static String libLinkerFlags = "";
    private static String linkLibs = "";
    private static Path tmpDir;

    // TODO: Use list from static-lib/module-included-libs.txt.
    private static final Set<String> EXCLUDED_LIBS = Set.of(
        "libsplashscreen.a", "libjsig.a", "libawt_headless.a");

    @Override
    public ResourcePool transform(ResourcePool in, ResourcePoolBuilder out) {
        if (!collectNativeLibs) {
            in.transformAndCopy(Function.identity(), out);
        } else {
            // Collect the list of static native libraries and form the
            // command for native linking, if no pre-linked launcher executable
            // or native linking command is provided.
            try {
                tmpDir = Files.createTempDirectory("jlink-natives");
                linkLibs += " " + WHOLE_ARCHIVE;

                in.transformAndCopy(res -> {
                    if (res.type().equals(ResourcePoolEntry.Type.NATIVE_LIB)) {
                        // FIXME: No need to check suffix if only static libraries are
                        //        included in static jmod. Note that the current check
                        //        is platform specific.
                        String entryPath = res.path();
                        if (entryPath.endsWith(".a")) {
                            String lib = entryPath.substring(entryPath.lastIndexOf('/') + 1);
                            if (EXCLUDED_LIBS.contains(lib)) {
                                return res;
                            }
                            // Extract the static library into the temp directory.
                            Path p = tmpDir.resolve(lib);
                            try {
                                res.content().transferTo(Files.newOutputStream(p));
                            } catch (IOException ioe) { throw new PluginException(ioe); }
                            linkLibs += " " + p.toString();
                        } else if (res.path().endsWith(".ldflags")) {
                            // TODO: I think we can add a new resource type for jmod packaged
                            //       .ldflags. For now, just hardcode explicit checks for
                            //       files ending with .ldflags.
                            libLinkerFlags += " " + new String(res.contentBytes(), StandardCharsets.UTF_8).trim();
                        }
                    } else if (res.type().equals(ResourcePoolEntry.Type.NATIVE_CMD) &&
                               res.path().endsWith("bin/java.ldflags")) {
                        launcherLinkerFlags =
                            new String(res.contentBytes(), StandardCharsets.UTF_8).trim();
                    }
                    return res;
                }, out);

                linkLibs += " " + NO_WHOLE_ARCHIVE;
            } catch (IOException e) {
                throw new PluginException(e);
            }

            if (linkOutput == null) {
                // User provided extra link flags do not specify output.
                linkOutput = tmpDir.resolve("static-launcher-executable").toString();
            }
        }

        try {
            byte[] data = linkAndGetExecutable();
            out.add(ResourcePoolEntry.create(STATIC_LAUNCHER_EXECUTABLE,
                                             ResourcePoolEntry.Type.NATIVE_CMD,
                                             data));
        } finally {
            if (tmpDir != null) {
               deleteDirRecursivelyIgnoreResult(tmpDir);
            }
        }
        return out.build();
    }

    // FIXME:
    // Copied from src/jdk.jlink/linux/classes/jdk/tools/jlink/internal/plugins/StripNativeDebugSymbolsPlugin.java
    // Perhaps move to src/jdk.jlink/share/classes/jdk/tools/jlink/internal/Utils.java.
    private void deleteDirRecursivelyIgnoreResult(Path tempDir) {
        try {
            Files.walkFileTree(tempDir, new SimpleFileVisitor<Path>() {
                @Override
                public FileVisitResult visitFile(Path file,
                        BasicFileAttributes attrs) throws IOException {
                    Files.delete(file);
                    return FileVisitResult.CONTINUE;
                }

                @Override
                public FileVisitResult postVisitDirectory(Path dir,
                        IOException exc) throws IOException {
                    Files.delete(dir);
                    return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException e) {
            // ignore deleting the temp dir
        }
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
