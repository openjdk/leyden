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

#ifndef SHARE_OOPS_TRAININGDATA_HPP
#define SHARE_OOPS_TRAININGDATA_HPP

#include "cds/archiveUtils.hpp"
#include "classfile/compactHashtable.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "compiler/compiler_globals.hpp"
#include "memory/allocation.hpp"
#include "memory/metaspaceClosure.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/symbolHandle.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/handles.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/count_leading_zeros.hpp"
#include "utilities/resizeableResourceHash.hpp"

class ciEnv;
class ciBaseObject;
class CompileTask;
class xmlStream;
class CompileTrainingData;
class KlassTrainingData;
class MethodTrainingData;
class TrainingDataDumper;
class TrainingDataSetLocker;
class DumpTimeTrainingDataInfo;
class TrainingDataDictionary;
class RunTimeClassInfo;
class RunTimeMethodDataInfo;

// Options are a list of comma-separated booleans (for now)
// For example: TrainingOptions=xml

class TrainingData : public Metadata {
  friend KlassTrainingData;
  friend MethodTrainingData;
  friend CompileTrainingData;
 public:
  class Key {
    Symbol* _name1;   // Klass::name or Method::name
    Symbol* _name2;   // class_loader_name_and_id or signature
    const TrainingData* const _holder; // TD for containing klass or method

    // These guys can get to my constructors:
    friend TrainingData;
    friend KlassTrainingData;
    friend MethodTrainingData;
    friend CompileTrainingData;

    // The empty key
    Key() : Key(nullptr, nullptr) { }
    bool is_empty() const {
      return _name1 == nullptr && _name2 == nullptr && _holder == nullptr;
    }

  public:
    Key(Symbol* name1, Symbol* name2,
        const TrainingData* holder = nullptr)
      : _name1(name1), _name2(name2), _holder(holder)
      // Since we are using SymbolHandles here, the reference counts
      // are incremented here, in this constructor.  We assume that
      // the symbols are already kept alive by some other means, but
      // after this point the Key object keeps them alive as well.
    { }
    Key(const KlassTrainingData* klass, Symbol* method_name, Symbol* signature);
    Key(const InstanceKlass* klass);
    Key(const Method* method);

    static unsigned cds_hash(const Key* const& k);
    static bool can_compute_cds_hash(const Key* const& k);
    static unsigned hash(const Key* const& k) {
      // A symmetric hash code is usually a bad idea, except in cases
      // like this where it is very unlikely that any one string might
      // appear in two positions, and even less likely that two
      // strings might trade places in two otherwise equal keys.
      return (Symbol::identity_hash(k->name1()) +
              Symbol::identity_hash(k->name2()) +
              (k->holder() == nullptr ? 0 : hash(k->holder()->key())));
    }
    static bool equals(const Key* const& k1, const Key* const& k2) {
      // We assume that all Symbols come for SymbolTable and therefore are unique.
      // Hence pointer comparison is enough to prove equality.
      return (k1->name1()   == k2->name1() &&
              k1->name2()   == k2->name2() &&
              k1->holder()  == k2->holder());
    }
    static inline bool equals(TrainingData* value, const TrainingData::Key* key, int unused) {
      return equals(value->key(), key);
    }
    int cmp(const Key* that) const {
      auto h1 = this->holder();
      auto h2 = that->holder();
      #define NULL_CHECKS(x1, x2, cmpx1x2)                      \
        ((x1) == nullptr ? -1 : (x2) == nullptr ? +1 : cmpx1x2)
      if (h1 != h2) {
        return NULL_CHECKS(h1, h2, h1->key()->cmp(h2->key()));
      }
      Symbol* k1; Symbol* k2;
      #define CHECK_COMPONENT(name)                             \
        if ((k1 = this->name()) != (k2 = that->name()))         \
          return NULL_CHECKS(k1, k2, k1->cmp(k2))
      CHECK_COMPONENT(name1);
      CHECK_COMPONENT(name2);
      #undef CHECK_COMPONENT
      #undef NULL_CHECKS
      return 0; // no pair of differing components
    }
    Symbol* name1() const       { return _name1; }
    Symbol* name2() const       { return _name2; }
    const TrainingData* holder() const { return _holder; }

