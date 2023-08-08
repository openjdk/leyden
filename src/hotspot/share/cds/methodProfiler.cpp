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
#include "cds/methodProfiler.hpp"
#include "code/codeBlob.hpp"
#include "code/nmethod.hpp"
#include "logging/log.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handshake.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/registerMap.hpp"
#include "runtime/task.hpp"

volatile uint64_t MethodProfiler::_num_samples = 0;
class MethodProfilerTask : public PeriodicTask {
public:
  MethodProfilerTask()
    : PeriodicTask(RecordOptCompilationOrderInterval) {}

  void task() {
    MethodProfiler::tick();
  }
};

static MethodProfilerTask* _task;

void MethodProfiler::initialize() {
  if (RecordOptCompilationOrder) {
    _task = new MethodProfilerTask();
    _task->enroll();
  }
}

class MethodProfilerClosure : public HandshakeClosure {
public:
MethodProfilerClosure()
    : HandshakeClosure("MethodProfiler") {}

  void do_thread(Thread* thread) {
    ResourceMark rm;

    if (thread != Thread::current()) {
      // Run by the VM thread - implication is that the target
      // thread was blocked or in native, i.e. not executing
      // Java code.
      return;
    }

    JavaThread* jt = JavaThread::cast(thread);
    if (!jt->has_last_Java_frame()) {
      return;
    }

    frame fr = jt->last_frame();
    if (fr.is_safepoint_blob_frame()) {
      RegisterMap rm(jt,
                     RegisterMap::UpdateMap::skip,
                     RegisterMap::ProcessFrames::skip,
                     RegisterMap::WalkContinuation::skip);
      fr = fr.sender(&rm);
    }

    if (!fr.is_compiled_frame()) {
      return;
    }

    nmethod* nm = fr.cb()->as_nmethod();
    if (!nm->is_compiled_by_c2()) {
      return;
    }

    log_trace(cds, profiling)("%s sampled %s::%s: " UINT64_FORMAT,
                              thread->name(),
                              nm->method()->method_holder()->name()->as_C_string(),
                              nm->method()->name()->as_C_string(),
                              nm->method_profiling_count());

    // Found a C2 top frame that was just executing - sample it
    nm->inc_method_profiling_count();
    Atomic::inc(&MethodProfiler::_num_samples);
  }
};

void MethodProfiler::tick() {
  MethodProfilerClosure cl;
  Handshake::execute(&cl);
}

static int method_hotness_compare(nmethod** a, nmethod** b) {
  return (*b)->method_profiling_count() - (*a)->method_profiling_count();
}


GrowableArrayCHeap<nmethod*, mtClassShared>* MethodProfiler::sampled_nmethods() {
  GrowableArrayCHeap<nmethod*, mtClassShared>* nmethods = new GrowableArrayCHeap<nmethod*, mtClassShared>();

  {
    MutexLocker mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    NMethodIterator iter(NMethodIterator::only_not_unloading);
    while(iter.next()) {
      nmethod* nm = iter.method();
      if (nm->is_compiled_by_c2() || nm->is_compiled_by_jvmci()) {
        nmethods->append(nm);
      }
    }
  }

  nmethods->sort(&method_hotness_compare);

  return nmethods;
}

double MethodProfiler::hotness(nmethod* nm) {
  return double(nm->method_profiling_count()) / double(_num_samples) * 100.0;
}

uint64_t MethodProfiler::num_samples() {
  return _num_samples;
}

void MethodProfiler::process_method_hotness() {
  if (_num_samples == 0) {
    return;
  }

  _task->disenroll();

  LogTarget(Debug, cds, profiling) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    double accumulated_sample_percent = 0.0;
    int i = 0;
    GrowableArrayCHeap<nmethod*, mtClassShared>* nmethods = sampled_nmethods();

    for (auto it = nmethods->begin(); it != nmethods->end(); ++it) {
      ++i;
      nmethod* nm = *it;
      if (nm->method_profiling_count() == 0) {
        break;
      }
      double sample_percent = hotness(nm);
      accumulated_sample_percent += sample_percent;
      log_info(cds, profiling)("%d (%.2f). %s::%s: " UINT64_FORMAT " (%.2f%%, %.2f%% accumulated)",
                               i,
                               double(i) / double(nmethods->length()) * 100.0,
                               nm->method()->method_holder()->name()->as_C_string(),
                               nm->method()->name()->as_C_string(),
                               nm->method_profiling_count(),
                               sample_percent,
                               accumulated_sample_percent);
    }

    delete nmethods;
  }
}
