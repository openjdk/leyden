/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "ci/ciEnv.hpp"
#include "ci/ciMetadata.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/compactHashtable.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "compiler/compileTask.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/resourceArea.hpp"
#include "oops/method.hpp"
#include "oops/methodCounters.hpp"
#include "oops/recompilationSchedule.hpp"
#include "oops/trainingData.hpp"
#include "runtime/arguments.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "utilities/growableArray.hpp"

TrainingData::TrainingDataSet TrainingData::_training_data_set(1024, 0x3fffffff);
TrainingData::TrainingDataDictionary TrainingData::_archived_training_data_dictionary;
TrainingData::TrainingDataDictionary TrainingData::_archived_training_data_dictionary_for_dumping;
TrainingData::DumptimeTrainingDataDictionary* TrainingData::_dumptime_training_data_dictionary = nullptr;
int TrainingData::TrainingDataLocker::_lock_mode;
volatile bool TrainingData::TrainingDataLocker::_snapshot = false;

MethodTrainingData::MethodTrainingData() {
  assert(CDSConfig::is_dumping_static_archive() || UseSharedSpaces, "only for CDS");
}

KlassTrainingData::KlassTrainingData() {
  assert(CDSConfig::is_dumping_static_archive() || UseSharedSpaces, "only for CDS");
}

CompileTrainingData::CompileTrainingData() : _level(-1), _compile_id(-1) {
  assert(CDSConfig::is_dumping_static_archive() || UseSharedSpaces, "only for CDS");
}

void TrainingData::initialize() {
  // this is a nop if training modes are not enabled
  if (have_data() || need_data()) {
    TrainingDataLocker::initialize();
  }
  RecompilationSchedule::initialize();
}

#if INCLUDE_CDS
static void verify_archived_entry(TrainingData* td, const TrainingData::Key* k) {
  guarantee(TrainingData::Key::can_compute_cds_hash(k), "");
  TrainingData* td1 = TrainingData::lookup_archived_training_data(k);
  guarantee(td == td1, "");
}
#endif

void TrainingData::verify() {
#if INCLUDE_CDS
  if (TrainingData::have_data()) {
    archived_training_data_dictionary()->iterate([&](TrainingData* td) {
      if (td->is_KlassTrainingData()) {
        KlassTrainingData* ktd = td->as_KlassTrainingData();
        if (ktd->has_holder() && ktd->holder()->is_loaded()) {
          Key k(ktd->holder());
          verify_archived_entry(td, &k);
        }
        ktd->verify();
      } else if (td->is_MethodTrainingData()) {
        MethodTrainingData* mtd = td->as_MethodTrainingData();
        if (mtd->has_holder() && mtd->holder()->method_holder()->is_loaded()) {
          Key k(mtd->holder());
          verify_archived_entry(td, &k);
        }
        mtd->verify();
      } else if (td->is_CompileTrainingData()) {
        td->as_CompileTrainingData()->verify();
      }
    });
  }
#endif
}

MethodTrainingData* MethodTrainingData::make(const methodHandle& method,
                                             bool null_if_not_found) {
  MethodTrainingData* mtd = nullptr;
  if (!have_data() && !need_data()) {
    return mtd;
  }
  // Try grabbing the cached value first.
  MethodCounters* mcs = method->method_counters();
  if (mcs != nullptr) {
    mtd = mcs->method_training_data();
    if (mtd != nullptr) {
      return mtd;
    }
  } else {
    mcs = Method::build_method_counters(Thread::current(), method());
  }

  KlassTrainingData* holder = KlassTrainingData::make(method->method_holder(), null_if_not_found);
  if (holder == nullptr) {
    return nullptr; // allocation failure
  }
  Key key(method());
  TrainingData* td = CDS_ONLY(have_data() ? lookup_archived_training_data(&key) :) nullptr;
  if (td != nullptr) {
    mtd = td->as_MethodTrainingData();
    method->init_training_data(mtd);  // Cache the pointer for next time.
    return mtd;
  } else {
    TrainingDataLocker l;
    td = training_data_set()->find(&key);
    if (td == nullptr && null_if_not_found) {
      return nullptr;
    }
    if (td != nullptr) {
      mtd = td->as_MethodTrainingData();
      method->init_training_data(mtd); // Cache the pointer for next time.
      return mtd;
    }
  }
  assert(td == nullptr && mtd == nullptr && !null_if_not_found, "Should return if have result");
  KlassTrainingData* ktd = KlassTrainingData::make(method->method_holder());
  if (ktd != nullptr) {
    TrainingDataLocker l;
    td = training_data_set()->find(&key);
    if (td == nullptr) {
      mtd = MethodTrainingData::allocate(method(), ktd);
      if (mtd == nullptr) {
        return nullptr; // allocation failure
      }
      td = training_data_set()->install(mtd);
      assert(td == mtd, "");
    } else {
      mtd = td->as_MethodTrainingData();
    }
    method->init_training_data(mtd);
  }
  return mtd;
}