    void metaspace_pointers_do(MetaspaceClosure *iter);
  };
  class TrainingDataLocker {
    static int _lock_mode;
    static void lock() {
      assert(_lock_mode != 0, "Forgot to call TrainingDataLocker::initialize()");
      if (_lock_mode > 0) {
        TrainingData_lock->lock();
      }
    }
    static void unlock() {
      if (_lock_mode > 0) {
        TrainingData_lock->unlock();
      }
    }
    static bool safely_locked() {
      assert(_lock_mode != 0, "Forgot to call TrainingDataLocker::initialize()");
      if (_lock_mode > 0) {
        return TrainingData_lock->owned_by_self();
      } else {
        return true;
      }
    }
  public:
    static void initialize() {
      _lock_mode = need_data() ? +1 : -1;   // if -1, we go lock-free
    }
    static void assert_locked() {
      assert(TrainingDataLocker::safely_locked(), "use under TrainingDataLocker");
    }
    TrainingDataLocker() {
      lock();
    }
    ~TrainingDataLocker() {
      unlock();
    }
  };
  class TrainingDataSet {
    friend TrainingData;
    ResizeableResourceHashtable<const Key*, TrainingData*,
                                AnyObj::C_HEAP, MEMFLAGS::mtCompiler,
                                &TrainingData::Key::hash,
                                &TrainingData::Key::equals>
      _table;

  public:
    template<typename... Arg>
    TrainingDataSet(Arg... arg)
      : _table(arg...) {
    }
    TrainingData* find(const Key* key) const {
      TrainingDataLocker::assert_locked();
      auto res = _table.get(key);
      return res == nullptr ? nullptr : *res;
    }
    bool remove(const Key* key) {
      return _table.remove(key);
    }
    TrainingData* install(TrainingData* tdata) {
      TrainingDataLocker::assert_locked();
      auto key = tdata->key();
      if (key->is_empty())   return tdata;  // unkeyed TD not installed
      bool created = false;
      auto prior = _table.put_if_absent(key, tdata, &created);
      if (prior == nullptr || *prior == tdata) {
        return tdata;
      }
      assert(false, "no pre-existing elements allowed");
      return *prior;
    }
    template<typename FN>
    void iterate_all(FN fn) const { // lambda enabled API
      return _table.iterate_all(fn);
    }
  };

  class Options {
  public:
    enum BooleanOption { XML, CDS };
  private:
    int _boolean_options;
  public:
    void set_boolean_option(BooleanOption o) { _boolean_options |= 1 << o; }
    bool get_boolean_option(BooleanOption o) { return _boolean_options & (1 << o); }
    void parse();
    void print_on(outputStream* st);
  };
private:
  Key _key;
  bool _do_not_dump;
  static Options _options;

  // just forward all constructor arguments to the embedded key
  template<typename... Arg>
  TrainingData(Arg... arg)
    : _key(arg...) {
    _do_not_dump = false;
  }

  static TrainingDataSet _training_data_set;
  static TrainingDataDictionary _archived_training_data_dictionary;
  static GrowableArrayCHeap<DumpTimeTrainingDataInfo, mtClassShared>* _dumptime_training_data_dictionary;
  static Array<MethodTrainingData*>* _recompilation_schedule;
  static volatile bool* _recompilation_status;

  static Options* options() { return &_options; }
public:
  // Returns the key under which this TD is installed, or else
  // Key::EMPTY if it is not installed.
  const Key* key() const { return &_key; }

  bool do_not_dump() const { return _do_not_dump; }
  void set_do_not_dump(bool z) { _do_not_dump = z; }

