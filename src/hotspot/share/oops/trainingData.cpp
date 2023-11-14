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

#include <cds/archiveBuilder.hpp>
#include <classfile/systemDictionaryShared.hpp>
#include "precompiled.hpp"
#include "ci/ciEnv.hpp"
#include "ci/ciMetadata.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/metaspaceShared.hpp"
#include "cds/methodDataDictionary.hpp"
#include "cds/methodProfiler.hpp"
#include "cds/runTimeClassInfo.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/compactHashtable.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "compiler/compileTask.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/resourceArea.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/method.hpp"
#include "oops/methodCounters.hpp"
#include "oops/trainingData.hpp"
#include "runtime/arguments.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/xmlstream.hpp"

TrainingData::TrainingDataSet TrainingData::_training_data_set(1024, 0x3fffffff);
TrainingDataDictionary TrainingData::_archived_training_data_dictionary;
TrainingDataDictionary TrainingData::_archived_training_data_dictionary_for_dumping;
GrowableArrayCHeap<DumpTimeTrainingDataInfo, mtClassShared>* TrainingData::_dumptime_training_data_dictionary = nullptr;
Array<MethodTrainingData*>* TrainingData::_recompilation_schedule = nullptr;
Array<MethodTrainingData*>* TrainingData::_recompilation_schedule_for_dumping = nullptr;
volatile bool* TrainingData::_recompilation_status = nullptr;
int TrainingData::TrainingDataLocker::_lock_mode;
TrainingData::Options TrainingData::_options;

void TrainingData::Options::parse() {
  if (TrainingOptions != nullptr) {
    const char delimiter[] = " ,";
    size_t length = strlen(TrainingOptions);
    char* options_list = NEW_C_HEAP_ARRAY(char, length + 1, mtCompiler);
    strncpy(options_list, TrainingOptions, length + 1);
    char* save_ptr;
    char* token = strtok_r(options_list, delimiter, &save_ptr);
    while (token != nullptr) {
      if (strcmp(token, "xml") == 0) {
        set_boolean_option(BooleanOption::XML);
      } else if (strcmp(token, "cds") == 0) {
        set_boolean_option(BooleanOption::CDS);
      } else {
        vm_exit_during_initialization(err_msg("TrainingOptions: \'%s\' flag unknown, please correct it", token));
      }
      token = strtok_r(nullptr, delimiter, &save_ptr);
    }
    FREE_C_HEAP_ARRAY(char, options_list);
  }
  // Don't allow both XML and CDS options
  if (get_boolean_option(BooleanOption::CDS) && get_boolean_option(BooleanOption::XML)) {
    vm_exit_during_initialization(err_msg("TrainingOptions: cds and xml options cannot be specified together"));
  }
  // If neither XML nor CDS are specified, choose CDS.
  if (!get_boolean_option(BooleanOption::CDS) && !get_boolean_option(BooleanOption::XML)) {
    set_boolean_option(BooleanOption::CDS);
  }
}

void TrainingData::Options::print_on(outputStream* st) {
  st->print("TrainingData::Options:");
  if (get_boolean_option(BooleanOption::CDS)) {
    st->print(" cds");
  }
  if (get_boolean_option(BooleanOption::XML)) {
    st->print(" xml");
  }
  st->cr();
}

#if INCLUDE_CDS
void TrainingData::restore_all_unshareable_info(TRAPS) {
  class RestoreUnshareableInfo {
    JavaThread *_thread;
  public:
    RestoreUnshareableInfo(JavaThread *thread) : _thread(thread) { }
    void do_value(TrainingData* td) {
      td->restore_unshareable_info(_thread);
    }
  };

  if (have_data()) {
    if (!archived_training_data_dictionary()->empty()) {
      RestoreUnshareableInfo r(THREAD);
      archived_training_data_dictionary()->iterate(&r);
    }
  }
}
#endif

void TrainingData::initialize() {
  // this is a nop if training modes are not enabled
  if (have_data() || need_data()) {
    TrainingDataLocker::initialize();
  }
  options()->parse();

  if (have_data()) {
    if (use_xml()) {
      load_profiles();
    }
    // Initialize dependency tracking
    training_data_set()->iterate_all([](const Key* k, TrainingData* td) {
      if (td->is_MethodTrainingData()) {
        td->as_MethodTrainingData()->initialize_deps_tracking();
      }
    });

    if (_recompilation_schedule != nullptr && _recompilation_schedule->length() > 0) {
      const int size = _recompilation_schedule->length();
      _recompilation_status = NEW_C_HEAP_ARRAY(bool, size, mtCompiler);
      for (int i = 0; i < size; i++) {
        _recompilation_status[i] = false;
      }
    }
  }
}

TrainingData::Key::Key(const KlassTrainingData* klass, Symbol* method_name, Symbol* signature)
  : Key(method_name, signature, klass) {}

TrainingData::Key::Key(const InstanceKlass* klass)
  : Key(klass->name(), klass->class_loader_name_and_id()) {
  // Often the loader is either null or the string "'app'" (w/ extra quotes).
  // It can also be "'platform'".
}

TrainingData::Key::Key(const Method* method)
  : Key(method->name(), method->signature(), KlassTrainingData::make(method->method_holder()))
{}

MethodTrainingData* MethodTrainingData::make(KlassTrainingData* klass, Symbol* name, Symbol* signature) {
  Key key(klass, name, signature);
  TrainingData* td = have_data() ? lookup_archived_training_data(&key) : nullptr;
  MethodTrainingData* mtd = nullptr;
  if (td != nullptr) {
    mtd = td->as_MethodTrainingData();
    return mtd;
  }
  TrainingDataLocker l;
  td = training_data_set()->find(&key);
  if (td != nullptr) {
    mtd = td->as_MethodTrainingData();
  } else {
    mtd = MethodTrainingData::allocate(klass, name, signature);
    td = training_data_set()->install(mtd);
    assert(td == mtd, "");
  }
  assert(mtd != nullptr, "");
  return mtd;
}

MethodTrainingData* MethodTrainingData::make(KlassTrainingData* klass,
                                             const char* name, const char* signature) {
  TempNewSymbol n = SymbolTable::new_symbol(name);
  TempNewSymbol s = SymbolTable::new_symbol(signature);
  return make(klass, n, s);
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
    if (mtd != nullptr)  return mtd;
  } else {
    mcs = Method::build_method_counters(Thread::current(), method());
  }

  KlassTrainingData* holder = KlassTrainingData::make(method->method_holder(), null_if_not_found);
  assert(holder != nullptr || null_if_not_found, "");
  if (holder == nullptr) {
    return nullptr;
  }
  Key key(method->name(), method->signature(), holder);
  TrainingData* td = have_data()? lookup_archived_training_data(&key) : nullptr;
  if (td != nullptr) {
    mtd = td->as_MethodTrainingData();
    mtd->refresh_from(method());
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
      mtd->refresh_from(method());
      method->init_training_data(mtd); // Cache the pointer for next time.
      return mtd;
    }
  }
  assert(td == nullptr && mtd == nullptr && !null_if_not_found, "Should return if have result");
  KlassTrainingData* ktd = KlassTrainingData::make(method->method_holder());
  {
    TrainingDataLocker l;
    td = training_data_set()->find(&key);
    if (td == nullptr) {
      mtd = MethodTrainingData::allocate(ktd, method());
      td = training_data_set()->install(mtd);
      assert(td == mtd, "");
    } else {
      mtd = td->as_MethodTrainingData();
    }
    mtd->refresh_from(method());
    method->init_training_data(mtd);
  }
  return mtd;
}

void MethodTrainingData::print_on(outputStream* st, bool name_only) const {
  _klass->print_on(st, true);
  st->print(".");
  name()->print_symbol_on(st);
  signature()->print_symbol_on(st);
  if (name_only)  return;
  if (!has_holder())  st->print("[SYM]");
  if (_do_not_dump)  st->print("[DND]");
  if (_level_mask)  st->print(" LM%d", _level_mask);
  st->print(" mc=%p mdo=%p", _final_counters, _final_profile);
}