void MethodTrainingData::print_on(outputStream* st, bool name_only) const {
  if (has_holder()) {
    _klass->print_on(st, true);
    st->print(".");
    name()->print_symbol_on(st);
    signature()->print_symbol_on(st);
  }
  if (name_only) {
    return;
  }
  if (!has_holder()) {
    st->print("[SYM]");
  }
  if (_level_mask) {
    st->print(" LM%d", _level_mask);
  }
  st->print(" mc=%p mdo=%p", _final_counters, _final_profile);
}

CompileTrainingData* CompileTrainingData::make(CompileTask* task) {
  int level = task->comp_level();
  int compile_id = task->compile_id();
  Thread* thread = Thread::current();
  methodHandle m(thread, task->method());
  MethodTrainingData* mtd = MethodTrainingData::make(m);
  if (mtd == nullptr) {
    return nullptr; // allocation failure
  }
  mtd->notice_compilation(level);

  TrainingDataLocker l;
  CompileTrainingData* ctd = CompileTrainingData::allocate(mtd, level, compile_id);
  if (ctd != nullptr) {
    if (mtd->_last_toplevel_compiles[level - 1] != nullptr) {
      if (mtd->_last_toplevel_compiles[level - 1]->compile_id() < compile_id) {
        mtd->_last_toplevel_compiles[level - 1]->clear_init_deps();
        mtd->_last_toplevel_compiles[level - 1] = ctd;
        mtd->_highest_top_level = MAX2(mtd->_highest_top_level, level);
      }
    } else {
      mtd->_last_toplevel_compiles[level - 1] = ctd;
      mtd->_highest_top_level = MAX2(mtd->_highest_top_level, level);
    }
  }
  return ctd;
}


void CompileTrainingData::dec_init_deps_left(KlassTrainingData* ktd) {
  LogStreamHandle(Trace, training) log;
  if (log.is_enabled()) {
    log.print("CTD "); print_on(&log); log.cr();
    log.print("KTD "); ktd->print_on(&log); log.cr();
  }
  assert(ktd!= nullptr && ktd->has_holder(), "");
  assert(_init_deps.contains(ktd), "");
  assert(_init_deps_left > 0, "");

  uint init_deps_left1 = Atomic::sub(&_init_deps_left, 1);

  if (log.is_enabled()) {
    uint init_deps_left2 = compute_init_deps_left();
    log.print("init_deps_left: %d (%d)", init_deps_left1, init_deps_left2);
    ktd->print_on(&log, true);
  }
}

uint CompileTrainingData::compute_init_deps_left(bool count_initialized) {
  int left = 0;
  for (int i = 0; i < _init_deps.length(); i++) {
    KlassTrainingData* ktd = _init_deps.at(i);
    // Ignore symbolic refs and already initialized classes (unless explicitly requested).
    if (ktd->has_holder()) {
      InstanceKlass* holder = ktd->holder();
      if (!ktd->holder()->is_initialized() || count_initialized) {
        ++left;
      } else if (holder->is_shared_unregistered_class()) {
        Key k(holder);
        if (CDS_ONLY(!Key::can_compute_cds_hash(&k)) NOT_CDS(true)) {
          ++left; // FIXME: !!! init tracking doesn't work well for custom loaders !!!
        }
      }
    }
  }
  return left;
}

