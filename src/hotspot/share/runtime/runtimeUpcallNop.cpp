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

#include "runtime/globals_extension.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/runtimeUpcallNop.hpp"
#include "runtime/runtimeUpcalls.hpp"

bool RuntimeUpcallNop::_method_filter_result = false;

bool runtimeUpcallNop_register_upcalls()
{
  return RuntimeUpcallNop::register_upcalls();
}

bool RuntimeUpcallNop::register_upcalls()
{
  if(AddRuntimeUpcallsNOP == nullptr || FLAG_IS_DEFAULT(AddRuntimeUpcallsNOP)) return true;

  const char* method_entry = "onMethodEntry:";
  const size_t method_entry_len = strlen(method_entry);
  const char* method_exit = "onMethodExit:";
  const size_t method_exit_len = strlen(method_exit);

  const char* filter_all = "all";
  const size_t filter_all_len = strlen(filter_all);
  const char* filter_none = "none";
  const size_t filter_none_len = strlen(filter_none);

  const char* filter_option = nullptr;
  RuntimeUpcallType upcall_type = RuntimeUpcallType::onMethodEntry;
  const char* command = AddRuntimeUpcallsNOP == nullptr ? "" : AddRuntimeUpcallsNOP;

  if (strncmp(command, method_entry, method_entry_len) == 0) {
    filter_option = command + method_entry_len;
    upcall_type = RuntimeUpcallType::onMethodEntry;
  } else if (strncmp(command, method_exit, method_exit_len) == 0) {
    filter_option = command + method_exit_len;
    upcall_type = RuntimeUpcallType::onMethodExit;
  } else {
    ttyLocker ttyl;
    tty->print_cr("An error occurred during parsing AddRuntimeUpcallsNOP");
    tty->print_cr("Error! Expected 'onMethodEntry:' or 'onMethodExit:'");
    return false;
  }

  assert(filter_option != nullptr, "sanity");
  if (strncmp(filter_option, filter_all, filter_all_len) == 0) {
    _method_filter_result = true;
  } else if (strncmp(filter_option, filter_none, filter_none_len) == 0) {
    _method_filter_result = false;
  } else {
    ttyLocker ttyl;
    tty->print_cr("An error occurred during parsing AddRuntimeUpcallsNOP");
    tty->print_cr("Error! Expected 'all' or 'none'");
    return false;
  }

  if (RuntimeUpcalls::register_upcall(
        upcall_type,
        "nop_method",
        RuntimeUpcallNop::nop_method,
        RuntimeUpcallNop::filter_method_callback)) {
    return true;
  }
  return false;
}

bool RuntimeUpcallNop::filter_method_callback(MethodDetails& method_details)
{
  return _method_filter_result;
}

JRT_ENTRY(void, RuntimeUpcallNop::nop_method(JavaThread* current))
{
}
JRT_END