void MethodTrainingData::refresh_from(const Method* method) {
  if (method == nullptr || method == _holder)  return;
  _holder = method;
}

CompileTrainingData* CompileTrainingData::make(CompileTask* task,
                                               Method* inlined_method) {
  int level = task->comp_level();
  int compile_id = task->compile_id();
  Thread* thread = Thread::current();
  methodHandle top_method(thread, task->method());
  methodHandle this_method;
  if (inlined_method == nullptr || inlined_method == top_method()) {
    this_method = top_method;
  } else {
    this_method = methodHandle(thread, inlined_method);
  }
  MethodTrainingData* topm = MethodTrainingData::make(top_method);
  MethodTrainingData* thism = topm;
  if (inlined_method != top_method()) {
    thism = MethodTrainingData::make(this_method);
  }
  auto tdata = CompileTrainingData::make(thism, topm, level, compile_id);
  return tdata;
}

CompileTrainingData* CompileTrainingData::make(MethodTrainingData* this_method,
                                               MethodTrainingData* top_method,
                                               int level, int compile_id) {
  assert(level > CompLevel_none, "not a compiled level");
  top_method->notice_compilation(level);
  if (this_method != top_method) {
    this_method->notice_compilation(level, true);
  }

  // Find the insertion point.  Also check for duplicate records.
  CompileTrainingData* *insp = &this_method->_compile;
  while ((*insp) != nullptr && (*insp)->compile_id() > compile_id) {
    insp = &(*insp)->_next;
  }
  while ((*insp) != nullptr && (*insp)->compile_id() == compile_id) {
    if ((*insp)->method() == this_method &&
        (*insp)->top_method() == top_method) {
      break;
    }
  }

  auto tdata = CompileTrainingData::allocate(this_method, top_method, level, compile_id);

  // Link it into the method, under a lock.
  TrainingDataLocker l;
  while ((*insp) != nullptr && (*insp)->compile_id() == compile_id) {
    if ((*insp)->method() == this_method &&
        (*insp)->top_method() == top_method) {
      delete tdata;
      return (*insp);
    }
  }
  tdata->_next = (*insp);
  (*insp) = tdata;
  if (top_method->_last_toplevel_compiles[level - 1] == nullptr || top_method->_last_toplevel_compiles[level - 1]->compile_id() < compile_id) {
    top_method->_last_toplevel_compiles[level - 1] = tdata;
    top_method->_highest_top_level = MAX2(top_method->_highest_top_level, level);
  }
  return tdata;
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

  Atomic::sub(&_init_deps_left, 1);
}

void CompileTrainingData::print_on(outputStream* st, bool name_only) const {
  _method->print_on(st, true);
  if (is_inlined()) {
    st->print("/");
    _top_method->print_on(st, true);
  }
  st->print("#%dL%d", _compile_id, _level);
  if (name_only)  return;
  if (_do_not_dump)  st->print("[DND]");
  #define MAYBE_TIME(Q, _qtime) \
    if (_qtime != 0)  st->print(" " #Q "%.3f", _qtime)
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
  //CompileTrainingData::make(task, method);
  // all this does is put a mark on the method:
  auto mtd = MethodTrainingData::make(method);
  if (mtd != nullptr)  mtd->notice_compilation(task->comp_level(), true);
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
        KlassTrainingData* ktd = ik->training_data_or_null();
        if (ktd != nullptr) {
          ktd->record_touch_common(env->log(), "jit", task,
                                   compiling_klass, nullptr,
                                   method->name(), method->signature(),
                                   nullptr);
          // This JIT task is (probably) requesting that ik be initialized,
          // so add him to my _init_deps list.
          TrainingDataLocker l;
          add_init_dep(ktd);
        }
      }
    }
  }
}

static int cmp_zeroes_to_end(int id1, int id2) {
  int cmp = id1 - id2;
  // sort zeroes to the end, not the start
  return (id1 == 0 || id2 == 0) ? -cmp : cmp;
}

int CompileTrainingData::cmp(const TrainingData* tdata) const {
  if (this == tdata)  return 0;
  if (tdata->is_CompileTrainingData()) {
    const CompileTrainingData* that = tdata->as_CompileTrainingData();
    if (this->method() == that->method()) {  // (or top_method?)
      int cmp = cmp_zeroes_to_end(this->level(),
                                  that->level());
      if (cmp != 0)  return cmp;
      cmp = cmp_zeroes_to_end(this->compile_id(),
                              that->compile_id());
      return cmp != 0 ? cmp : this->key()->cmp(that->key());
    }
    tdata = that->method();
  }
  return this->method()->cmp(tdata) | 1;
}

int MethodTrainingData::cmp(const TrainingData* tdata) const {
  if (this == tdata)  return 0;
  if (tdata->is_CompileTrainingData()) {
    return this->cmp(tdata->as_CompileTrainingData()->method()) | 1;
  }
  if (tdata->is_MethodTrainingData()) {
    const MethodTrainingData* that = tdata->as_MethodTrainingData();
    if (this->klass() == that->klass()) {
      int cmp = cmp_zeroes_to_end(this->last_compile_id(),
                                  that->last_compile_id());
      return cmp != 0 ? cmp : this->key()->cmp(that->key());
    }
    tdata = that->klass();
  }
  return this->klass()->cmp(tdata) | 1;
}

int KlassTrainingData::cmp(const TrainingData* tdata) const {
  if (this == tdata)  return 0;
  if (tdata->is_KlassTrainingData()) {
    const KlassTrainingData* that = tdata->as_KlassTrainingData();
    int cmp = cmp_zeroes_to_end(this->clinit_sequence_index_or_zero(),
                                that->clinit_sequence_index_or_zero());
    return cmp != 0 ? cmp : this->key()->cmp(that->key());
  }
  if (tdata->is_CompileTrainingData()) {
    tdata = tdata->as_CompileTrainingData()->method();
  }
  assert(tdata->is_MethodTrainingData(), "");
  return (0 - tdata->cmp(this)) | 1;
}

// State machine for dumping.  There are three phases for each node:
// prepare, identify, detail.  All nodes prepare before any of the
// other phases execute.  A node can recursively prepare another node
// to ensure it is in the output set.  A node outputs its identify the
// first time identify is called on it.  Later on it outputs its
// details.
class TrainingDataDumper {
  xmlStream* _out;
  GrowableArray<TrainingData*> _index;
  GrowableArray<TrainingData*> _nodes;

 public:
  TrainingDataDumper() {
    _out = nullptr;
  }

  void set_out(xmlStream* out) {
    assert(_out == nullptr && out != nullptr, "");
    _out = out;
    _out->head("training_data");
  }

  void close() {
    if (_out != nullptr) {
      _out->tail("training_data");
      _out->flush();
      _out = nullptr;
    }
  }

  ~TrainingDataDumper() { close(); }

  xmlStream* out() { return _out; }

  // Return -1 if not yet dumped, else index it was dumped under.
  // Second argument enables allocation of a new index if needed.
  // Yes, this is quadratic.  No, we don't care about that at the
  // end of a training run.  When deserializing, the corresponding
  // operation uses a similar temporary GrowableArray but is O(1).
  int id_of(TrainingData* tdata) {
    return _index.find(tdata);
  }

  TrainingData* node_at(int i) {
    assert(_out == nullptr, "");
    return _index.at(i);
  }

  int node_count() {
    assert(_out == nullptr, "");
    return _index.length();
  }

  GrowableArray<TrainingData*> &hand_off_node_list() {
    assert(_out == nullptr, "");
    _nodes.swap(&_index);
    _index.clear();  // for reassigning id numbers in the future
    return _nodes;
  }

  void prepare(TrainingData::TrainingDataSet* tds, TRAPS) {
    TrainingData::TrainingDataLocker l;
    int prev_len = -1, len = 0;
    while (prev_len != len) {
      assert(prev_len < len, "must not shrink the worklist");
      prev_len = len; len = node_count();
      tds->iterate_all([&](const TrainingData::Key* k, TrainingData* td) {
        if (!td->do_not_dump()) {
          prepare(td); // FIXME: may allocate in metaspace and hence throw an exception
        }
      });
    }
  }