void CompileTrainingData::print_on(outputStream* st, bool name_only) const {
  _method->print_on(st, true);
  st->print("#%dL%d", _compile_id, _level);
  if (name_only) {
    return;
  }
  #define MAYBE_TIME(Q, _qtime) \
    if (_qtime != 0) st->print(" " #Q "%.3f", _qtime)
  MAYBE_TIME(Q, _qtime);
  MAYBE_TIME(S, _stime);
  MAYBE_TIME(E, _etime);
  if (_init_deps.length() > 0) {
    if (_init_deps_left > 0) {
      st->print(" udeps=%d", _init_deps_left);
    }
    for (int i = 0, len = _init_deps.length(); i < len; i++) {
      st->print(" dep:");
      _init_deps.at(i)->print_on(st, true);
    }
  }
}

void CompileTrainingData::record_compilation_queued(CompileTask* task) {
  _qtime = tty->time_stamp().seconds();
}
void CompileTrainingData::record_compilation_start(CompileTask* task) {
  _stime = tty->time_stamp().seconds();
}
void CompileTrainingData::record_compilation_end(CompileTask* task) {
  _etime = tty->time_stamp().seconds();
  if (task->is_success()) {   // record something about the nmethod output
    _nm_total_size = task->nm_total_size();
  }
}
void CompileTrainingData::notice_inlined_method(CompileTask* task,
                                                const methodHandle& method) {
  MethodTrainingData* mtd = MethodTrainingData::make(method);
  if (mtd != nullptr) {
    mtd->notice_compilation(task->comp_level(), true);
  }
}

void CompileTrainingData::notice_jit_observation(ciEnv* env, ciBaseObject* what) {
  // A JIT is starting to look at class k.
  // We could follow the queries that it is making, but it is
  // simpler to assume, conservatively, that the JIT will
  // eventually depend on the initialization state of k.
  CompileTask* task = env->task();
  assert(task != nullptr, "");
  Method* method = task->method();
  InstanceKlass* compiling_klass = method->method_holder();
  if (what->is_metadata()) {
    ciMetadata* md = what->as_metadata();
    if (md->is_loaded() && md->is_instance_klass()) {
      ciInstanceKlass* cik = md->as_instance_klass();

      if (cik->is_initialized()) {
        InstanceKlass* ik = md->as_instance_klass()->get_instanceKlass();
        KlassTrainingData* ktd = KlassTrainingData::make(ik);
        if (ktd == nullptr) {
          // Allocation failure or snapshot in progress
          return;
        }
        // This JIT task is (probably) requesting that ik be initialized,
        // so add him to my _init_deps list.
        TrainingDataLocker l;
        add_init_dep(ktd);
      }
    }
  }
}

void KlassTrainingData::prepare(Visitor& visitor) {
  if (visitor.is_visited(this)) {
    return;
  }
  visitor.visit(this);
  ClassLoaderData* loader_data = nullptr;
  if (_holder != nullptr) {
    loader_data = _holder->class_loader_data();
  } else {
    loader_data = java_lang_ClassLoader::loader_data(SystemDictionary::java_system_loader()); // default CLD
  }
  _comp_deps.prepare(loader_data);
}

void MethodTrainingData::prepare(Visitor& visitor) {
  if (visitor.is_visited(this)) {
    return;
  }
  visitor.visit(this);
  klass()->prepare(visitor);
  if (has_holder()) {
    _final_counters = holder()->method_counters();
    _final_profile  = holder()->method_data();
    assert(_final_profile == nullptr || _final_profile->method() == holder(), "");
  }
  for (int i = 0; i < CompLevel_count; i++) {
    CompileTrainingData* ctd = _last_toplevel_compiles[i];
    if (ctd != nullptr) {
      ctd->prepare(visitor);
    }
  }
}

void CompileTrainingData::prepare(Visitor& visitor) {
  if (visitor.is_visited(this)) {
    return;
  }
  visitor.visit(this);
  method()->prepare(visitor);
  ClassLoaderData* loader_data = _method->klass()->class_loader_data();
  _init_deps.prepare(loader_data);
  _ci_records.prepare(loader_data);
}

KlassTrainingData* KlassTrainingData::make(InstanceKlass* holder, bool null_if_not_found) {
  Key key(holder);
  TrainingData* td = CDS_ONLY(have_data() ? lookup_archived_training_data(&key) :) nullptr;
  KlassTrainingData* ktd = nullptr;
  if (td != nullptr) {
    ktd = td->as_KlassTrainingData();
    guarantee(!ktd->has_holder() || ktd->holder() == holder, "");
    if (ktd->has_holder()) {
      return ktd;
    }
  }
  TrainingDataLocker l;
  td = training_data_set()->find(&key);
  if (td == nullptr) {
    if (null_if_not_found) {
      return nullptr;
    }
    ktd = KlassTrainingData::allocate(holder);
    if (ktd == nullptr) {
      return nullptr; // allocation failure
    }
    td = training_data_set()->install(ktd);
    assert(ktd == td, "");
  } else {
    ktd = td->as_KlassTrainingData();
    guarantee(ktd->holder() != nullptr, "null holder");
  }
  assert(ktd != nullptr, "");
  guarantee(ktd->holder() == holder, "");
  return ktd;
}

void KlassTrainingData::print_on(outputStream* st, bool name_only) const {
  if (has_holder()) {
    name()->print_symbol_on(st);
    switch (holder()->init_state()) {
      case InstanceKlass::allocated:            st->print("[A]"); break;
      case InstanceKlass::loaded:               st->print("[D]"); break;
      case InstanceKlass::linked:               st->print("[L]"); break;
      case InstanceKlass::being_initialized:    st->print("[i]"); break;
      case InstanceKlass::fully_initialized:    /*st->print("");*/ break;
      case InstanceKlass::initialization_error: st->print("[E]"); break;
      default: fatal("unknown state: %d", holder()->init_state());
    }
    if (holder()->is_interface()) {
      st->print("I");
    }
  } else {
    st->print("[SYM]");
  }
  if (name_only) {
    return;
  }
  if (_comp_deps.length() > 0) {
    for (int i = 0, len = _comp_deps.length(); i < len; i++) {
      st->print(" dep:");
      _comp_deps.at(i)->print_on(st, true);
    }
  }
}

KlassTrainingData::KlassTrainingData(InstanceKlass* klass) : TrainingData(klass) {
  if (holder() == klass) {
    return;   // no change to make
  }

  jobject hmj = _holder_mirror;
  if (hmj != nullptr) {   // clear out previous handle, if any
    _holder_mirror = nullptr;
    assert(JNIHandles::is_global_handle(hmj), "");
    JNIHandles::destroy_global(hmj);
  }

  // Keep the klass alive during the training run, unconditionally.
  //
  // FIXME: Revisit this decision; we could allow training runs to
  // unload classes in the normal way.  We might use make_weak_global
  // instead of make_global.
  //
  // The data from the training run would mention the name of the
  // unloaded class (and of its loader).  Is it worth the complexity
  // to track and then unload classes, remembering just their names?

  if (klass != nullptr) {
    Handle hm(JavaThread::current(), klass->java_mirror());
    hmj = JNIHandles::make_global(hm);
    Atomic::release_store(&_holder_mirror, hmj);
  }

  Atomic::release_store(&_holder, const_cast<InstanceKlass*>(klass));
  assert(holder() == klass, "");
}

void KlassTrainingData::notice_fully_initialized() {
  ResourceMark rm;
  assert(has_holder(), "");
  assert(holder()->is_initialized(), "wrong state: %s %s",
         holder()->name()->as_C_string(), holder()->init_state_name());

  TrainingDataLocker l; // Not a real lock if we don't collect the data,
                        // that's why we need the atomic decrement below.
  for (int i = 0; i < comp_dep_count(); i++) {
    comp_dep(i)->dec_init_deps_left(this);
  }
  holder()->set_has_init_deps_processed();
}

void TrainingData::init_dumptime_table(TRAPS) {
  if (!need_data()) {
    return;
  }
  _dumptime_training_data_dictionary = new DumptimeTrainingDataDictionary();
  if (CDSConfig::is_dumping_final_static_archive()) {
    _archived_training_data_dictionary.iterate([&](TrainingData* record) {
      _dumptime_training_data_dictionary->append(record);
    });
  } else {
    TrainingDataLocker l;
    TrainingDataLocker::snapshot();

    ResourceMark rm;
    Visitor visitor(training_data_set()->size());
    training_data_set()->iterate_all([&](const TrainingData::Key* k, TrainingData* td) {
      td->prepare(visitor);
      if (!td->is_CompileTrainingData()) {
        _dumptime_training_data_dictionary->append(td);
      }
    });

    if (VerifyTrainingData) {
      training_data_set()->verify();
    }
  }

  RecompilationSchedule::prepare(CHECK);
}

#if INCLUDE_CDS
void TrainingData::iterate_roots(MetaspaceClosure* it) {
  if (!need_data()) {
    return;
  }
  assert(_dumptime_training_data_dictionary != nullptr, "");
  for (int i = 0; i < _dumptime_training_data_dictionary->length(); i++) {
    _dumptime_training_data_dictionary->at(i).metaspace_pointers_do(it);
  }
  RecompilationSchedule::iterate_roots(it);
}

void TrainingData::dump_training_data() {
  if (!need_data()) {
    return;
  }
  write_training_data_dictionary(&_archived_training_data_dictionary_for_dumping);
}

void TrainingData::cleanup_training_data() {
  if (_dumptime_training_data_dictionary != nullptr) {
    ResourceMark rm;
    Visitor visitor(_dumptime_training_data_dictionary->length());
    for (int i = 0; i < _dumptime_training_data_dictionary->length(); i++) {
      TrainingData* td = _dumptime_training_data_dictionary->at(i).training_data();
      td->cleanup(visitor);
    }
    // Throw away all elements with empty keys
    int j = 0;
    for (int i = 0; i < _dumptime_training_data_dictionary->length(); i++) {
      TrainingData* td = _dumptime_training_data_dictionary->at(i).training_data();
      if (td->key()->is_empty()) {
        continue;
      }
      if (i != j) { // no need to copy if it's the same
        _dumptime_training_data_dictionary->at_put(j, td);
      }
      j++;
    }
    _dumptime_training_data_dictionary->trunc_to(j);
  }
  RecompilationSchedule::cleanup();
}

void KlassTrainingData::cleanup(Visitor& visitor) {
  if (visitor.is_visited(this)) {
    return;
  }
  visitor.visit(this);
  if (has_holder()) {
    bool is_excluded = !holder()->is_loaded() || SystemDictionaryShared::check_for_exclusion(holder(), nullptr);
    if (is_excluded) {
      ResourceMark rm;
      log_debug(cds)("Cleanup KTD %s", name()->as_klass_external_name());
      _holder = nullptr;
      key()->make_empty();
    }
  }
  for (int i = 0; i < _comp_deps.length(); i++) {
    _comp_deps.at(i)->cleanup(visitor);
  }
}

void MethodTrainingData::cleanup(Visitor& visitor) {
  if (visitor.is_visited(this)) {
    return;
  }
  visitor.visit(this);
  if (has_holder()) {
    if (SystemDictionaryShared::check_for_exclusion(holder()->method_holder(), nullptr)) {
      log_debug(cds)("Cleanup MTD %s::%s", name()->as_klass_external_name(), signature()->as_utf8());
      if (_final_profile != nullptr && _final_profile->method() != _holder) {
        log_warning(cds)("Stale MDO for  %s::%s", name()->as_klass_external_name(), signature()->as_utf8());
      }
      _holder = nullptr;
      key()->make_empty();
    }
  }
  for (int i = 0; i < CompLevel_count; i++) {
    CompileTrainingData* ctd = _last_toplevel_compiles[i];
    if (ctd != nullptr) {
      ctd->cleanup(visitor);
    }
  }
}

void KlassTrainingData::verify() {
  for (int i = 0; i < comp_dep_count(); i++) {
    CompileTrainingData* ctd = comp_dep(i);
    if (!ctd->_init_deps.contains(this)) {
      print_on(tty); tty->cr();
      ctd->print_on(tty); tty->cr();
    }
    guarantee(ctd->_init_deps.contains(this), "");
  }
}

void MethodTrainingData::verify() {
  iterate_all_compiles([](CompileTrainingData* ctd) {
    ctd->verify();

    int init_deps_left1 = ctd->init_deps_left();
    int init_deps_left2 = ctd->compute_init_deps_left();

    if (init_deps_left1 != init_deps_left2) {
      ctd->print_on(tty); tty->cr();
    }
    guarantee(init_deps_left1 == init_deps_left2, "mismatch: %d %d %d",
              init_deps_left1, init_deps_left2, ctd->init_deps_left());
  });
}

void CompileTrainingData::verify() {
  for (int i = 0; i < init_dep_count(); i++) {
    KlassTrainingData* ktd = init_dep(i);
    if (ktd->has_holder() && ktd->holder()->is_shared_unregistered_class()) {
      LogStreamHandle(Warning, training) log;
      if (log.is_enabled()) {
        ResourceMark rm;
        log.print("CTD "); print_value_on(&log);
        log.print(" depends on unregistered class %s", ktd->holder()->name()->as_C_string());
      }
    }
    if (!ktd->_comp_deps.contains(this)) {
      print_on(tty); tty->cr();
      ktd->print_on(tty); tty->cr();
    }
    guarantee(ktd->_comp_deps.contains(this), "");
  }
}

void CompileTrainingData::cleanup(Visitor& visitor) {
  if (visitor.is_visited(this)) {
    return;
  }
  visitor.visit(this);
  method()->cleanup(visitor);
}

void TrainingData::serialize_training_data(SerializeClosure* soc) {
  if (soc->writing()) {
    _archived_training_data_dictionary_for_dumping.serialize_header(soc);
  } else {
    _archived_training_data_dictionary.serialize_header(soc);
  }
  RecompilationSchedule::serialize_training_data(soc);
}

void TrainingData::print_archived_training_data_on(outputStream* st) {
  st->print_cr("Archived TrainingData Dictionary");
  TrainingDataPrinter tdp(st);
  TrainingDataLocker::initialize();
  _archived_training_data_dictionary.iterate(&tdp);
  RecompilationSchedule::print_archived_training_data_on(st);
}

void TrainingData::Key::metaspace_pointers_do(MetaspaceClosure *iter) {
  iter->push(const_cast<Metadata**>(&_meta));
}

void TrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  _key.metaspace_pointers_do(iter);
}

