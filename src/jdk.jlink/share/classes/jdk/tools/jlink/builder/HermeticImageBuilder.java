/*
 * Copyright (c) 2025 Red Hat, Inc.
 *
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

package jdk.tools.jlink.builder;

import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;
import java.util.Set;

import jdk.tools.jlink.internal.ExecutableImage;
import jdk.tools.jlink.internal.Platform;
import jdk.tools.jlink.plugin.ResourcePool;

public final class HermeticImageBuilder implements ImageBuilder {

    private final Platform platform;
    private final Path hermeticImage;
    private final Set<String> modules = new HashSet<String>();

    public HermeticImageBuilder(Platform platform, Path hermeticImage) {
        this.hermeticImage = hermeticImage;
        this.platform = platform;
    }

    @Override
    public DataOutputStream getJImageOutputStream() {
        try {
            OutputStream fos = Files.newOutputStream(hermeticImage);
            BufferedOutputStream bos = new BufferedOutputStream(fos);
            return new DataOutputStream(bos);
        } catch (IOException ex) {
            throw new UncheckedIOException(ex);
        }
    }

    @Override
    public Path getJImageFile() {
        return hermeticImage;
    }

    @Override
    public void storeFiles(ResourcePool files) {
        files.moduleView().modules().forEach(m -> {
            // Only add modules that contain packages
            if (!m.packages().isEmpty()) {
                modules.add(m.name());
            }
        });
    }

    @Override
    public ExecutableImage getExecutableImage() {
        return new HermeticExecutableImage(platform, hermeticImage, modules);
    }

    static final class HermeticExecutableImage implements ExecutableImage {

        private final Set<String> modules;
        private final Platform platform;
        private final Path imagePath;

        private HermeticExecutableImage(Platform platform, Path image,
                Set<String> modules) {
            this.imagePath = Objects.requireNonNull(image);
            this.platform = platform;
            this.modules = Collections.unmodifiableSet(modules);
        }

        @Override
        public Path getHome() {
            return imagePath;
        }

        @Override
        public Set<String> getModules() {
            return modules;
        }

        @Override
        public List<String> getExecutionArgs() {
            // TODO What should that be?
            return null;
        }

        @Override
        public void storeLaunchArgs(List<String> args) {
            // TODO Implement
        }

        @Override
        public Platform getTargetPlatform() {
            return platform;
        }

    }

}