  void prepare(TrainingData* tdata) {
    assert(_out == nullptr, "");
    identify_or_prepare(tdata);
  }

  // Make sure this guy get line printed with id='%d'.
  int identify(TrainingData* tdata) {
    assert(_out != nullptr, "");
    return identify_or_prepare(tdata);
  }

 private:
  int identify_or_prepare(TrainingData* tdata) {
    bool prepare_only = (_out == nullptr);
    if (tdata == nullptr)  return -1;
    int len = _index.length();
    int id = id_of(tdata);
    if (id >= 0) {  // already assigned
      return id;
    }
    id = _index.append(tdata);
    if (prepare_only) {
      // This can cause recursive calls to prepare,
      // which will add to the index list.
      tdata->dump(*this, TrainingData::DP_prepare);
      return 0;
    }
    // At this point we are doing real output, so commit to each ID.
    if (tdata->dump(*this, TrainingData::DP_identify)) {
      return id;
    }
    // this tdata refused to identify itself
    if (id == _index.length() - 1) {
      _index.remove_at(id);
    } else {
      _index.at_put(id, nullptr);
    }
    return -1;
  }
};

using ClassState = InstanceKlass::ClassState;
#define EACH_CLASS_STATE(FN) \
    FN(allocated, "A") \
    FN(loaded, "O") \
    FN(being_linked, "BL") \
    FN(linked, "L") \
    FN(being_initialized, "BI") \
    FN(fully_initialized, "I") \
    FN(initialization_error, "IE") \
    /**/
static const char* ClassState_to_name(ClassState state) {
  #define SWITCH_CASE(x, y) \
    case ClassState::x: return y;
  switch (state) { EACH_CLASS_STATE(SWITCH_CASE) }
  #undef SWITCH_CASE
  return "?";
}
#if 0
static int name_to_ClassState(const char* n) {
  #define NAME_CASE(x, y) \
    if (!strcmp(n, y))  return (int)ClassState::x;
  EACH_CLASS_STATE(NAME_CASE);
  #undef NAME_CASE
  return -1;
}
#endif
bool KlassTrainingData::dump(TrainingDataDumper& tdd, DumpPhase dp) {
  if (dp == DP_prepare) {
    // FIXME: Decide if we should set _do_not_dump on some records.
    ClassLoaderData* loader_data = nullptr;
    if (_holder != nullptr) {
      loader_data = _holder->class_loader_data();
    } else {
      loader_data = java_lang_ClassLoader::loader_data(SystemDictionary::java_system_loader()); // default CLD
    }
    _init_deps.prepare(loader_data);
    _comp_deps.prepare(loader_data);
    return true;
  }
  auto out = tdd.out();
  int kid = tdd.id_of(this);
  if (dp == DP_identify) {
    out->begin_elem("klass id='%d'", kid);
    out->name(this->name());
    Symbol* ln = this->loader_name();
    if (ln != nullptr)  out->name(ln, "loader_");
    ClassState state = ClassState::allocated;
    if (has_holder())  state = holder()->init_state();
    out->print(" state='%s'", ClassState_to_name(state));
    out->end_elem();
    return true;
  }
  assert(dp == DP_detail, "");
  TrainingDataLocker l;
  for (int i = 0, depc = init_dep_count(); i < depc; i++) {
    int did = tdd.identify(init_dep(i));
    out->elem("init_dep klass='%d' dep='%d'", kid, did);
  }
  return true;
}

bool MethodTrainingData::dump(TrainingDataDumper& tdd, DumpPhase dp) {
  auto kd = klass();
  if (dp == DP_prepare) {
    tdd.prepare(kd);
    // FIXME: Decide if we should set _do_not_dump on some records.
    if (has_holder()) {
      // FIXME: we might need to clone these two things
      _final_counters = holder()->method_counters();
      _final_profile  = holder()->method_data();
      assert(_final_profile == nullptr || _final_profile->method() == holder(), "");
    }
    if (_compile != nullptr) {
      // Just prepare the first one, or prepare them all?  This needs
      // an option, because it's useful to dump them all for analysis,
      // but it is likely only the first one (= most recent) matters.
      for (auto cd = _compile; cd != nullptr; cd = cd->next()) {
        tdd.prepare(cd);
      }
    }
    return true;
  }
  auto out = tdd.out();
  int mid = tdd.id_of(this);
  if (dp == DP_identify) {
    int kid = tdd.identify(kd);
    out->begin_elem("method id='%d' klass='%d'", mid, kid);
    out->name(this->name());
    out->signature(this->signature());
    out->print(" level_mask='%d'", _level_mask);
    if (last_compile_id() != 0) out->print(" compile_id='%d'", last_compile_id());
    // FIXME: dump counters, MDO, list of classes depended on
    out->end_elem();
    return true;
  }
  assert(dp == DP_detail, "");
  return true;
}

bool CompileTrainingData::dump(TrainingDataDumper& tdd, DumpPhase dp) {
  auto md = method();
  auto td = top_method();
  if (dp == DP_prepare) {
    tdd.prepare(md);
    tdd.prepare(td);
    _init_deps.prepare(_method->klass()->class_loader_data());
    _ci_records.prepare(_method->klass()->class_loader_data());
    return true;
  }
  auto out = tdd.out();
  int cid = tdd.id_of(this);
  int mid = tdd.identify(md);
  int tid = tdd.identify(td);
  if (dp == DP_identify) {
    out->begin_elem("compile id='%d' compile_id='%d' level='%d' method='%d'",
                    cid, compile_id(), level(), mid);
    if (is_inlined()) {
      out->print(" is_inlined='1' top_method='%d'", tid);
    }
    if (md->_compile == this) {
      out->print(" last='1'");
    }
    out->end_elem();
    return true;
  }
  assert(dp == DP_detail, "");
  TrainingDataLocker l;
  for (int i = 0, depc = init_dep_count(); i < depc; i++) {
    int did = tdd.identify(init_dep(i));
    out->elem("init_dep compile='%d' dep='%d'", cid, did);
  }
  return true;
}

static int qsort_compare_tdata(TrainingData** p1, TrainingData** p2) {
  return (*p1)->cmp(*p2);
}

void TrainingData::store_results() {
  if (!need_data() && !have_data())  return;

  ResourceMark rm;
  TrainingDataDumper tdd;

  // Collect all the training data and prepare to dump or archive.
  // The first dump phase is preparation, which can mark nodes as
  // do_not_dump, and/or can generate additional nodes.  The second
  // phase is identification, where every time a node ID is required,
  // we call identify; the first such call for each node runs its
  // identification dump.  The third phase is detail dumping, which
  // potentially dumps more besides than the one-line summary that
  // comes out of the identity phase.  The last two phases are
  // necessary because of the potential for circular references.
  // Cycles must inherently be broken by first defining nodes and then
  // defining edges between them, such as (in this case)
  // initialization dependencies.  The detail phase is where the
  // edges appear, after the identify phase which assigns id
  // numbers to nodes.
  int prev_len = -1, len = 0;
  while (prev_len != len) {
    assert(prev_len < len, "must not shrink the worklist");
    prev_len = len; len = tdd.node_count();
    // Since dump(DP_prepare) might have entered new items into the
    // global TD table, we need to enumerate again from scratch.
    {
      TrainingDataLocker l;
      training_data_set()->iterate_all([&](const Key* k, TrainingData* td) {
        if (td->do_not_dump())  return;
        tdd.prepare(td);
      });
    }
  }

  auto tda = tdd.hand_off_node_list();
  tda.sort(qsort_compare_tdata);
  // Data is ready to dump now.

  const char* file_name = TrainingFile;
  if (file_name == nullptr)  file_name = "hs_training_%p.log";
  if (strstr(file_name, "%p")) {
    const char* tmplt = file_name;
    size_t buf_len = strlen(tmplt) + 100;
    char* buf = NEW_RESOURCE_ARRAY(char, buf_len);
    if (buf != nullptr) {
      Arguments::copy_expand_pid(tmplt, strlen(tmplt), buf, buf_len);
      // (if copy_expand_pid fails, we will be OK with its partial output)
      file_name = buf;
    }
  }
  fileStream file(file_name);
  if (!file.is_open()) {
    warning("Training data failed: cannot open file %s", file_name);
    return;
  }
  xmlStream out(&file);
  tdd.set_out(&out);
  for (int i = 0; i < tda.length(); i++) {
    auto td = tda.at(i);
    if (td->do_not_dump())  continue;
    tdd.identify(td);
    td->dump(tdd, DP_detail);
  }
  tdd.close();
}

