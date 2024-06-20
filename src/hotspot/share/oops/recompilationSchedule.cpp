/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/methodProfiler.hpp"
#include "code/nmethod.hpp"
#include "oops/recompilationSchedule.hpp"
#include "oops/trainingData.hpp"

Array<MethodTrainingData*>* RecompilationSchedule::_schedule = nullptr;
Array<MethodTrainingData*>* RecompilationSchedule::_schedule_for_dumping = nullptr;
volatile bool* RecompilationSchedule::_status = nullptr;

void RecompilationSchedule::initialize() {
  if (TrainingData::have_data()) {
    if (_schedule != nullptr && _schedule->length() > 0) {
      const int size = _schedule->length();
      _status = NEW_C_HEAP_ARRAY(bool, size, mtCompiler);
      for (int i = 0; i < size; i++) {
        _status[i] = false;
      }
    }
  }
}

void RecompilationSchedule::prepare(TRAPS) {
  if (!TrainingData::need_data()) {
    return;
  }
  auto nmethods = MethodProfiler::sampled_nmethods();
  GrowableArray<MethodTrainingData*> dyn_schedule;
  for (auto it = nmethods->begin(); it != nmethods->end(); ++it) {
    nmethod* nm = *it;
    if (RecordOnlyTopCompilations && nm->method_profiling_count() == 0) {
      break;
    }
    if (nm->method() != nullptr) {
      MethodTrainingData* mtd = nm->method()->training_data_or_null();
      if (mtd != nullptr) {
        dyn_schedule.append(mtd);
      }
    }
  }
  delete nmethods;
  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  _schedule_for_dumping = MetadataFactory::new_array<MethodTrainingData*>(loader_data, dyn_schedule.length(), CHECK);
  int i = 0;
  for (auto it = dyn_schedule.begin(); it != dyn_schedule.end(); ++it) {
    _schedule_for_dumping->at_put(i++, *it);
  }
}

#if INCLUDE_CDS
void RecompilationSchedule::iterate_roots(MetaspaceClosure* it) {
  if (!TrainingData::need_data()) {
    return;
  }
  it->push(&_schedule_for_dumping);
}

void RecompilationSchedule::cleanup() {
  _status = nullptr;
}

void RecompilationSchedule::serialize_training_data(SerializeClosure* soc) {
  if (soc->writing()) {
    soc->do_ptr(&_schedule_for_dumping);
  } else {
    soc->do_ptr(&_schedule);
  }
}


void RecompilationSchedule::print_archived_training_data_on(outputStream* st) {
  if (_schedule != nullptr && _schedule->length() > 0) {
    st->print_cr("Archived TrainingData Recompilation Schedule");
    for (int i = 0; i < _schedule->length(); i++) {
      st->print("%4d: ", i);
      MethodTrainingData* mtd = _schedule->at(i);
      if (mtd != nullptr) {
        mtd->print_on(st);
      } else {
        st->print("nullptr");
      }
      st->cr();
    }
  }
}
#endif //INCLUDE_CDS