  static bool have_data() { return ReplayTraining;  } // Going to read
  static bool need_data() { return RecordTraining;  } // Going to write
  static bool use_cds()   { return options()->get_boolean_option(Options::BooleanOption::CDS); }
  static bool use_xml()   { return options()->get_boolean_option(Options::BooleanOption::XML); }

  static TrainingDataSet* training_data_set() { return &_training_data_set; }
  static TrainingDataDictionary* archived_training_data_dictionary() { return &_archived_training_data_dictionary; }
  static bool have_recompilation_schedule() { return _recompilation_schedule != nullptr; }
  static Array<MethodTrainingData*>* recompilation_schedule() { return _recompilation_schedule; }
  static volatile bool* recompilation_status() { return _recompilation_status; }

  virtual MethodTrainingData*   as_MethodTrainingData()  const { return nullptr; }
  virtual KlassTrainingData*    as_KlassTrainingData()   const { return nullptr; }
  virtual CompileTrainingData*  as_CompileTrainingData() const { return nullptr; }
  bool is_MethodTrainingData()  const { return as_MethodTrainingData() != nullptr; }
  bool is_KlassTrainingData()   const { return as_KlassTrainingData()  != nullptr; }
  bool is_CompileTrainingData() const { return as_CompileTrainingData()  != nullptr; }

  virtual int cmp(const TrainingData* that) const = 0;

  enum DumpPhase {
    DP_prepare,   // no output, set final structure
    DP_identify,  // output only an id='%d' element
    DP_detail,    // output any additional information
  };
  virtual bool dump(TrainingDataDumper& tdd, DumpPhase dp) = 0;
  // prepare_to_dump(TrainingDataDumper& tdd) { return dump(tdd, DP_prepare); }
  // dump_identity(TrainingDataDumper& tdd) { return dump(tdd, DP_identify); }
  // dump_detail(TrainingDataDumper& tdd) { return dump(tdd, DP_detail); }

  static void initialize();

  // Store results to a file, and/or mark them for retention by CDS,
  // if RecordTraining is enabled.
  static void store_results();

  // Load stored results from a file if ReplayTraining is enabled.
  static void load_profiles();

  // Widget for recording dependencies, as an N-to-M graph relation,
  // possibly cyclic.
  template<typename E>
  class DepList : public StackObj {
    GrowableArrayCHeap<E, mtCompiler>* _deps_dyn;
    Array<E>*                          _deps;
    // (hmm, could we have state-selected union of these two?)
  public:
    DepList() {
      _deps_dyn = nullptr;
      _deps = nullptr;
    }

    int length() const {
      return (_deps_dyn != nullptr ? _deps_dyn->length()
              : _deps   != nullptr ? _deps->length()
              : 0);
    }
    E* adr_at(int i) const {
      return (_deps_dyn != nullptr ? _deps_dyn->adr_at(i)
              : _deps   != nullptr ? _deps->adr_at(i)
              : nullptr);
    }
    E at(int i) const {
      assert(i >= 0 && i < length(), "oob");
      return *adr_at(i);
    }
    bool append_if_missing(E dep) {
      //assert(_deps == nullptr, "must be growable");
      if (_deps_dyn == nullptr) {
        _deps_dyn = new GrowableArrayCHeap<E, mtCompiler>(10);
        _deps_dyn->append(dep);
        return true;
      } else {
        return _deps_dyn->append_if_missing(dep);
      }
    }
    bool contains(E dep) {
      for (int i = 0; i < length(); i++) {
        if (dep == at(i)) {
          return true; // found
        }
      }
      return false; // not found
    }

#if INCLUDE_CDS
    void remove_unshareable_info() {
      _deps_dyn = nullptr;
    }
    void restore_unshareable_info(TRAPS) {}
#endif
    void prepare(ClassLoaderData* loader_data);
    void metaspace_pointers_do(MetaspaceClosure *iter);
  };