static bool str_starts(const char* str, const char* start) {
  return !strncmp(str, start, strlen(start));
}

static bool str_scan(const char* str, const char* fmt, ...)
  ATTRIBUTE_SCANF(2, 3);
static bool str_scan(const char* str, const char* fmt, ...) {
  bool res = false;
  va_list args;
  va_start(args, fmt);
  const char* pct = strchr(fmt, '%');
  assert(pct != nullptr, "");
  size_t pfxlen = pct - fmt;  // length of prefix before %
  const char* sp = str;
  while ((sp = strchr(sp, fmt[0])) != nullptr) {
    if (!strncmp(sp, fmt, pfxlen)) {
      sp += pfxlen;
      fmt += pfxlen;
      break;
    }
    ++sp;
  }
  if (sp != nullptr) {
    if (str_starts(fmt, "%p%n")) {
      const char* endq = fmt + strlen("%p%n");
      const char* ep = *endq ? strstr(sp, endq) : sp + strlen(sp);
      if (ep != nullptr) {
        *va_arg(args, const char**) = sp;
        *va_arg(args, int*) = (int)(ep - sp);
        res = true;
      }
    } else {
      res = (vsscanf(sp, fmt, args) > 0);
    }
  }
  va_end(args);
  return res;
}

void TrainingData::load_profiles() {
  if (!have_data())  return;
  const char* file_name = TrainingFile;
  if (file_name == nullptr)  file_name = "hs_training.log";
  fileStream file(file_name, "r");
  if (!file.is_open()) {
    warning("Training data not found: cannot open file %s", file_name);
    return;
  }
  // FIXME: add a line-oriented stream API for reading config files
  const size_t buflen = 4096;
  char buffer[buflen];
  GrowableArrayCHeap<TrainingData*, mtCompiler> id2td;
  #define ID2TD(id) (id >= 0 && id < id2td.length() ? id2td.at(id) : nullptr)
  #define ADD_ID2TD(id, tdata) {                                 \
      while (id2td.length() <= id)  id2td.append(nullptr);       \
      id2td.at_put(id, tdata);                                   \
    }
  char* line;
  while ((line = file.readln(buffer, buflen)) != nullptr) {
    if (line == nullptr)  break;
    typedef char* cstr;
    cstr name, sig, lname;
    int nlen, slen, lnlen;
    int kid, mid, cid, tid, did;
    if (line == nullptr)  break;
    if (str_starts(line, "<training_data>") ||
        str_starts(line, "</training_data>")) {
      continue;
    } else if (str_starts(line, "<klass ")) {
      // <klass id='10' name='Foo' loader_name='bar'/>
      if (!str_scan(line, " id='%d'", &kid) ||
          !str_scan(line, " name='%p%n'", &name, &nlen)) {
        break;
      }
      if (!str_scan(line, " loader_name='%p%n'", &lname, &lnlen)) {
        lname = nullptr;
      }
      name[nlen] = '\0';
      if (lname != nullptr) {
        lname[lnlen] = '\0';
        const char* qapos = "&apos;";  //FIXME: do proper unescaping
        if (str_starts(lname, qapos)) {
          lname += strlen(qapos);
          *--lname = '\'';
        }
        for (char* sp; (sp = strstr(lname, qapos)); ) {
          *sp++ = '\'';
          strcpy(sp, sp + strlen(qapos) - 1);
        }
      }
      auto tdata = KlassTrainingData::make(name, lname);
      ADD_ID2TD(kid, tdata);
    } else if (str_starts(line, "<method ")) {
      // <method id='14' klass='13' name='m' signature='()V' level='3'/>
      if (!str_scan(line, " id='%d'", &mid) ||
          !str_scan(line, " klass='%d'", &kid) ||
          !str_scan(line, " name='%p%n'", &name, &nlen) ||
          !str_scan(line, " signature='%p%n'", &sig, &slen)) {
        break;
      }
      auto kdata = ID2TD(kid);
      if (kdata == nullptr || !kdata->is_KlassTrainingData())  break;
      name[nlen] = '\0';
      if (!strcmp(name, "&lt;init&gt;")) {
        name = (char*) "<init>";  //FIXME: do proper unescaping
      }
      sig[slen] = '\0';
      auto tdata = MethodTrainingData::make(kdata->as_KlassTrainingData(),
                                            name, sig);
      ADD_ID2TD(mid, tdata);
    } else if (str_starts(line, "<compile ")) {
      // <compile id='42' compile_id='1283' level='3' method='1005'/>
      if (!str_scan(line, " id='%d'", &cid) ||
          !str_scan(line, " method='%d'", &mid)) {
        break;
      }
      if (!str_scan(line, " top_method='%d'", &tid))  tid = mid;
      int task = 0;
      str_scan(line, " compile_id='%d'", &task);
      int level = 0;
      str_scan(line, " level='%d'", &level);
      auto md = ID2TD(mid);
      auto td = ID2TD(tid);
      if (md == nullptr || td == nullptr)  break;
      auto tdata = CompileTrainingData::make(md->as_MethodTrainingData(),
                                             td->as_MethodTrainingData(),
                                             level, task);
      ADD_ID2TD(cid, tdata);
    } else if (str_starts(line, "<init_dep ")) {
      // <init_dep klass='1040' dep='14'/>
      // <init_dep compile='1041' dep='14'/>
      kid = cid = -1;
      if (!str_scan(line, " dep='%d'", &did) ||
          (!str_scan(line, " klass='%d'", &kid) &&
           !str_scan(line, " compile='%d'", &cid))) {
        break;
      }
      auto kd = ID2TD(kid);
      auto cd = ID2TD(cid);
      auto dd = ID2TD(did);
      if ((kd == nullptr && cd == nullptr) || dd == nullptr)  break;
      if (kd != nullptr) {
        TrainingDataLocker l;
        kd->as_KlassTrainingData()
          ->add_init_dep(dd->as_KlassTrainingData());
      } else {
        TrainingDataLocker l;
        cd->as_CompileTrainingData()
          ->add_init_dep(dd->as_KlassTrainingData());
      }
    } else {
      break;
    }
  }
  if (line != nullptr) {
    warning("unrecognized training line: %s", line);
  }
}

using FieldData = KlassTrainingData::FieldData;
int KlassTrainingData::_clinit_count;  //number <clinit> events in RecordTraining
GrowableArrayCHeap<FieldData, mtCompiler>* KlassTrainingData::_no_static_fields;

KlassTrainingData* KlassTrainingData::make(Symbol* name, Symbol* loader_name) {
  Key key(name, loader_name);
  TrainingData* td = have_data() ? lookup_archived_training_data(&key) : nullptr;
  KlassTrainingData* ktd = nullptr;
  if (td != nullptr) {
    ktd = td->as_KlassTrainingData();
    return ktd;
  }
  TrainingDataLocker l;
  td = training_data_set()->find(&key);
  if (td == nullptr) {
    ktd = KlassTrainingData::allocate(name, loader_name);
    td = training_data_set()->install(ktd);
    assert(ktd == td, "");
  } else {
    ktd = td->as_KlassTrainingData();
  }
  assert(ktd != nullptr, "");
  return ktd;
}

KlassTrainingData* KlassTrainingData::make(const char* name, const char* loader_name) {
  TempNewSymbol n = SymbolTable::new_symbol(name);
  if (loader_name == nullptr) {
    return make(n, nullptr);
  } else {
    TempNewSymbol l = SymbolTable::new_symbol(loader_name);
    return make(n, l);
  }
}

