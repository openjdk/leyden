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

#include "precompiled.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsNOPUpcall.hpp"
#include "cds/metaspaceShared.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/runtimeUpcalls.hpp"

MethodPattern* cdsNOPUpcall_parse_command(const char* command)
{
  MethodPattern* result = MethodPattern::Create(nullptr);
  return result;
}

bool cdsNOPUpcall_register_upcalls()
{
  if (!CDSConfig::is_dumping_preimage_static_archive_with_triggers()) {
    return true;
  }

  MethodPattern* methodPattern = cdsNOPUpcall_parse_command(AOTEndTrainingOnMethodEntry);
  if (methodPattern != nullptr) {
    if (RuntimeUpcalls::register_upcall(
          RuntimeUpcallType::onMethodEntry,
          methodPattern,
          "nop",
          CDSNOPUpcall::nop
          )) {
        return true;
      }
  }
  return false;
}

JRT_ENTRY(void, CDSNOPUpcall::nop(JavaThread* current))
{
  // Do nothing
  return;
}
JRT_END