  virtual void metaspace_pointers_do(MetaspaceClosure *iter);

#if INCLUDE_CDS
  virtual void remove_unshareable_info() {}
  virtual void restore_unshareable_info(TRAPS) {}
  static void restore_all_unshareable_info(TRAPS);
#endif
  static void init_dumptime_table(TRAPS);
  static void prepare_recompilation_schedule(TRAPS);
  static void iterate_roots(MetaspaceClosure* it);
  static void dump_training_data();
  static void cleanup_training_data();
  static void serialize_training_data(SerializeClosure* soc);
  static void adjust_training_data_dictionary();
  static void print_archived_training_data_on(outputStream* st);
  static void write_training_data_dictionary(TrainingDataDictionary* dictionary);
  static size_t estimate_size_for_archive();

  virtual void cleanup() = 0;

  static TrainingData* lookup_archived_training_data(const Key* k);
};



class KlassTrainingData : public TrainingData {
  friend TrainingData;
  friend CompileTrainingData;

  // Used by CDS. These classes need to access the private default constructor.
  template <class T> friend class CppVtableTesterA;
  template <class T> friend class CppVtableTesterB;
  template <class T> friend class CppVtableCloner;

 public:
  // Tracking field initialization, when RecordTraining is enabled.
  struct FieldData {
    Symbol*   _name;    // name of field, for making reports (no refcount)
    int      _index;    // index in the field stream (a unique id)
    BasicType _type;    // what kind of field is it?
    int     _offset;    // offset of field storage, within mirror
    int _fieldinit_sequence_index;  // 1-based local initialization order
    void init_from(fieldDescriptor& fd) {
      _name = fd.name();
      _index = fd.index();
      _offset = fd.offset();
      _type = fd.field_type();
      _fieldinit_sequence_index = 0;
    }
  };

 private:
  // cross-link to live klass, or null if not loaded or encountered yet
  InstanceKlass* _holder;
  jobject _holder_mirror;   // extra link to prevent unloading by GC

  // initialization tracking state
  bool _has_initialization_touch;
  int _clinit_sequence_index;       // 1-based global initialization order
  GrowableArrayCHeap<FieldData, mtCompiler>* _static_fields;  //do not CDS
  int _fieldinit_count;  // count <= _static_fields.length()
  bool _clinit_is_done;
  DepList<KlassTrainingData*> _init_deps;  // classes to initialize before me
  DepList<CompileTrainingData*> _comp_deps; // compiles that depend on me
  static GrowableArrayCHeap<FieldData, mtCompiler>* _no_static_fields;
  static int _clinit_count;  // global count (so far) of clinit events

  void init() {
    _holder_mirror = nullptr;
    _holder = nullptr;
    _has_initialization_touch = false;
    _clinit_sequence_index = 0;
    _static_fields = nullptr;
    _fieldinit_count = 0;
    _clinit_is_done = false;
  }

  KlassTrainingData() {
    assert(DumpSharedSpaces || UseSharedSpaces, "only for CDS");
  }

  KlassTrainingData(Symbol* klass_name, Symbol* loader_name)
    : TrainingData(klass_name, loader_name)
  {
    init();
  }
  KlassTrainingData(InstanceKlass* klass)
    : TrainingData(klass)
  {
    init();
  }

  int init_dep_count() const {
    TrainingDataLocker::assert_locked();
    return _init_deps.length();
  }
  KlassTrainingData* init_dep(int i) const {
    TrainingDataLocker::assert_locked();
    return _init_deps.at(i);
  }
  void add_init_dep(KlassTrainingData* ktd) {
    TrainingDataLocker::assert_locked();
    guarantee(ktd != this, "");
    _init_deps.append_if_missing(ktd);
  }

  int comp_dep_count() const {
    TrainingDataLocker::assert_locked();
    return _comp_deps.length();
  }
  CompileTrainingData* comp_dep(int i) const {
    TrainingDataLocker::assert_locked();
    return _comp_deps.at(i);
  }
  void add_comp_dep(CompileTrainingData* ctd) {
    TrainingDataLocker::assert_locked();
     _comp_deps.append_if_missing(ctd);
  }

