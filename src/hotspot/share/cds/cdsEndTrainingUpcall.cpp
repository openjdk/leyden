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
#include "cds/cdsEndTrainingUpcall.hpp"
#include "cds/metaspaceShared.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/runtimeUpcalls.hpp"

uint volatile  CDSEndTrainingUpcall::_count = 0;
uint           CDSEndTrainingUpcall::_limit = 1;
int  volatile  CDSEndTrainingUpcall::_triggered = 0;
MethodPattern* CDSEndTrainingUpcall::_method_pattern = nullptr;

bool cdsEndTrainingUpcall_register_upcalls()
{
  if (!CDSConfig::is_dumping_preimage_static_archive_with_triggers()) {
    return true;
  }
  return CDSEndTrainingUpcall::register_upcalls();
}

bool CDSEndTrainingUpcall::register_upcalls() {
  if (CDSEndTrainingUpcall::parse_vm_command()) {
    return RuntimeUpcalls::register_upcall(
          RuntimeUpcallType::onMethodEntry,
          CDSEndTrainingUpcall::_method_pattern,
          "end_training_check",
          CDSEndTrainingUpcall::end_training_check
          );
  }
  return false;
}

JRT_ENTRY(void, CDSEndTrainingUpcall::end_training_check(JavaThread* current))
{
    if (_triggered == 0) {
      Atomic::inc(&_count);
      if(_count >= _limit) {
        CDSEndTrainingUpcall::end_training(current);
      }
    }
}
JRT_END

bool CDSEndTrainingUpcall::end_training(JavaThread* current)
{
  if (_triggered == 0) {
    if (Atomic::cmpxchg(&_triggered, 0, 1) == 0) {
      MetaspaceShared::preload_and_dump(current);
      assert(!current->has_pending_exception(), "must be");
     return true;
    }
  }
  return false;
}

bool CDSEndTrainingUpcall::include_method_callback(Symbol* class_name, Symbol* method_name, Symbol* signature)
{
  return false;
}

bool CDSEndTrainingUpcall::parse_vm_command()
{
  CDSEndTrainingUpcall::_method_pattern = MethodPattern::Create(AOTEndTrainingOnMethodEntry);

  /*ResourceMark rm;
  char error_buf[1024] = {0};
  LineCopy original(command);
  char* method_pattern;
  do {
    if (line[0] == '\0') {
      break;
    }
    method_pattern = strtok_r(line, ",", &line);
    if (method_pattern != nullptr) {
      // if method pattern starts with count=, then parse the count
      if (count != nullptr && strncmp(method_pattern, "count=", 6) == 0) {
        int number = atoi(method_pattern + 6);
        if (number > 0) {
          CDSEndTraining::set_limit((uint)number);
          continue;
        }
        strcpy(error_buf, "count must be a valid integer > 0");
      } else {
        TypedMethodOptionMatcher* matcher = TypedMethodOptionMatcher::parse_method_pattern(method_pattern, error_buf, sizeof(error_buf));
        if (matcher != nullptr) {
          if (result != nullptr) {
            result = new MethodPattern(result, matcher);
          }
          register_command(matcher, command, true);
          continue;
        }
      }
    }
    ttyLocker ttyl;
    tty->print_cr("%s: An error occurred during parsing", error_prefix);
    if (*error_buf != '\0') {
      tty->print_cr("Error: %s", error_buf);
    }
    tty->print_cr("Line: '%s'", original.get());
    return nullptr;
  } while (method_pattern != nullptr && line != nullptr);*/
  return true;
}