KlassTrainingData* KlassTrainingData::make(InstanceKlass* holder, bool null_if_not_found) {
  Key key(holder);
  TrainingData* td = have_data() ? lookup_archived_training_data(&key) : nullptr;
  KlassTrainingData* ktd = nullptr;
  if (td != nullptr) {
    ktd = td->as_KlassTrainingData();
    ktd->refresh_from(holder);
    holder->init_training_data(ktd);
    guarantee(ktd->has_holder() && ktd->holder() == holder, "");
    return ktd;
  }
  TrainingDataLocker l;
  td = training_data_set()->find(&key);
  if (td == nullptr) {
    if (null_if_not_found) {
      return nullptr;
    }
    ktd = KlassTrainingData::allocate(holder);
    td = training_data_set()->install(ktd);
    assert(ktd == td, "");
  } else {
    ktd = td->as_KlassTrainingData();
  }
  assert(ktd != nullptr, "");
  ktd->refresh_from(holder);
  bool ok = holder->init_training_data(ktd);
  assert(ok, "CAS under mutex cannot fail");
  guarantee(ktd->has_holder() && ktd->holder() == holder, "");
  return ktd;
}

void KlassTrainingData::print_on(outputStream* st, bool name_only) const {
  name()->print_symbol_on(st);
  if (has_holder()) {
    switch (holder()->init_state()) {
      case InstanceKlass::allocated:            st->print("[A]"); break;
      case InstanceKlass::loaded:               st->print("[D]"); break;
      case InstanceKlass::being_linked:         st->print("[l]"); break;
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
  if (_do_not_dump)  st->print("[DND]");
  if (name_only)  return;
  if (_clinit_sequence_index)  st->print("IC%d", _clinit_sequence_index);
  for (int i = 0, len = _init_deps.length(); i < len; i++) {
    st->print(" dep:");
    _init_deps.at(i)->print_on(st, true);
  }
}

void KlassTrainingData::refresh_from(const InstanceKlass* klass) {
  if (!has_holder()) {
    init_holder(klass);
  }
  if (holder() == klass) {
    if (klass->is_initialized() && !_clinit_is_done) {
      _clinit_is_done = true;
    }
  }
}

void KlassTrainingData::init_holder(const InstanceKlass* klass) {
  if (holder() == klass) {
    return;   // no change to make
  }

  jobject hmj = _holder_mirror;
  if (hmj != nullptr) {   // clear out previous handle, if any
    _holder_mirror = nullptr;
    assert(JNIHandles::is_global_handle(hmj), "");
    JNIHandles::destroy_global(hmj);
  }

  // reset state derived from any previous klass
  _static_fields = nullptr;
  _fieldinit_count = 0;
  _clinit_is_done = false;
  _clinit_sequence_index = 0;

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

  if (klass == nullptr)  return;

  bool is_init = klass->is_initialized();
  _clinit_is_done = is_init;
  if (is_init) {
    // if <clinit> is in the past, do not bother tracking fields
    _static_fields = no_static_fields();
  } else {
    setup_static_fields(klass);
  }
}

void KlassTrainingData::record_initialization_start() {
  if (!TrainingData::need_data()) {
    return;
  }
  //assert(_clinit_sequence_index == 0, "set this under mutex");
  _clinit_sequence_index = next_clinit_count();
  log_initialization(true);
}

bool KlassTrainingData::add_initialization_touch(Klass* requester) {
  _has_initialization_touch = true;
  if (requester == nullptr || !requester->is_instance_klass())
    return false;
  auto rtd = KlassTrainingData::make(InstanceKlass::cast(requester));
  if (rtd != nullptr && rtd != this) {
    // The requester is asking that I be initialized; this means
    // that I should be added to his _init_deps list.up
    TrainingDataLocker l;
    rtd->add_init_dep(this);
  }
  return true;
}

void KlassTrainingData::record_initialization_end() {
  _clinit_is_done = true;  // we know this now
  log_initialization(false);
}

void KlassTrainingData::notice_fully_initialized() {
  TrainingDataLocker l; // Not a real lock if we don't collect the data,
                        // that's why we need the atomic decrement below.
  for (int i = 0; i < comp_dep_count(); i++) {
    comp_dep(i)->dec_init_deps_left(this);
  }
}

GrowableArrayCHeap<FieldData, mtCompiler>*
KlassTrainingData::no_static_fields() {
  GrowableArrayCHeap<FieldData, mtCompiler>* nsf = _no_static_fields;
  if (nsf != nullptr) {
    return nsf;
  }
  nsf = new GrowableArrayCHeap<FieldData, mtCompiler>(0);
  if (nsf != nullptr && !Atomic::replace_if_null(&_no_static_fields, nsf)) {
    delete nsf;
    nsf = _no_static_fields;
  }
  return nsf;
}

// Note:  Racers may do this more than once.
// So, make no externally visible side effects.
void KlassTrainingData::setup_static_fields(const InstanceKlass* holder) {
  auto fda = Atomic::load_acquire(&_static_fields);
  if (fda != nullptr)  return;
  fda = new GrowableArrayCHeap<FieldData, mtCompiler>();
  int num_statics = 0;
  for (JavaFieldStream fs(holder); !fs.done(); fs.next()) {
    if (!fs.access_flags().is_static())
      continue;  // only tracking static fields
    if (fs.access_flags().is_final() && fs.initval_index() != 0)
      continue;  // skip constants initialized directly by the JVM
    fda->append(FieldData());
    // set up tracking data for the field
    FieldData& data = fda->adr_at(num_statics)[0];
    data.init_from(fs.field_descriptor());
    if (!field_state_is_clean(&data)) {
      data._fieldinit_sequence_index = ++_fieldinit_count;
    }
    ++num_statics;
  }
  if (num_statics == 0) {
    delete fda;
    fda = no_static_fields();
  }

  // After the array is set up, store it; arbitrate among racers.
  if (!Atomic::replace_if_null(&_static_fields, fda)) {
    if (fda != no_static_fields()) {
      delete fda;
    }
  }
}

// Combined linear search pass to find the name, and also
// note missed field updates.  It could be a fancy binary search,
// except we want to do a linear walk anyway to look for updates.
// It is possible we missed an initial `putstatic`, or maybe it never happened.
// Work around the leaky detection by periodic checks for evidence of inits.
KlassTrainingData::FieldData*
KlassTrainingData::check_field_states_and_find_field(Symbol* name) {
  int len;
  if (_static_fields == nullptr || (len = _static_fields->length()) == 0)
    return nullptr;
  FieldData* result = nullptr;
  for (int i = 0; i < len; i++) {
    FieldData* fdata = _static_fields->adr_at(i);
    if (fdata->_name == name)  result = fdata;
    if (fdata->_fieldinit_sequence_index == 0 &&
        !field_state_is_clean(fdata)) {
      // Oops, a missed update.  Track it after the fact.
      assert(!all_field_states_done(), "");
      record_static_field_init(fdata, "unknown");
    }
  }
  return result;
}

bool KlassTrainingData::record_static_field_init(FieldData* fdata,
                                            const char* reason) {
  int& seq = fdata->_fieldinit_sequence_index;
  int PENDING = -1;
  int found = Atomic::cmpxchg(&seq, 0, PENDING, memory_order_conservative);
  if (found != 0)  return false;  // racer beat us to it
  Atomic::store(&seq, next_fieldinit_count());
  {
    ttyLocker ttyl;
    xtty->begin_elem("initialize_static_field");
    xtty->klass(holder());
    print_iclock_attr(holder(), xtty, seq);
    xtty->name(fdata->_name);
    xtty->print(" reason='%s'", reason);
    xtty->thread();
    xtty->stamp();
    xtty->end_elem();
  }
  return true;
}

void KlassTrainingData::print_klass_attrs(xmlStream* xtty,
                                     Klass* klass, const char* prefix) {
  if (!klass)  return;
  xtty->klass(klass, prefix);
  if (!klass->is_instance_klass())  return;

  // print a little more information in case it is useful
  InstanceKlass* ik = InstanceKlass::cast(klass);
  int ikf = ik->access_flags().as_int() & (u2)-1;
  ikf &= ~JVM_ACC_SUPER;  // this is strictly noise
  char ikf2[20];
  char* ikf2p = &ikf2[0];
  if (ik->is_sealed()) { *ikf2p++ = 's'; }
  *ikf2p = 0;
  // no need for is_hidden since the name makes it obvious
  xtty->print(" %skflags='%d%s'", prefix, ikf, &ikf2[0]);
  print_iclock_attr(ik, xtty, -1, prefix);
}

void KlassTrainingData::print_iclock_attr(InstanceKlass* klass,
                                          xmlStream* xtty,
                                          int fieldinit_index,
                                          const char* prefix) {
  KlassTrainingData* tdata = klass->training_data_or_null();
  const int all_fields_done = 9999;
  int clinit_index = 0;
  if (tdata != nullptr) {
    if (fieldinit_index < 0) {
      if (tdata->_clinit_is_done)
        fieldinit_index = all_fields_done;
      else {
        fieldinit_index = tdata->_fieldinit_count;
        if (fieldinit_index > 900) {
          // ... 42.899, 42.900, 42.900901, 42.900902, ... 42.930000
          fieldinit_index += 900000;
        }
      }
    }
    clinit_index = tdata->clinit_sequence_index_or_zero();
  }
  const char* istate = "";
  if (klass->is_initialized()) {
    if (tdata != nullptr)
      tdata->_clinit_is_done = true;  // notice this, just in case
    fieldinit_index = all_fields_done;
  } else if (klass->is_not_initialized()) {
    if (tdata == nullptr || clinit_index != 0)
      istate = "U";
  } else if (klass->is_being_initialized()) {
    // check for intermediate states:  R = recursive, O = other thread
    istate = klass->is_init_thread(JavaThread::current()) ? "R" : "O";
  } else {
    istate = "E";  // initialization error, which is very rare
  }
  if (fieldinit_index < 0)
    fieldinit_index = 0;
  if (fieldinit_index < 100000)
    xtty->print(" %siclock='%d.%03d%s'", prefix,
                clinit_index, fieldinit_index, istate);
  else
    // avoid clock wrap for ridiculous field counts
    xtty->print(" %siclock='%d.%06d%s'", prefix,
                clinit_index, fieldinit_index, istate);
}


// Decide if the field state looks clean.
// Without further effort we cannot tell if someone has just stored
// the default value, so this query can return false positives,
// claims that a field is "clean" even if it has been subject to updates.
bool KlassTrainingData::field_state_is_clean(FieldData* fdata) {
  oop mirror = holder()->java_mirror();
  int fo = fdata->_offset;
  switch (fdata->_type) {
  case T_OBJECT:
  case T_ARRAY:
    return (mirror->obj_field(fo) == nullptr);
  case T_BYTE:
    return (mirror->byte_field(fo) == 0);
  case T_BOOLEAN:
    return (mirror->bool_field(fo) == 0);
  case T_CHAR:
    return (mirror->char_field(fo) == 0);
  case T_SHORT:
    return (mirror->short_field(fo) == 0);
  case T_INT:
  case T_FLOAT:
    // use int field format to test for zero because of -0.0f
    return (mirror->int_field(fo) == 0);
  case T_LONG:
  case T_DOUBLE:
    // use long field format to test for zero because of -0.0d
    return (mirror->long_field(fo) == 0);
  default:
    break;
  }
  return true;
}

// called externally
bool KlassTrainingData::record_static_field_init(fieldDescriptor* fd,
                                            const char* reason) {
  if (!_static_fields)  return false;  // should not happen unless OOM
  if (fd->field_holder() != holder())  return false;  // should not happen...
  FieldData* fdp = check_field_states_and_find_field(fd->name());
  if (fdp == nullptr)  return false;
  return record_static_field_init(fdp, reason);
}

void KlassTrainingData::record_touch_common(xmlStream* xtty,
                                            const char* reason,
                                            CompileTask* jit_task,
                                            Klass* init_klass,
                                            Klass* requesting_klass,
                                            Symbol* name,
                                            Symbol* sig,
                                            const char* context) {
  if (xtty == nullptr)  return;  // no detailed logging
  xtty->begin_elem("initialization_touch reason='%s'", reason);
  if (context)  xtty->print(" context='%s'", context);
  print_klass_attrs(xtty, holder());
  if (name)  xtty->name(name);
  if (sig)   xtty->signature(sig);
  // report up to two requesting parties
  for (int pass = 0; pass <= 1; pass++) {
    Klass* k = !pass ? init_klass : requesting_klass;
    if (!k)  continue;
    if (pass && k == init_klass)  break;
    const char* prefix = !pass ? "init_" : "requesting_";
    if (k == holder()) {
      xtty->print(" %sklass='//self'", prefix); continue;
    }
    print_klass_attrs(xtty, k, prefix);
  }
  if (!init_klass && !requesting_klass) {
    xtty->print_raw(" requesting_klass=''");
  }
  if (jit_task != nullptr) {
    xtty->print(" compile_id='%d'", jit_task->compile_id());
  }
  xtty->thread();
  xtty->stamp();
  xtty->end_elem();
}

void KlassTrainingData::record_initialization_touch(const char* reason,
                                                    Symbol* name,
                                                    Symbol* sig,
                                                    Klass* requesting_klass,
                                                    const char* context,
                                                    TRAPS) {
  Klass* init_klass = THREAD->class_being_initialized();
  if (!strcmp(reason, "super")) {
    // Extra-special touch during class initialization per JVMS Step 7.
    // We track this touch as if from RK.<clinit>, even if RK doesn't have one.
    init_klass = requesting_klass;
    requesting_klass = nullptr;  // ignore any real <clinit> on stack
  }
  add_initialization_touch(init_klass ? init_klass : requesting_klass);
  ttyLocker ttyl;
  record_touch_common(xtty, reason, /*jit_env*/ nullptr,
                      init_klass, requesting_klass,
                      name, sig, context);
}

void KlassTrainingData::log_initialization(bool is_start) {
  if (xtty == nullptr)  return;
  ttyLocker ttyl;
  // Note:  These XML records might not nest properly.
  // So we use <init/> and <init_done/>, not <init> and </init>.
  if (is_start) {
    xtty->begin_elem("initialization");
    print_klass_attrs(xtty, holder());
    xtty->thread();
    xtty->stamp();
    xtty->end_elem();
  } else {
    xtty->begin_elem("initialization_done");
    print_klass_attrs(xtty, holder());
    xtty->thread();
    xtty->stamp();
    xtty->end_elem();
  }
}

// CDS support

class TrainingData::Transfer : StackObj {
public:
  void do_value(TrainingData* record) {
    _dumptime_training_data_dictionary->append(record);
  }
};


void TrainingData::init_dumptime_table(TRAPS) {
  if (!need_data()) {
    return;
  }
  if (CDSConfig::is_dumping_final_static_archive()) {
    _dumptime_training_data_dictionary = new GrowableArrayCHeap<DumpTimeTrainingDataInfo, mtClassShared>();
    Transfer transfer;
    _archived_training_data_dictionary.iterate(&transfer);
  } else {
    ResourceMark rm;
    TrainingDataDumper tdd;
    tdd.prepare(training_data_set(), CHECK);
    GrowableArray<TrainingData*>& tda = tdd.hand_off_node_list();
    tda.sort(qsort_compare_tdata); // FIXME: needed?
    int num_of_entries = tda.length();
    _dumptime_training_data_dictionary = new GrowableArrayCHeap<DumpTimeTrainingDataInfo, mtClassShared>(num_of_entries);
    for (int i = 0; i < num_of_entries; i++) {
      TrainingData* td = tda.at(i);
      if (td->is_CompileTrainingData()) {
        continue; // skip CTDs; discoverable through corresponding MTD
      } else {
        // TODO: filter TD
        // if (SystemDictionaryShared::check_for_exclusion(), nullptr);
        _dumptime_training_data_dictionary->append(td);
      }
    }
  }

  prepare_recompilation_schedule(CHECK);
}

void TrainingData::prepare_recompilation_schedule(TRAPS) {
  if (!need_data()) {
    return;
  }
  auto nmethods = MethodProfiler::sampled_nmethods();
  GrowableArray<MethodTrainingData*> dyn_recompilation_schedule;
  for (auto it = nmethods->begin(); it != nmethods->end(); ++it) {
    nmethod* nm = *it;
    if (RecordOnlyTopCompilations && nm->method_profiling_count() == 0) {
      break;
    }
    if (nm->method() != nullptr) {
      MethodTrainingData* mtd = nm->method()->training_data_or_null();
      if (mtd != nullptr) {
        dyn_recompilation_schedule.append(mtd);
      }
    }
  }
  delete nmethods;
  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  _recompilation_schedule_for_dumping = MetadataFactory::new_array<MethodTrainingData*>(loader_data, dyn_recompilation_schedule.length(), THREAD);
  int i = 0;
  for (auto it = dyn_recompilation_schedule.begin(); it != dyn_recompilation_schedule.end(); ++it) {
    _recompilation_schedule_for_dumping->at_put(i++, *it);
  }
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
  it->push(&_recompilation_schedule_for_dumping);
}

void TrainingData::dump_training_data() {
  if (!need_data()) {
    return;
  }
  write_training_data_dictionary(&_archived_training_data_dictionary_for_dumping);
}

void TrainingData::cleanup_training_data() {
  if (_dumptime_training_data_dictionary != nullptr) {
    for (int i = 0; i < _dumptime_training_data_dictionary->length(); i++) {
      TrainingData* td = _dumptime_training_data_dictionary->at(i).training_data();
      td->cleanup();
    }
  }
  _recompilation_status = nullptr;
}

void KlassTrainingData::cleanup() {
  if (holder() != nullptr) {
    bool is_excluded = !holder()->is_loaded() || SystemDictionaryShared::check_for_exclusion(holder(), nullptr);
    if (is_excluded) {
      ResourceMark rm;
      log_debug(cds)("Cleanup KTD %s", name()->as_klass_external_name());
      _holder = nullptr; // reset
    }
  }
  for (int i = 0; i < _comp_deps.length(); i++) {
    _comp_deps.at(i)->cleanup();
  }
}

void MethodTrainingData::cleanup() {
  if (has_holder()) {
    if (SystemDictionaryShared::check_for_exclusion(holder()->method_holder(), nullptr)) {
      log_debug(cds)("Cleanup MTD %s::%s", name()->as_klass_external_name(), signature()->as_utf8());
      if (_final_profile != nullptr && _final_profile->method() != _holder) {
        // FIXME: MDO still points at the stale method; either completely drop the MDO or zero out the link
        log_warning(cds)("Stale MDO for  %s::%s", name()->as_klass_external_name(), signature()->as_utf8());
      }
      _holder = nullptr;
    }
  }
  for (CompileTrainingData* ctd = _compile; ctd != nullptr; ctd = ctd->next()) {
    if (ctd->method() != this) {
      ctd->method()->cleanup();
    }
    if (ctd->is_inlined() && ctd->top_method() != this) {
      ctd->top_method()->cleanup();
    }
  }
}

void CompileTrainingData::cleanup() {
  method()->cleanup();
  if (is_inlined()) {
    top_method()->cleanup();
  }
}

void TrainingData::serialize_training_data(SerializeClosure* soc) {
  if (soc->writing()) {
    _archived_training_data_dictionary_for_dumping.serialize_header(soc);
    soc->do_ptr(&_recompilation_schedule_for_dumping);
  } else {
    _archived_training_data_dictionary.serialize_header(soc);
    soc->do_ptr(&_recompilation_schedule);
  }
}

void TrainingData::adjust_training_data_dictionary() {
  if (!need_data()) {
    return;
  }
  assert(_dumptime_training_data_dictionary != nullptr, "");
  for (int i = 0; i < _dumptime_training_data_dictionary->length(); i++) {
    TrainingData* td = _dumptime_training_data_dictionary->at(i).training_data();
    td = ArchiveBuilder::current()->get_buffered_addr(td);
    assert(MetaspaceShared::is_in_shared_metaspace(td) || ArchiveBuilder::current()->is_in_buffer_space(td), "");
    td->remove_unshareable_info();
  }
}

void TrainingData::print_archived_training_data_on(outputStream* st) {
  st->print_cr("Archived TrainingData Dictionary");
  TrainingDataPrinter tdp(st);
  TrainingDataLocker::initialize();
  _archived_training_data_dictionary.iterate(&tdp);
  if (_recompilation_schedule != nullptr && _recompilation_schedule->length() > 0) {
    st->print_cr("Archived TrainingData Recompilation Schedule");
    for (int i = 0; i < _recompilation_schedule->length(); i++) {
      st->print("%4d: ", i);
      MethodTrainingData* mtd = _recompilation_schedule->at(i);
      if (mtd != nullptr) {
        mtd->print_on(st);
      } else {
        st->print("nullptr");
      }
      st->cr();
    }
  }
}

void TrainingData::Key::metaspace_pointers_do(MetaspaceClosure *iter) {
  iter->push(&_name1);
  iter->push(&_name2);
  iter->push((TrainingData**)&_holder);
}

void TrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  _key.metaspace_pointers_do(iter);
}

bool TrainingData::Key::can_compute_cds_hash(const Key* const& k) {
  return (k->name1() == nullptr || MetaspaceObj::is_shared(k->name1())) &&
         (k->name2() == nullptr || MetaspaceObj::is_shared(k->name2())) &&
         (k->holder() == nullptr || MetaspaceObj::is_shared(k->holder()));
}

uint TrainingData::Key::cds_hash(const Key* const& k) {
  uint hash = 0;
  if (k->holder() != nullptr) {
    hash += SystemDictionaryShared::hash_for_shared_dictionary((address)k->holder());
  }
  if (k->name1() != nullptr) {
    hash += SystemDictionaryShared::hash_for_shared_dictionary((address)k->name1());
  }
  if (k->name2() != nullptr) {
    hash += SystemDictionaryShared::hash_for_shared_dictionary((address)k->name2());
  }
  return hash;
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
      // FIXME: decide what to do with symbolic TD
      LogStreamHandle(Info,training) log;
      if (log.is_enabled()) {
        ResourceMark rm;
        log.print_cr("Ignored symbolic TrainingData:");
        log.print_cr("  Key: %s %s",
                     (k->name1() != nullptr ? k->name1()->as_C_string() : "(null)"),
                     (k->name2() != nullptr ? k->name2()->as_C_string() : "(null)"));
        td->print_on(&log);
        log.cr();
      }
    }
  }
  return nullptr;
}
#endif