  static GrowableArrayCHeap<FieldData, mtCompiler>* no_static_fields();
  static int next_clinit_count() {
    return Atomic::add(&_clinit_count, 1);
  }
  int next_fieldinit_count() {
    return Atomic::add(&_fieldinit_count, 1);
  }
  void log_initialization(bool is_start);

  bool record_static_field_init(FieldData* fdata, const char* reason);

 public:
  Symbol* name()                const { return _key.name1(); }
  Symbol* loader_name()         const { return _key.name2(); }
  bool    has_holder()          const { return _holder != nullptr; }
  InstanceKlass* holder()       const { return _holder; }

  // This sets up the mirror as well, and may scan for field metadata.
  void init_holder(const InstanceKlass* klass);

  // Update any copied data.
  void refresh_from(const InstanceKlass* klass);

  // factories from live class and from symbols:
  static KlassTrainingData* make(Symbol* name, Symbol* loader_name);
  static KlassTrainingData* make(const char* name, const char* loader_name);
  static KlassTrainingData* make(InstanceKlass* holder,
                                 bool null_if_not_found = false);
  static KlassTrainingData* find(InstanceKlass* holder) {
    return make(holder, true);
  }
  virtual int cmp(const TrainingData* that) const;

  virtual KlassTrainingData* as_KlassTrainingData() const { return const_cast<KlassTrainingData*>(this); };

  ClassLoaderData* class_loader_data() {
    assert(has_holder(), "");
    return holder()->class_loader_data();
  }
  void setup_static_fields(const InstanceKlass* holder);
  bool field_state_is_clean(FieldData* fdata);
  FieldData* check_field_states_and_find_field(Symbol* name);
  bool all_field_states_done() {
    return _static_fields != nullptr && _static_fields->length() == _fieldinit_count;
  }
  static void print_klass_attrs(xmlStream* xtty,
                                Klass* klass, const char* prefix = "");
  static void print_iclock_attr(InstanceKlass* klass,
                                xmlStream* xtty,
                                int fieldinit_index = -1,
                                const char* prefix = "");

  void record_touch_common(xmlStream* xtty,
                           const char* reason,
                           CompileTask* jit_task,
                           Klass* init_klass,
                           Klass* requesting_klass,
                           Symbol* name,
                           Symbol* sig,
                           const char* context);

  // A 1-based global order in which <clinit> was called, or zero if
  // that never did happen, or has not yet happened.
  int clinit_sequence_index_or_zero() const {
    return _clinit_sequence_index;
  }

  // Has this class been the subject of an initialization touch?
  bool has_initialization_touch() { return _has_initialization_touch; }
  // Add such a touch to the history of this class.
  bool add_initialization_touch(Klass* requester);

  // For some reason, somebody is touching my class (this->holder())
  // and that might be relevant to my class's initialization state.
  // We collect these events even after my class is fully initialized.
  //
  // The requesting class, if not null, is the class which is causing
  // the event, somehow (depending on the reason).
  //
  // The name and signature, if not null, are somehow relevant to
  // the event; depending on the reason, they might refer to a
  // member of my class, or else to a member of the requesting class.
  //
  // The context is a little extra information.
  //
  // The record that will be emitted records all this information,
  // plus extra stuff, notably whether there is a <clinit> execution
  // on stack, and if so, who that is.  Often, the class running its
  // <clinit> is even more interesting than the requesting class.
  void record_initialization_touch(const char* reason,
                                   Symbol* name,
                                   Symbol* sig,
                                   Klass* requesting_klass,
                                   const char* context,
                                   TRAPS);

  void record_initialization_start();
  void record_initialization_end();
  void notice_fully_initialized();
  // Record that we have witnessed the initialization of the name.
  // This is called when we know we are doing a `putstatic` or equivalent.
  // It can be called either just before or just after.  It is only
  // safe to call this inside the initializing thread.
  bool record_static_field_init(fieldDescriptor* fd, const char* reason);

  void cleanup();

  void print_on(outputStream* st, bool name_only) const;
  virtual void print_on(outputStream* st) const { print_on(st, false); }
  virtual void print_value_on(outputStream* st) const { print_on(st, true); }

