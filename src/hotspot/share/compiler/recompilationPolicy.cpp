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

#include "compiler/compilationPolicy.hpp"
#include "compiler/recompilationPolicy.hpp"
#include "oops/method.inline.hpp"
#include "oops/recompilationSchedule.hpp"
#include "runtime/handles.inline.hpp"

RecompilationPolicy::LoadAverage RecompilationPolicy::_load_average;
volatile bool RecompilationPolicy::_recompilation_done = false;

void RecompilationPolicy::sample_load_average() {
  if (AOTRecompilation) {
    const int c2_queue_size = CompileBroker::queue_size(CompLevel_full_optimization);
    _load_average.sample(c2_queue_size);
  }
}

void RecompilationPolicy::print_load_average(outputStream* st) {
  st->print(" load=%lf", _load_average.value());
}

bool RecompilationPolicy::have_recompilation_work() {
  if (AOTRecompilation && TrainingData::have_data() && RecompilationSchedule::have_schedule() &&
                          RecompilationSchedule::length() > 0 && !_recompilation_done) {
    if (_load_average.value() <= AOTRecompilationLoadAverageThreshold) {
      return true;
    }
  }
  return false;
}

bool RecompilationPolicy::recompilation_step(int step, TRAPS) {
  if (!have_recompilation_work() || os::elapsedTime() < AOTDelayRecompilation) {
    return false;
  }

  const int size = RecompilationSchedule::length();
  int i = 0;
  int count = 0;
  bool repeat = false;
  for (; i < size && count < step; i++) {
    if (!RecompilationSchedule::status_at(i)) {
      MethodTrainingData* mtd = RecompilationSchedule::at(i);
      if (!mtd->has_holder()) {
        RecompilationSchedule::set_status_at(i, true);
        continue;
      }
      const Method* method = mtd->holder();
      InstanceKlass* klass = method->method_holder();
      if (klass->is_not_initialized()) {
        repeat = true;
        continue;
      }
      nmethod *nm = method->code();
      if (nm == nullptr) {
        repeat = true;
        continue;
      }

      if (!AOTForceRecompilation && !(nm->is_aot() && nm->comp_level() == CompLevel_full_optimization)) {
        // If it's already online-compiled at level 4, mark it as done.
        if (nm->comp_level() == CompLevel_full_optimization) {
          RecompilationSchedule::set_status_at(i, true);
        } else {
          repeat = true;
        }
        continue;
      }
      if (RecompilationSchedule::claim_at(i)) {
        const methodHandle m(THREAD, const_cast<Method*>(method));
        CompLevel next_level = CompLevel_full_optimization;

        if (method->method_data() == nullptr) {
          CompilationPolicy::create_mdo(m, THREAD);
        }

        if (PrintTieredEvents) {
          CompilationPolicy::print_event(CompilationPolicy::FORCE_RECOMPILE, m(), m(), InvocationEntryBci, next_level);
        }
        CompileBroker::compile_method(m, InvocationEntryBci, CompLevel_full_optimization, 0,
                                      true /*requires_online_compilation*/, CompileTask::Reason_MustBeCompiled, THREAD);
        if (HAS_PENDING_EXCEPTION) {
          CLEAR_PENDING_EXCEPTION;
        }
        count++;
      }
    }
  }

  if (i == size && !repeat) {
    Atomic::release_store(&_recompilation_done, true);
  }
  return count > 0;
}