bool TrainingData::Key::can_compute_cds_hash(const Key* const& k) {
  return k->meta() == nullptr || MetaspaceObj::is_shared(k->meta());
}

uint TrainingData::Key::cds_hash(const Key* const& k) {
  return SystemDictionaryShared::hash_for_shared_dictionary((address)k->meta());
}

void TrainingData::write_training_data_dictionary(TrainingDataDictionary* dictionary) {
  if (!need_data()) {
    return;
  }
  assert(_dumptime_training_data_dictionary != nullptr, "");
  CompactHashtableStats stats;
  dictionary->reset();
  CompactHashtableWriter writer(_dumptime_training_data_dictionary->length(), &stats);
  for (int i = 0; i < _dumptime_training_data_dictionary->length(); i++) {
    TrainingData* td = _dumptime_training_data_dictionary->at(i).training_data();
#ifdef ASSERT
    for (int j = i+1; j < _dumptime_training_data_dictionary->length(); j++) {
      TrainingData* td1 = _dumptime_training_data_dictionary->at(j).training_data();
      assert(!TrainingData::Key::equals(td1, td->key(), -1), "conflict");
    }
#endif // ASSERT
    td = ArchiveBuilder::current()->get_buffered_addr(td);
    uint hash = TrainingData::Key::cds_hash(td->key());
    u4 delta = ArchiveBuilder::current()->buffer_to_offset_u4((address)td);
    writer.add(hash, delta);
  }
  writer.dump(dictionary, "training data dictionary");
}