  virtual bool dump(TrainingDataDumper& tdd, DumpPhase dp);

  MetaspaceObj::Type type() const {
    return KlassTrainingDataType;
  }

#if INCLUDE_CDS
  virtual void remove_unshareable_info();
  virtual void restore_unshareable_info(TRAPS);
#endif

  void metaspace_pointers_do(MetaspaceClosure *iter);

  int size() const {
    return (int)align_metadata_size(align_up(sizeof(KlassTrainingData), BytesPerWord)/BytesPerWord);
  }

  const char* internal_name() const {
    return "{ klass training data }";
  };

  static KlassTrainingData* allocate(InstanceKlass* holder);
  static KlassTrainingData* allocate(Symbol* name, Symbol* loader_name);

  template<typename FN>
  void iterate_all_comp_deps(FN fn) const { // lambda enabled API
    TrainingDataLocker l;
    for (int i = 0; i < comp_dep_count(); i++) {
      fn(comp_dep(i));
    }
  }
};

// Information about particular JIT tasks.
class CompileTrainingData : public TrainingData {
  // Used by CDS. These classes need to access the private default constructor.
  template <class T> friend class CppVtableTesterA;
  template <class T> friend class CppVtableTesterB;
  template <class T> friend class CppVtableCloner;

  MethodTrainingData* _method;  // inlined method, or same as top method
  MethodTrainingData* _top_method;
  CompileTrainingData* _next;   // singly linked list, latest first
  const short _level;
  const int _compile_id;
  int _nm_total_size;
  float _qtime, _stime, _etime;   // time queued, started, ended

  // classes that should be initialized before this JIT task runs
  DepList<KlassTrainingData*> _init_deps;
  volatile int _init_deps_left;

  CompileTrainingData() : _level(-1), _compile_id (-1) {
    assert(DumpSharedSpaces || UseSharedSpaces, "only for CDS");
  }

  // (should we also capture counters or MDO state or replay data?)
  CompileTrainingData(MethodTrainingData* method,
                      MethodTrainingData* top_method,
                      int level,
                      int compile_id)
      : TrainingData(),  // empty key
        _method(method), _top_method(top_method),
        _level(level), _compile_id(compile_id)
  {
    _next = nullptr;
    _qtime = _stime = _etime = 0;
    _nm_total_size = 0;
    _init_deps_left = 0;
  }

public:
  // Record a use of a method in a given task.  If non-null, the given
  // method is not the top-level method of the task, but instead it is
  // inlined into the top-level method.
  static CompileTrainingData* make(CompileTask* task,
                                   Method* inlined_method = nullptr);
  static CompileTrainingData* make(MethodTrainingData* this_method,
                                   MethodTrainingData* top_method,
                                   int level, int compile_id);
  static CompileTrainingData* make(MethodTrainingData* method,
                                   int level, int compile_id) {
    return make(method, method, level, compile_id);
  }

  virtual CompileTrainingData* as_CompileTrainingData() const { return const_cast<CompileTrainingData*>(this); };

  MethodTrainingData* method()      const { return _method; }
  MethodTrainingData* top_method()  const { return _top_method; }
  bool                is_inlined()  const { return _method != _top_method; }

  CompileTrainingData* next() const { return _next; }

  int level() const { return _level; }
  //void set_level(int level) { _level = level; }

  int compile_id() const { return _compile_id; }

