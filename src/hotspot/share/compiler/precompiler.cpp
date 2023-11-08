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
#include "cds/archiveBuilder.hpp"
#include "cds/cdsAccess.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/runTimeClassInfo.hpp"
#include "code/SCCache.hpp"
#include "compiler/compiler_globals.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/precompiler.hpp"
#include "memory/allocation.hpp"
#include "oops/trainingData.hpp"

class PrecompileIterator : StackObj {
private:
  Thread* _thread;
  GrowableArray<Method*> _methods;

  static nmethod* precompile(Method* m, TRAPS) {
    assert(m->method_holder()->is_linked(), "required");

    methodHandle mh(THREAD, m);
    assert(!HAS_PENDING_EXCEPTION, "");
    return CompileBroker::compile_method(mh, InvocationEntryBci, CompLevel_full_optimization, methodHandle(), 0,
                                         true /*requires_online_comp*/, CompileTask::Reason_Precompile,
                                         THREAD);
  }

  static nmethod* precompile(Method* m, ArchiveBuilder* builder, TRAPS) {
    nmethod* code = precompile(m, THREAD);
    bool status = (!HAS_PENDING_EXCEPTION) && (code != nullptr);

    static int count = 0;
    static CompiledMethod* last = nullptr;
    Method* requested_m = builder->to_requested(builder->get_buffered_addr(m));
    ++count;

    if (log_is_enabled(Info, precompile)) {
      int isz = 0;
      int delta = 0;
      if (status) {
        isz = code->insts_size();
        if (last != nullptr) {
          delta = (int) (address(code) - address(last));
        }
        last = code;
      }

      ResourceMark rm;
      log_info(precompile)("[%4d] Compiled %s [%p -> %p] (%s) code = %p insts_size = %d delta = %d", count,
                           m->external_name(), m, requested_m,
                           status ? "success" : "FAILED", code, isz, delta);
    }
    return code;
  }

public:
  PrecompileIterator(): _thread(Thread::current()) {
    assert(TrainingData::have_data(), "sanity");
  }

  bool include(Method* m) {
    if (m->is_native() || m->is_abstract()) {
      return false;
    }
    DirectiveSet* directives = DirectivesStack::getMatchingDirective(methodHandle(_thread, m), nullptr);
    if (directives->DontPrecompileOption) {
      return false; // excluded
    } else if (directives->PrecompileRecordedOption > 0) {
      return true;
    }
    int cid = compile_id(m, CompLevel_full_optimization);
    return (cid < INT_MAX);
  }

  void do_value(const RunTimeClassInfo* record) {
    Array<Method*>* methods = record->_klass->methods();
    for (int i = 0; i < methods->length(); i++) {
      Method* m = methods->at(i);
      if (include(m)) {
        _methods.push(m);
      }
    }
  }
  void do_value(TrainingData* td) {
    if (td->is_MethodTrainingData()) {
      MethodTrainingData* mtd = td->as_MethodTrainingData();
      if (mtd->has_holder() && include((Method*)mtd->holder())) {
        _methods.push((Method*)mtd->holder());
      }
    }
  }

  static int compile_id(Method* m, int level) {
    MethodTrainingData* mtd = TrainingData::lookup_for(m);
    if (mtd != nullptr) {
      CompileTrainingData* ctd = mtd->last_toplevel_compile(level);
      if (ctd != nullptr) {
        return ctd->compile_id();
      }
    }
    return INT_MAX; // treat as the last compilation
  }

  static int compare_by_compile_id(Method** m1, Method** m2) {
    int id1 = compile_id(*m1, CompLevel_full_optimization);
    int id2 = compile_id(*m2, CompLevel_full_optimization);
    return (id1 - id2);
  }

  static void sort_methods_by_compile_id(GrowableArray<Method*>* methods) {
    methods->sort(&compare_by_compile_id);
  }

  void precompile(ArchiveBuilder* builder, TRAPS) {
    sort_methods_by_compile_id(&_methods);

    for (int i = 0; i < _methods.length(); i++) {
      Method* m = _methods.at(i);

      assert(!HAS_PENDING_EXCEPTION, "");
      precompile(m, builder, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        CLEAR_PENDING_EXCEPTION;
      }
    }
  }
};

// New workflow only
void Precompiler::compile_cached_code(ArchiveBuilder* builder, TRAPS) {
  assert(CDSConfig::is_dumping_final_static_archive() && StoreCachedCode, "sanity");
  if (TrainingData::have_data()) {
    ResourceMark rm;
    PrecompileIterator pi;

    SCCache::new_workflow_start_writing_cache();

    TrainingData::archived_training_data_dictionary()->iterate(&pi);
    pi.precompile(builder, THREAD);

    SCCache::new_workflow_end_writing_cache();
  }
}
