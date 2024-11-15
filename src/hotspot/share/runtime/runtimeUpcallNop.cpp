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
#include "runtime/globals_extension.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/runtimeUpcallNop.hpp"
#include "runtime/runtimeUpcalls.hpp"

bool RuntimeUpcallNop::methodFilterResult = false;

bool runtimeUpcallNop_register_upcalls()
{
  if(AddRuntimeUpcallsNOP == nullptr || FLAG_IS_DEFAULT(AddRuntimeUpcallsNOP)) return true;

  const char* methodEntry = "onMethodEntry:";
  const size_t methodEntryLen = strlen(methodEntry);
  const char* methodExit = "onMethodExit:";
  const size_t methodExitLen = strlen(methodExit);

  const char* filterAll = "all";
  const size_t filterAllLen = strlen(filterAll);
  const char* filterNone = "none";
  const size_t filterNoneLen = strlen(filterNone);

  const char* filterOption = nullptr;
  RuntimeUpcallType upcallType = RuntimeUpcallType::onMethodEntry;
  const char* command = AddRuntimeUpcallsNOP == nullptr ? "" : AddRuntimeUpcallsNOP;

  if (strncmp(command, methodEntry, methodEntryLen) == 0) {
    filterOption = command + methodEntryLen;
    upcallType = RuntimeUpcallType::onMethodEntry;
  } else if (strncmp(command, methodExit, methodExitLen) == 0) {
    filterOption = command + methodExitLen;
    upcallType = RuntimeUpcallType::onMethodExit;
  } else {
    ttyLocker ttyl;
    tty->print_cr("An error occurred during parsing AddRuntimeUpcallsNOP");
    tty->print_cr("Error! Expected 'onMethodEntry:' or 'onMethodExit:'");
    return false;
  }

  assert(filterOption != nullptr, "sanity");
  if (strncmp(filterOption, filterAll, filterAllLen) == 0) {
    RuntimeUpcallNop::methodFilterResult = true;
  } else if (strncmp(filterOption, filterNone, filterNoneLen) == 0) {
    RuntimeUpcallNop::methodFilterResult = false;
  } else {
    ttyLocker ttyl;
    tty->print_cr("An error occurred during parsing AddRuntimeUpcallsNOP");
    tty->print_cr("Error! Expected 'all' or 'none'");
    return false;
  }

  if (RuntimeUpcalls::register_upcall(
        upcallType,
        "nop_method",
        RuntimeUpcallNop::nop_method,
        RuntimeUpcallNop::filter_method_callback)) {
    return true;
  }
  return false;
}

bool RuntimeUpcallNop::filter_method_callback(MethodDetails& methodDetails)
{
  return methodFilterResult;
}

JRT_ENTRY(void, RuntimeUpcallNop::nop_method(JavaThread* current))
{
}
JRT_END