  int init_dep_count() const {
    TrainingDataLocker::assert_locked();
    return _init_deps.length();
  }
  KlassTrainingData* init_dep(int i) const {
    TrainingDataLocker::assert_locked();
    return _init_deps.at(i);
  }
  void add_init_dep(KlassTrainingData* ktd) {
    TrainingDataLocker::assert_locked();
    ktd->add_comp_dep(this);
    _init_deps.append_if_missing(ktd);
  }
  void dec_init_deps_left(KlassTrainingData* ktd);
  int init_deps_left() const {
    return _init_deps_left;
  }
  void initialize_deps_tracking() {
    for (int i = 0; i < _init_deps.length(); i++) {
      KlassTrainingData* dep = _init_deps.at(i);
      if (dep->has_holder() && !dep->holder()->is_initialized()) {
        _init_deps_left++; // ignore symbolic refs && already initialized classes
      }
    }
  }
  void record_compilation_queued(CompileTask* task);
  void record_compilation_start(CompileTask* task);
  void record_compilation_end(CompileTask* task);
  void notice_inlined_method(CompileTask* task, const methodHandle& method);

  // The JIT looks at classes and objects too and can depend on their state.
  // These simple calls just report the *possibility* of an observation.
  void notice_jit_observation(ciEnv* env, ciBaseObject* what);

  virtual int cmp(const TrainingData* that) const;

  virtual bool dump(TrainingDataDumper& tdd, DumpPhase dp);

  void print_on(outputStream* st, bool name_only) const;
  virtual void print_on(outputStream* st) const { print_on(st, false); }
  virtual void print_value_on(outputStream* st) const { print_on(st, true); }

#if INCLUDE_CDS
  virtual void remove_unshareable_info();
  virtual void restore_unshareable_info(TRAPS);
#endif

  virtual void metaspace_pointers_do(MetaspaceClosure* iter);
  virtual MetaspaceObj::Type type() const { return CompileTrainingDataType; }

  virtual const char* internal_name() const {
    return "{ compile training data }";
  };

  virtual int size() const {
    return (int)align_metadata_size(align_up(sizeof(CompileTrainingData), BytesPerWord)/BytesPerWord);
  }

  void cleanup();

  static CompileTrainingData* allocate(MethodTrainingData* this_method,
                                       MethodTrainingData* top_method,
                                       int level, int compile_id);
};

// Record information about a method at the time compilation is requested.
class MethodTrainingData : public TrainingData {
  friend TrainingData;
  friend CompileTrainingData;

  // Used by CDS. These classes need to access the private default constructor.
  template <class T> friend class CppVtableTesterA;
  template <class T> friend class CppVtableTesterB;
  template <class T> friend class CppVtableCloner;

  KlassTrainingData* _klass;
  const Method* _holder;  // can be null
  CompileTrainingData* _compile;   // singly linked list, latest first
  CompileTrainingData* _last_toplevel_compiles[CompLevel_count];
  int _highest_top_level;
  int _level_mask;  // bit-set of all possible levels
  bool _was_inlined;
  bool _was_toplevel;
  // metadata snapshots of final state:
  MethodCounters* _final_counters;
  MethodData*     _final_profile;

  MethodTrainingData() {
    assert(DumpSharedSpaces || UseSharedSpaces, "only for CDS");
  }

  MethodTrainingData(KlassTrainingData* klass,
                     Symbol* name, Symbol* signature)
    : TrainingData(klass, name, signature)
  {
    _klass = klass;
    _holder = nullptr;
    _compile = nullptr;
    for (int i = 0; i < CompLevel_count; i++) {
      _last_toplevel_compiles[i] = nullptr;
    }
    _highest_top_level = CompLevel_none;
    _level_mask = 0;
    _was_inlined = _was_toplevel = false;
  }

  static int level_mask(int level) {
    return ((level & 0xF) != level ? 0 : 1 << level);
  }
  static CompLevel highest_level(int mask) {
    if (mask == 0)  return (CompLevel) 0;
    int diff = (count_leading_zeros(level_mask(0)) - count_leading_zeros(mask));
    return (CompLevel) diff;
  }

 public:
  KlassTrainingData* klass()  const { return _klass; }
  CompileTrainingData* compile() const { return _compile; }
  bool has_holder()           const { return _holder != nullptr; }
  const Method* holder()      const { return _holder; }
  Symbol* name()              const { return _key.name1(); }
  Symbol* signature()         const { return _key.name2(); }
  bool only_inlined()         const { return !_was_toplevel; }
  bool never_inlined()        const { return !_was_inlined; }
  bool saw_level(CompLevel l) const { return (_level_mask & level_mask(l)) != 0; }
  int highest_level()         const { return highest_level(_level_mask); }
  int highest_top_level()     const { return _highest_top_level; }
  MethodData* final_profile() const { return _final_profile; }

