/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#ifndef SHARE_COMPILER_PRECOMPILER_HPP
#define SHARE_COMPILER_PRECOMPILER_HPP

#include "memory/allStatic.hpp"
#include "utilities/exceptions.hpp"

class ArchiveBuilder;

class Precompiler : AllStatic  {
private:
  static int _total_count;

public:
  static void compile_aot_code(CompLevel search_level, bool for_preload, CompLevel comp_level, TRAPS);
  static void compile_aot_code(TRAPS);
  static void compile_aot_code(ArchiveBuilder* builder, TRAPS);
};

#endif // SHARE_COMPILER_PRECOMPILER_HPP