size_t TrainingData::estimate_size_for_archive() {
  if (_dumptime_training_data_dictionary != nullptr) {
    return CompactHashtableWriter::estimate_size(_dumptime_training_data_dictionary->length());
  } else {
    return 0;
  }
}

TrainingData* TrainingData::lookup_archived_training_data(const Key* k) {
  // For this to work, all components of the key must be in shared metaspace.
  if (!TrainingData::Key::can_compute_cds_hash(k) || _archived_training_data_dictionary.empty()) {
    return nullptr;
  }
  uint hash = TrainingData::Key::cds_hash(k);
  TrainingData* td = _archived_training_data_dictionary.lookup(k, hash, -1 /*unused*/);
  if (td != nullptr) {
    if ((td->is_KlassTrainingData()  && td->as_KlassTrainingData()->has_holder()) ||
        (td->is_MethodTrainingData() && td->as_MethodTrainingData()->has_holder())) {
      return td;
    } else {
      ShouldNotReachHere();
    }
  }
  return nullptr;
}
#endif

KlassTrainingData* TrainingData::lookup_for(InstanceKlass* ik) {
#if INCLUDE_CDS
  if (TrainingData::have_data() && ik != nullptr && ik->is_loaded()) {
    TrainingData::Key key(ik);
    TrainingData* td = TrainingData::lookup_archived_training_data(&key);
    if (td != nullptr && td->is_KlassTrainingData()) {
      return td->as_KlassTrainingData();
    }
  }
#endif
  return nullptr;
}