  CompileTrainingData* last_toplevel_compile(int level) const {
    if (level > CompLevel_none) {
      return _last_toplevel_compiles[level - 1];
    }
    return nullptr;
  }

  inline int last_compile_id() const;

  void notice_compilation(int level, bool inlined = false) {
    if (inlined)  _was_inlined = true;
    else          _was_toplevel = true;
    _level_mask |= level_mask(level);
  }

  // Update any copied data.
  void refresh_from(const Method* method);

  static MethodTrainingData* make(KlassTrainingData* klass,
                                  Symbol* name, Symbol* signature);
  static MethodTrainingData* make(KlassTrainingData* klass,
                                  const char* name, const char* signature);
  static MethodTrainingData* make(const methodHandle& method,
                                  bool null_if_not_found = false);
  static MethodTrainingData* find(const methodHandle& method) {
    return make(method, true);
  }

  virtual int cmp(const TrainingData* that) const;

  virtual MethodTrainingData* as_MethodTrainingData() const { return const_cast<MethodTrainingData*>(this); };

  void print_on(outputStream* st, bool name_only) const;
  virtual void print_on(outputStream* st) const { print_on(st, false); }
  virtual void print_value_on(outputStream* st) const { print_on(st, true); }

  virtual bool dump(TrainingDataDumper& tdd, DumpPhase dp);

  void cleanup();

  template<typename FN>
  void iterate_all_compiles(FN fn) const { // lambda enabled API
    for (CompileTrainingData* ctd = _compile; ctd != nullptr; ctd = ctd->next()) {
      fn(ctd);
    }
  }

  void initialize_deps_tracking() {
    iterate_all_compiles([](CompileTrainingData* ctd) { ctd->initialize_deps_tracking(); });
  }

  virtual void metaspace_pointers_do(MetaspaceClosure* iter);
  virtual MetaspaceObj::Type type() const { return MethodTrainingDataType; }

#if INCLUDE_CDS
  virtual void remove_unshareable_info();
  virtual void restore_unshareable_info(TRAPS);
#endif

  virtual int size() const {
    return (int)align_metadata_size(align_up(sizeof(MethodTrainingData), BytesPerWord)/BytesPerWord);
  }

  virtual const char* internal_name() const {
    return "{ method training data }";
  };

  static MethodTrainingData* allocate(KlassTrainingData* ktd, Method* m);
  static MethodTrainingData* allocate(KlassTrainingData* ktd, Symbol* name, Symbol* signature);
};

inline int MethodTrainingData::last_compile_id() const {
  return (_compile == nullptr ? 0 : _compile->compile_id());
}

// CDS support

class DumpTimeTrainingDataInfo {
  TrainingData* _training_data;
public:
  DumpTimeTrainingDataInfo() : DumpTimeTrainingDataInfo(nullptr) {}

  DumpTimeTrainingDataInfo(TrainingData* training_data)
      : _training_data(training_data) {}

  void metaspace_pointers_do(MetaspaceClosure* it) {
    it->push(&_training_data);
  }

  TrainingData* training_data() {
    return _training_data;
  }
};

class TrainingDataDictionary : public OffsetCompactHashtable<
    const TrainingData::Key*, TrainingData*,
    TrainingData::Key::equals> {};

class TrainingDataPrinter : StackObj {
  outputStream* _st;
  int _index;

public:
  TrainingDataPrinter(outputStream* st) : _st(st), _index(0) {}

  void do_value(TrainingData* record);
  void do_value(const RunTimeClassInfo* record);
  void do_value(const RunTimeMethodDataInfo* record);
};

#endif // SHARE_OOPS_TRAININGDATA_HPP