KlassTrainingData* TrainingData::lookup_for(InstanceKlass* ik) {
  if (TrainingData::have_data() && ik != nullptr && ik->is_loaded()) {
    TrainingData::Key key(ik);
    TrainingData* td = TrainingData::lookup_archived_training_data(&key);
    if (td != nullptr && td->is_KlassTrainingData()) {
      return td->as_KlassTrainingData();
    }
  }
  return nullptr;
}

MethodTrainingData* TrainingData::lookup_for(Method* m) {
  if (TrainingData::have_data() && m != nullptr) {
    KlassTrainingData* holder_ktd = TrainingData::lookup_for(m->method_holder());
    if (holder_ktd != nullptr) {
      TrainingData::Key key(m->name(), m->signature(), holder_ktd);
      TrainingData* td = TrainingData::lookup_archived_training_data(&key);
      if (td != nullptr && td->is_MethodTrainingData()) {
        return td->as_MethodTrainingData();
      }
    }
  }
  return nullptr;
}

template <typename T>
void TrainingData::DepList<T>::metaspace_pointers_do(MetaspaceClosure* iter) {
  iter->push(&_deps);
}

void KlassTrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  log_trace(cds)("Iter(KlassTrainingData): %p", this);
  TrainingData::metaspace_pointers_do(iter);
  _init_deps.metaspace_pointers_do(iter);
  _comp_deps.metaspace_pointers_do(iter);
  iter->push(&_holder);
}

void MethodTrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  log_trace(cds)("Iter(MethodTrainingData): %p", this);
  TrainingData::metaspace_pointers_do(iter);
  iter->push(&_klass);
  iter->push((Method**)&_holder);
  iter->push(&_compile);
  for (int i = 0; i < CompLevel_count; i++) {
    iter->push(&_last_toplevel_compiles[i]);
  }
  iter->push(&_final_profile);
  iter->push(&_final_counters);
}

void CompileTrainingData::metaspace_pointers_do(MetaspaceClosure* iter) {
  log_trace(cds)("Iter(CompileTrainingData): %p", this);
  TrainingData::metaspace_pointers_do(iter);
  _init_deps.metaspace_pointers_do(iter);
  _ci_records.metaspace_pointers_do(iter);
  iter->push(&_method);
  iter->push(&_top_method);
  iter->push(&_next);
}

template <typename T>
void TrainingData::DepList<T>::prepare(ClassLoaderData* loader_data) {
  if (_deps == nullptr && _deps_dyn != nullptr) {
    JavaThread* THREAD = JavaThread::current();
    int len = _deps_dyn->length();
    _deps = MetadataFactory::new_array<T>(loader_data, len, THREAD);
    for (int i = 0; i < len; i++) {
      _deps->at_put(i, _deps_dyn->at(i)); // copy
    }
  }
}