MethodTrainingData* TrainingData::lookup_for(Method* m) {
#if INCLUDE_CDS
  if (TrainingData::have_data() && m != nullptr) {
    KlassTrainingData* holder_ktd = TrainingData::lookup_for(m->method_holder());
    if (holder_ktd != nullptr) {
      TrainingData::Key key(m);
      TrainingData* td = TrainingData::lookup_archived_training_data(&key);
      if (td != nullptr && td->is_MethodTrainingData()) {
        return td->as_MethodTrainingData();
      }
    }
  }
#endif
  return nullptr;
}

template <typename T>
void TrainingData::DepList<T>::metaspace_pointers_do(MetaspaceClosure* iter) {
  iter->push(&_deps);
}

void KlassTrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  log_trace(cds)("Iter(KlassTrainingData): %p", this);
#if INCLUDE_CDS
  TrainingData::metaspace_pointers_do(iter);
#endif
  _comp_deps.metaspace_pointers_do(iter);
  iter->push(&_holder);
}

void MethodTrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  log_trace(cds)("Iter(MethodTrainingData): %p", this);
#if INCLUDE_CDS
  TrainingData::metaspace_pointers_do(iter);
#endif
  iter->push(&_klass);
  iter->push((Method**)&_holder);
  for (int i = 0; i < CompLevel_count; i++) {
    iter->push(&_last_toplevel_compiles[i]);
  }
  iter->push(&_final_profile);
  iter->push(&_final_counters);
}

void CompileTrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  log_trace(cds)("Iter(CompileTrainingData): %p", this);
#if INCLUDE_CDS
  TrainingData::metaspace_pointers_do(iter);
#endif
  _init_deps.metaspace_pointers_do(iter);
  _ci_records.metaspace_pointers_do(iter);
  iter->push(&_method);
}

template <typename T>
void TrainingData::DepList<T>::prepare(ClassLoaderData* loader_data) {
  if (_deps == nullptr && _deps_dyn != nullptr) {
    int len = _deps_dyn->length();
    _deps = MetadataFactory::new_array_from_c_heap<T>(len, mtClassShared);
    for (int i = 0; i < len; i++) {
      _deps->at_put(i, _deps_dyn->at(i)); // copy
    }
  }
}

void TrainingDataPrinter::do_value(TrainingData* td) {
#ifdef ASSERT
#if INCLUDE_CDS
  TrainingData::Key key(td->key()->meta());
  assert(td == TrainingData::archived_training_data_dictionary()->lookup(td->key(), TrainingData::Key::cds_hash(td->key()), -1), "");
  assert(td == TrainingData::archived_training_data_dictionary()->lookup(&key, TrainingData::Key::cds_hash(&key), -1), "");
#endif
#endif // ASSERT

  const char* type = (td->is_KlassTrainingData()   ? "K" :
                      td->is_MethodTrainingData()  ? "M" :
                      td->is_CompileTrainingData() ? "C" : "?");
  _st->print("%4d: %p %s ", _index++, td, type);
  td->print_on(_st);
  _st->cr();
  if (td->is_KlassTrainingData()) {
    td->as_KlassTrainingData()->iterate_all_comp_deps([&](CompileTrainingData* ctd) {
      ResourceMark rm;
      _st->print_raw("  C ");
      ctd->print_on(_st);
      _st->cr();
    });
  } else if (td->is_MethodTrainingData()) {
    td->as_MethodTrainingData()->iterate_all_compiles([&](CompileTrainingData* ctd) {
      ResourceMark rm;
      _st->print_raw("  C ");
      ctd->print_on(_st);
      _st->cr();
    });
  } else if (td->is_CompileTrainingData()) {
    // ?
  }
}


#if INCLUDE_CDS
void KlassTrainingData::remove_unshareable_info() {
  TrainingData::remove_unshareable_info();
  _holder_mirror = nullptr;
  _comp_deps.remove_unshareable_info();
}

void MethodTrainingData::remove_unshareable_info() {
  TrainingData::remove_unshareable_info();
  if (_final_counters != nullptr) {
    _final_counters->remove_unshareable_info();
  }
  if (_final_profile != nullptr) {
    _final_profile->remove_unshareable_info();
  }
}

void CompileTrainingData::remove_unshareable_info() {
  TrainingData::remove_unshareable_info();
  _init_deps.remove_unshareable_info();
  _ci_records.remove_unshareable_info();
  _init_deps_left = compute_init_deps_left(true);
}

#endif // INCLUDE_CDS
