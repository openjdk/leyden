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
#include "logging/logStream.hpp"
#include "memory/allocation.hpp"
#include "oops/trainingData.hpp"
#include "runtime/handles.inline.hpp"

static int precompiled_total_count = 0;

class PrecompileIterator : StackObj {
private:
  CompLevel _comp_level;
  CompLevel _search_level;
  bool _for_preload;
  Thread* _thread;
  GrowableArray<Method*> _methods;

  bool precompile(Method* m, TRAPS) {
    assert(!HAS_PENDING_EXCEPTION, "");
    assert(m->method_holder()->is_linked(), "required");

    ++precompiled_total_count;

    uint entries_cnt_before = SCCache::store_entries_cnt();

    methodHandle mh(THREAD, m);
    CompileTask::CompileReason compile_reason = (_for_preload ? CompileTask::Reason_PrecompileForPreload
                                                              : CompileTask::Reason_Precompile);
    nmethod* nm = CompileBroker::compile_method(mh, InvocationEntryBci, _comp_level, methodHandle(), 0,
                                                true /*requires_online_comp*/, compile_reason,
                                                THREAD);

    uint entries_cnt_after = SCCache::store_entries_cnt();

    return (nm != nullptr) || (entries_cnt_after > entries_cnt_before);
  }

  bool precompile(Method* m, ArchiveBuilder* builder, TRAPS) {
    bool status = precompile(m, THREAD);

    LogStreamHandle(Info, precompile) log;
    if (log.is_enabled()) {
      ResourceMark rm;
      log.print("[%4d] T%d Compiled %s [%p",
                precompiled_total_count, _comp_level + (_for_preload ? 1 : 0), m->external_name(), m);
      if (builder != nullptr) {
        Method* requested_m = builder->to_requested(builder->get_buffered_addr(m));
        log.print(" -> %p", requested_m);
      }
      log.print("] [%d] (%s)", SCCache::store_entries_cnt(), (status ? "success" : "FAILED"));
    }
    return status;
  }

public:
  PrecompileIterator(CompLevel comp_level, bool for_preload, CompLevel search_level, JavaThread* thread)
  : _comp_level(comp_level), _search_level(search_level), _for_preload(for_preload), _thread(thread) {
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
    int cid = compile_id(m, _search_level);
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
    if (mtd != nullptr && mtd->highest_level() == level) {
      CompileTrainingData* ctd = mtd->last_toplevel_compile(level);
      if (ctd != nullptr) {
        return ctd->compile_id();
      }
    }
    return INT_MAX; // treat as the last compilation
  }

  static int compare_by_compile_id(Method** m1, Method** m2, CompLevel comp_level) {
    int id1 = compile_id(*m1, comp_level);
    int id2 = compile_id(*m2, comp_level);
    return (id1 - id2);
  }

  static int compare_by_compile_id_tier1(Method** m1, Method** m2) {
    return compare_by_compile_id(m1, m2, CompLevel_simple);
  }

  static int compare_by_compile_id_tier2(Method** m1, Method** m2) {
    return compare_by_compile_id(m1, m2, CompLevel_limited_profile);
  }

  static int compare_by_compile_id_tier3(Method** m1, Method** m2) {
    return compare_by_compile_id(m1, m2, CompLevel_full_profile);
  }

  static int compare_by_compile_id_tier4(Method** m1, Method** m2) {
    return compare_by_compile_id(m1, m2, CompLevel_full_optimization);
  }

  void sort_methods_by_compile_id(GrowableArray<Method*>* methods) {
    switch(_search_level) {
      case CompLevel_simple:            methods->sort(&compare_by_compile_id_tier1); break;
      case CompLevel_limited_profile:   methods->sort(&compare_by_compile_id_tier2); break;
      case CompLevel_full_profile:      methods->sort(&compare_by_compile_id_tier3); break;
      case CompLevel_full_optimization: methods->sort(&compare_by_compile_id_tier4); break;

      default: fatal("%d", _search_level);
    }
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
void Precompiler::compile_cached_code(CompLevel search_level, bool for_preload, CompLevel comp_level, TRAPS) {
  ResourceMark rm;
  PrecompileIterator pi(comp_level, for_preload, search_level, THREAD);
  TrainingData::archived_training_data_dictionary()->iterate(&pi);
  pi.precompile((ArchiveBuilder*)nullptr, THREAD);
}

void Precompiler::compile_cached_code(TRAPS) {
  log_info(precompile)("Precompilation started");
  if (TrainingData::have_data()) {
    TrainingData::archived_training_data_dictionary()->iterate([&](TrainingData* td) {
      if (td->is_KlassTrainingData()) {
        KlassTrainingData *ktd = td->as_KlassTrainingData();
        if (ktd->has_holder()) {
          assert(!HAS_PENDING_EXCEPTION, "");
          ktd->holder()->link_class(THREAD);
          if (HAS_PENDING_EXCEPTION) {
            LogStreamHandle(Warning, precompile) log;
            if (log.is_enabled()) {
              ResourceMark rm;
              log.print("Linkage failed for %s: ", ktd->holder()->external_name());
              PENDING_EXCEPTION->print_on(&log);
            }
            CLEAR_PENDING_EXCEPTION;
          }
        }
      }
    });

    compile_cached_code(CompLevel_full_optimization, true, CompLevel_full_optimization, CHECK);

    compile_cached_code(CompLevel_full_optimization, false, CompLevel_full_optimization, CHECK);
    compile_cached_code(CompLevel_full_profile,      false, CompLevel_limited_profile,   CHECK);
    compile_cached_code(CompLevel_limited_profile,   false, CompLevel_limited_profile,   CHECK);
    compile_cached_code(CompLevel_simple,            false, CompLevel_simple,            CHECK);
  }
  log_info(precompile)("Precompilation finished (total: %d)", precompiled_total_count);
}

// New workflow only
void Precompiler::compile_cached_code(ArchiveBuilder* builder, TRAPS) {
  assert(CDSConfig::is_dumping_final_static_archive() && StoreCachedCode, "sanity");
  if (TrainingData::have_data()) {
    ResourceMark rm;

    SCCache::new_workflow_start_writing_cache();

    {
      PrecompileIterator pi(CompLevel_full_optimization, true /*for_preload*/, CompLevel_full_optimization, THREAD);
      TrainingData::archived_training_data_dictionary()->iterate(&pi);
      pi.precompile(builder, THREAD);
    }

    for (int level = CompLevel_simple; level <= CompLevel_full_optimization; level++) {
      CompLevel comp_level = (CompLevel)level;
      if (comp_level == CompLevel_full_profile) {
        comp_level = CompLevel_limited_profile;
      }
      PrecompileIterator pi(comp_level, false /*for_preload*/, (CompLevel)level, THREAD);
      TrainingData::archived_training_data_dictionary()->iterate(&pi);
      pi.precompile(builder, THREAD);
    }

    SCCache::new_workflow_end_writing_cache();
  }
}