KlassTrainingData* KlassTrainingData::allocate(InstanceKlass* holder) {
  assert(need_data() || have_data(), "");
  JavaThread* THREAD = JavaThread::current();

  size_t size = align_metadata_size(align_up(sizeof(KlassTrainingData), BytesPerWord) / BytesPerWord);

  ClassLoaderData* loader_data = holder->class_loader_data();
  return new (loader_data, size, MetaspaceObj::KlassTrainingDataType, THREAD)
      KlassTrainingData(holder);
}

KlassTrainingData* KlassTrainingData::allocate(Symbol* name, Symbol* loader_name) {
  assert(need_data() || have_data(), "");
  JavaThread* THREAD = JavaThread::current();

  size_t size = align_metadata_size(align_up(sizeof(KlassTrainingData), BytesPerWord) / BytesPerWord);

  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  return new (loader_data, size, MetaspaceObj::KlassTrainingDataType, THREAD)
      KlassTrainingData(name, loader_name);
}

MethodTrainingData* MethodTrainingData::allocate(KlassTrainingData* ktd, Symbol* name, Symbol* signature) {
  assert(need_data() || have_data(), "");
  JavaThread* THREAD = JavaThread::current();
//  assert(!THREAD->owns_locks(), "Should not own any locks"); // FIXME

  size_t size = align_metadata_size(align_up(sizeof(MethodTrainingData), BytesPerWord) / BytesPerWord);

  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  return new (loader_data, size, MetaspaceObj::KlassTrainingDataType, THREAD)
      MethodTrainingData(ktd, name, signature);
}

MethodTrainingData* MethodTrainingData::allocate(KlassTrainingData* ktd, Method* m) {
  assert(need_data() || have_data(), "");
  JavaThread* THREAD = JavaThread::current();

  size_t size = align_metadata_size(align_up(sizeof(MethodTrainingData), BytesPerWord) / BytesPerWord);

  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  return new (loader_data, size, MetaspaceObj::KlassTrainingDataType, THREAD)
      MethodTrainingData(ktd, m->name(), m->signature());
}

CompileTrainingData* CompileTrainingData::allocate(MethodTrainingData* this_method,
                                                   MethodTrainingData* top_method,
                                                   int level,
                                                   int compile_id) {
  assert(need_data() || have_data(), "");
  JavaThread* THREAD = JavaThread::current();
  size_t size = align_metadata_size(align_up(sizeof(CompileTrainingData), BytesPerWord) / BytesPerWord);

  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  return new (loader_data, size, MetaspaceObj::KlassTrainingDataType, THREAD)
      CompileTrainingData(this_method, top_method, level, compile_id);
}

static const char* tag(void* p) {
  if (p == nullptr) {
    return "   ";
  } else if (MetaspaceShared::is_shared_dynamic(p)) {
    return "<D>";
  } else if (MetaspaceShared::is_in_shared_metaspace(p)) {
    return "<S>";
  } else {
    return "???";
  }
}

void TrainingDataPrinter::do_value(const RunTimeClassInfo* record) {
  ResourceMark rm;
  KlassTrainingData* ktd = record->_klass->training_data_or_null();
  if (ktd != nullptr) {
    _st->print("%4d: KTD %s%p for %s %s", _index++, tag(ktd), ktd, record->_klass->external_name(),
               SystemDictionaryShared::class_loader_name_for_shared(record->_klass));
    ktd->print_on(_st);
    _st->cr();
    ktd->iterate_all_comp_deps([&](CompileTrainingData* ctd) {
      ResourceMark rm;
      _st->print_raw("    ");
      ctd->print_on(_st);
    });
  }
}

void TrainingDataPrinter::do_value(const RunTimeMethodDataInfo* record) {
  ResourceMark rm;
  MethodCounters* mc = record->method_counters();
  if (mc != nullptr) {
    MethodTrainingData* mtd = mc->method_training_data();
    if (mtd != nullptr) {
      _st->print("%4d: MTD %s%p ", _index++, tag(mtd), mtd);
      mtd->print_on(_st);
      _st->cr();

      int i = 0;
      for (CompileTrainingData* ctd = mtd->compile(); ctd != nullptr; ctd = ctd->next()) {
        _st->print("  CTD[%d]: ", i++);
        ctd->print_on(_st);
        _st->cr();
      }
    }
  }
}

void TrainingDataPrinter::do_value(TrainingData* td) {
#ifdef ASSERT
  TrainingData::Key key(td->key()->name1(), td->key()->name2(), td->key()->holder());
  assert(td == TrainingData::archived_training_data_dictionary()->lookup(td->key(), TrainingData::Key::cds_hash(td->key()), -1), "");
  assert(td == TrainingData::archived_training_data_dictionary()->lookup(&key, TrainingData::Key::cds_hash(&key), -1), "");
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
  _static_fields = nullptr;
  _no_static_fields = nullptr;
  _holder_mirror = nullptr;
  _init_deps.remove_unshareable_info();
  _comp_deps.remove_unshareable_info();
}

void KlassTrainingData::restore_unshareable_info(TRAPS) {
  TrainingData::restore_unshareable_info(CHECK);
  _init_deps.restore_unshareable_info(CHECK);
  _comp_deps.restore_unshareable_info(CHECK);
}

void MethodTrainingData::remove_unshareable_info() {
  TrainingData::remove_unshareable_info();
  for (CompileTrainingData* ctd = _compile; ctd != nullptr; ctd = ctd->next()) {
    ctd->remove_unshareable_info();
  }
  if (_final_counters != nullptr) {
    _final_counters->remove_unshareable_info();
  }
  if (_final_profile != nullptr) {
    _final_profile->remove_unshareable_info();
  }
}

void MethodTrainingData::restore_unshareable_info(TRAPS) {
  TrainingData::restore_unshareable_info(CHECK);
  for (CompileTrainingData* ctd = _compile; ctd != nullptr; ctd = ctd->next()) {
    ctd->restore_unshareable_info(CHECK);
  }
  if (_final_counters != nullptr ) {
    _final_counters->restore_unshareable_info(CHECK);
  }
  if (_final_profile != nullptr) {
    _final_profile->restore_unshareable_info(CHECK);
  }
  initialize_deps_tracking();
}

void CompileTrainingData::remove_unshareable_info() {
  TrainingData::remove_unshareable_info();
  _init_deps.remove_unshareable_info();
  _ci_records.remove_unshareable_info();
}

void CompileTrainingData::restore_unshareable_info(TRAPS) {
  TrainingData::restore_unshareable_info(CHECK);
  _init_deps.restore_unshareable_info(CHECK);
  _ci_records.restore_unshareable_info(CHECK);
}
#endif // INCLUDE_CDS
