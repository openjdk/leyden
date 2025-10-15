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

#include "cds/aotMetaspace.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsEndTrainingUpcall.hpp"
#include "compiler/methodMatcher.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/runtimeUpcalls.hpp"

uint volatile  CDSEndTrainingUpcall::_count = 0;
uint           CDSEndTrainingUpcall::_limit = 1;
int  volatile  CDSEndTrainingUpcall::_triggered = 0;
BasicMatcher*  CDSEndTrainingUpcall::_matcher = nullptr;

bool cdsEndTrainingUpcall_register_upcalls()
{
  if (!CDSConfig::is_dumping_preimage_static_archive_with_triggers()) {
    return true;
  }
  return CDSEndTrainingUpcall::register_upcalls();
}

bool CDSEndTrainingUpcall::register_upcalls()
{
  if (!FLAG_IS_DEFAULT(AOTEndTrainingOnMethodEntry)) {
    if (CDSEndTrainingUpcall::parse_vm_command(AOTEndTrainingOnMethodEntry)) {
      return RuntimeUpcalls::register_upcall(
            RuntimeUpcallType::onMethodEntry,
            "end_training_check",
            CDSEndTrainingUpcall::end_training_check,
            CDSEndTrainingUpcall::filter_method_callback
            );
    }
    return false;
  }
  return true;
}

JRT_ENTRY(void, CDSEndTrainingUpcall::end_training_check(JavaThread* current))
{
    if (_triggered == 0) {
      AtomicAccess::inc(&_count);
      if(_count >= _limit) {
        CDSEndTrainingUpcall::end_training(current);
      }
    }
}
JRT_END

bool CDSEndTrainingUpcall::end_training(JavaThread* current)
{
  if (_triggered == 0) {
    if (AtomicAccess::cmpxchg(&_triggered, 0, 1) == 0) {
      AOTMetaspace::dump_static_archive(current);
      assert(!current->has_pending_exception(), "Unexpected exception");
      return true;
    }
  }
  return false;
}

bool CDSEndTrainingUpcall::filter_method_callback(MethodDetails& method_details)
{
  if (_matcher != nullptr) {
    return _matcher->match(method_details);
  }
  return false;
}

bool CDSEndTrainingUpcall::parse_vm_command(ccstrlist command)
{
  assert(command != nullptr, "sanity");
  ResourceMark rm;
  const char* error_msg = nullptr;
  char* copy = os::strdup(command, mtInternal);
  char* line = copy;
  char* method_pattern;
  int num_patterns = 0;
  bool error = false;
  const char* seperator_str = ",";
  const char* count_str = "count=";
  const size_t count_str_len = strlen(count_str);
  do {
    if (line[0] == '\0') {
      break;
    }
    method_pattern = strtok_r(line, seperator_str, &line);
    if (method_pattern != nullptr) {
      // if method pattern starts with count=, then parse the count
      if (strncmp(method_pattern, count_str, count_str_len) == 0) {
        int number = atoi(method_pattern + count_str_len);
        if (number > 0) {
          CDSEndTrainingUpcall::set_limit((uint)number);
          continue;
        }
        error_msg = "count must be a valid integer > 0";
      } else {
        BasicMatcher* matcher = BasicMatcher::parse_method_pattern(method_pattern, error_msg, false);
        if (matcher != nullptr) {
          if (_matcher != nullptr) {
            matcher->set_next(_matcher);
          }
          _matcher = matcher;
          num_patterns++;
          continue;
        }
      }
    }
    ttyLocker ttyl;
    tty->print_cr("An error occurred during parsing AOTEndTrainingOnMethodEntry");
    if (error_msg != nullptr) {
      tty->print_cr("Error: %s", error_msg);
    }
    tty->print_cr("Line: '%s'", command);
    error = true;
  } while (!error && method_pattern != nullptr && line != nullptr);
  os::free(copy);
  if (num_patterns == 0) {
    tty->print_cr("Error: No method patterns found in AOTEndTrainingOnMethodEntry");
    error = true;
  }
  return !error;
}
