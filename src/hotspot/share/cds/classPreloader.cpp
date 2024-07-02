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
#include "cds/archiveBuilder.hpp"
#include "cds/archiveUtils.inline.hpp"
#include "cds/cdsAccess.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsProtectionDomain.hpp"
#include "cds/classPrelinker.hpp"
#include "cds/classPreloader.hpp"
#include "cds/heapShared.hpp"
#include "cds/lambdaFormInvokers.inline.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "compiler/compilationPolicy.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/trainingData.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/perfData.inline.hpp"
#include "runtime/timer.hpp"
#include "services/management.hpp"

ClassPreloader::ClassesTable* ClassPreloader::_vm_classes = nullptr;
ClassPreloader::ClassesTable* ClassPreloader::_preloaded_classes = nullptr;
ClassPreloader::ClassesTable* ClassPreloader::_platform_initiated_classes = nullptr;
ClassPreloader::ClassesTable* ClassPreloader::_app_initiated_classes = nullptr;
bool ClassPreloader::_record_javabase_only = true;
bool ClassPreloader::_preload_javabase_only = true;
ClassPreloader::PreloadedKlasses ClassPreloader::_static_preloaded_classes;
ClassPreloader::PreloadedKlasses ClassPreloader::_dynamic_preloaded_classes;
Array<InstanceKlass*>* ClassPreloader::_unregistered_classes_from_preimage = nullptr;

static PerfCounter* _perf_classes_preloaded = nullptr;
static PerfTickCounters* _perf_class_preload_counters = nullptr;

void ClassPreloader::initialize() {
  _vm_classes = new (mtClass)ClassesTable();
  _preloaded_classes = new (mtClass)ClassesTable();
  _platform_initiated_classes = new (mtClass)ClassesTable();
  _app_initiated_classes = new (mtClass)ClassesTable();

  for (auto id : EnumRange<vmClassID>{}) {
    add_one_vm_class(vmClasses::klass_at(id));
  }

  if (_static_preloaded_classes._boot != nullptr && !CDSConfig::is_dumping_final_static_archive()) {
    assert(CDSConfig::is_dumping_dynamic_archive(), "must be");
    add_preloaded_classes(_static_preloaded_classes._boot);
    add_preloaded_classes(_static_preloaded_classes._boot2);
    add_preloaded_classes(_static_preloaded_classes._platform);
    add_preloaded_classes(_static_preloaded_classes._app);

    add_unrecorded_initiated_classes(_platform_initiated_classes, _static_preloaded_classes._platform_initiated);
    add_unrecorded_initiated_classes(_app_initiated_classes, _static_preloaded_classes._app_initiated);
  }

  // Record all the initiated classes that we used during dump time. This covers the verification constraints and
  // (resolved) class loader constraints.
  add_initiated_classes_for_loader(ClassLoaderData::class_loader_data_or_null(SystemDictionary::java_platform_loader()),
                                   "platform", _platform_initiated_classes);
  add_initiated_classes_for_loader(ClassLoaderData::class_loader_data_or_null(SystemDictionary::java_system_loader()),
                                   "app", _app_initiated_classes);
}

void ClassPreloader::dispose() {
  delete _vm_classes;
  delete _preloaded_classes;
  delete _platform_initiated_classes;
  delete _app_initiated_classes;
  _vm_classes = nullptr;
  _preloaded_classes = nullptr;
  _platform_initiated_classes = nullptr;
  _app_initiated_classes = nullptr;
}

bool ClassPreloader::is_vm_class(InstanceKlass* ik) {
  return (_vm_classes->get(ik) != nullptr);
}

void ClassPreloader::add_one_vm_class(InstanceKlass* ik) {
  bool created;
  add_preloaded_class(ik);
  _vm_classes->put_if_absent(ik, &created);
  if (created) {
    InstanceKlass* super = ik->java_super();
    if (super != nullptr) {
      add_one_vm_class(super);
    }
    Array<InstanceKlass*>* ifs = ik->local_interfaces();
    for (int i = 0; i < ifs->length(); i++) {
      add_one_vm_class(ifs->at(i));
    }
  }
}

bool ClassPreloader::is_preloaded_class(InstanceKlass* ik) {
  return (_preloaded_classes->get(ik) != nullptr);
}

void ClassPreloader::add_preloaded_class(InstanceKlass* ik) {
  bool created;
  _preloaded_classes->put_if_absent(ik, &created);
}

void ClassPreloader::add_preloaded_classes(Array<InstanceKlass*>* klasses) {
  for (int i = 0; i < klasses->length(); i++) {
    assert(klasses->at(i)->is_shared() && klasses->at(i)->is_loaded(), "must be");
    _preloaded_classes->put_when_absent(klasses->at(i), true);
  }
}

void ClassPreloader::add_unrecorded_initiated_classes(ClassesTable* table, Array<InstanceKlass*>* klasses) {
  // These initiated classes are already recorded in the static archive. There's no need to
  // record them again for the dynamic archive.
  assert(CDSConfig::is_dumping_dynamic_archive(), "must be");
  bool need_to_record = false;
  for (int i = 0; i < klasses->length(); i++) {
    InstanceKlass* ik = klasses->at(i);
    table->put_when_absent(ik, need_to_record);
  }
}

void ClassPreloader::add_extra_initiated_classes(PreloadedKlasses* table) {
  if (table->_app->length() > 0) {
    // Add all public classes in boot/platform to the app loader. This speeds up
    // Class.forName() operations in frameworks.
    GrowableArray<Klass*>* klasses = ArchiveBuilder::current()->klasses();
    for (GrowableArrayIterator<Klass*> it = klasses->begin(); it != klasses->end(); ++it) {
      Klass* k = *it;
      if (k->is_instance_klass() && !k->name()->starts_with("jdk/proxy")) { // FIXME add SystemDictionaryShared::is_archived_dynamic_proxy_class(ik)
        // TODO: only add classes that are visible to unnamed module in app loader.
        InstanceKlass* ik = InstanceKlass::cast(k);
        if (ik->is_public() && (ik->is_shared_boot_class() || ik->is_shared_platform_class())) {
          add_initiated_class(_app_initiated_classes, "app", ik);
        }
      }
    }
  }
}

class ClassPreloader::RecordInitiatedClassesClosure : public KlassClosure {
  ClassLoaderData* _loader_data;
  const char* _loader_name;
  ClassesTable* _table;
 public:
  RecordInitiatedClassesClosure(ClassLoaderData* loader_data, const char* loader_name, ClassesTable* table) :
    _loader_data(loader_data), _loader_name(loader_name), _table(table) {}
  virtual void do_klass(Klass* k) {
    if (k->is_instance_klass() && k->class_loader_data() != _loader_data) {
      add_initiated_class(_table, _loader_name, InstanceKlass::cast(k));
    }
  }
};

void ClassPreloader::add_initiated_classes_for_loader(ClassLoaderData* loader_data, const char* loader_name, ClassesTable* table) {
  if (loader_data != nullptr) {
    MonitorLocker mu1(SystemDictionary_lock);
    RecordInitiatedClassesClosure mk(loader_data, loader_name, table);  
    loader_data->dictionary()->all_entries_do(&mk);
  }
}

// ik has a reference to target:
//    - target is a declared supertype of ik, or
//    - one of the constant pool entries in ik references target
void ClassPreloader::add_initiated_class(InstanceKlass* ik, InstanceKlass* target) {
  if (ik->shared_class_loader_type() == target->shared_class_loader_type()) {
    return;
  }

  if (SystemDictionary::is_platform_class_loader(ik->class_loader())) {
    add_initiated_class(_platform_initiated_classes, "platform", target);
  } else {
    assert(SystemDictionary::is_system_class_loader(ik->class_loader()), "must be");
    add_initiated_class(_app_initiated_classes, "app", target);
  }
}

void ClassPreloader::add_initiated_class(ClassesTable* initiated_classes, const char* loader_name, InstanceKlass* target) {
  bool need_to_record = true;
  bool created;
  initiated_classes->put_if_absent(target, need_to_record, &created);
  if (created && log_is_enabled(Trace, cds, resolve)) {
    ResourceMark rm;
    log_trace(cds, resolve)("%s loader initiated %s", loader_name, target->external_name());
  }
}

bool ClassPreloader::is_in_javabase(InstanceKlass* ik) {
  if (ik->is_hidden() && HeapShared::is_lambda_form_klass(ik)) {
    return true;
  }

  return (ik->module() != nullptr &&
          ik->module()->name() != nullptr &&
          ik->module()->name()->equals("java.base"));
}

class ClassPreloader::PreloadedKlassRecorder : StackObj {
  int _loader_type;
  ResourceHashtable<InstanceKlass*, bool, 15889, AnyObj::RESOURCE_AREA, mtClassShared> _seen_classes;
  GrowableArray<InstanceKlass*> _list;
  bool loader_type_matches(InstanceKlass* ik) {
    InstanceKlass* buffered_ik = ArchiveBuilder::current()->get_buffered_addr(ik);
    return buffered_ik->shared_class_loader_type() == _loader_type;
  }

  void maybe_record(InstanceKlass* ik) {
    bool created;
    _seen_classes.put_if_absent(ik, true, &created);
    if (!created) {
      // Already seen this class when we walked the hierarchy of a previous class
      return;
    }
    if (!loader_type_matches(ik)) {
      return;
    }

    if (ik->is_hidden()) {
      assert(ik->shared_class_loader_type() != ClassLoader::OTHER, "must have been set");
      if (!CDSConfig::is_dumping_invokedynamic()) {
        return;
      }
      assert(SystemDictionaryShared::should_hidden_class_be_archived(ik), "sanity");
    }

    if (is_vm_class(ik)) {
      // vmClasses are loaded in vmClasses::resolve_all() at the very beginning
      // of VM bootstrap, before ClassPreloader::runtime_preload() is called.
      return;
    }

    if (_loader_type == ClassLoader::BOOT_LOADER) {
      if (_record_javabase_only != is_in_javabase(ik)) {
        return;
      }
    }

    if (MetaspaceObj::is_shared(ik)) {
      if (CDSConfig::is_dumping_dynamic_archive()) {
        return;
      } else {
        assert(CDSConfig::is_dumping_final_static_archive(), "must be");
      }
    }

    if (!ik->is_hidden()) {
      // Do not preload any module classes that are not from the modules images,
      // since such classes may not be loadable at runtime
      int scp_index = ik->shared_classpath_index();
      assert(scp_index >= 0, "must be");
      SharedClassPathEntry* scp_entry = FileMapInfo::shared_path(scp_index);
      if (scp_entry->in_named_module() && !scp_entry->is_modules_image()) {
        return;
      }
    }

    InstanceKlass* s = ik->java_super();
    if (s != nullptr) {
      maybe_record(s);
      add_initiated_class(ik, s);
    }

    Array<InstanceKlass*>* interfaces = ik->local_interfaces();
    int num_interfaces = interfaces->length();
    for (int index = 0; index < num_interfaces; index++) {
      InstanceKlass* intf = interfaces->at(index);
      maybe_record(intf);
      add_initiated_class(ik, intf);
    }

    _list.append((InstanceKlass*)ArchiveBuilder::get_buffered_klass(ik));
    _preloaded_classes->put_when_absent(ik, true);

    if (log_is_enabled(Info, cds, preload)) {
      ResourceMark rm;
      const char* loader_name;
      if (_loader_type  == ClassLoader::BOOT_LOADER) {
        if (_record_javabase_only) {
          loader_name = "boot ";
        } else {
          loader_name = "boot2";
        }
      } else if (_loader_type  == ClassLoader::PLATFORM_LOADER) {
        loader_name = "plat ";
      } else {
        loader_name = "app  ";
      }

      log_info(cds, preload)("%s %s", loader_name, ik->external_name());
    }
  }

public:
  PreloadedKlassRecorder(int loader_type) : _loader_type(loader_type),  _seen_classes(), _list() {}

  void iterate() {
    GrowableArray<Klass*>* klasses = ArchiveBuilder::current()->klasses();
    for (GrowableArrayIterator<Klass*> it = klasses->begin(); it != klasses->end(); ++it) {
      Klass* k = *it;
      //assert(!k->is_shared(), "must be");
      if (k->is_instance_klass()) {
        maybe_record(InstanceKlass::cast(k));
      }
    }
  }

  Array<InstanceKlass*>* to_array() {
    return ArchiveUtils::archive_array(&_list);
  }
};

Array<InstanceKlass*>* ClassPreloader::record_preloaded_classes(int loader_type) {
  ResourceMark rm;
  PreloadedKlassRecorder recorder(loader_type);
  recorder.iterate();
  return recorder.to_array();
}

void ClassPreloader::record_preloaded_classes(bool is_static_archive) {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_classes : &_dynamic_preloaded_classes;

    _record_javabase_only = true;
    table->_boot     = record_preloaded_classes(ClassLoader::BOOT_LOADER);
    _record_javabase_only = false;
    table->_boot2    = record_preloaded_classes(ClassLoader::BOOT_LOADER);

    table->_platform = record_preloaded_classes(ClassLoader::PLATFORM_LOADER);
    table->_app      = record_preloaded_classes(ClassLoader::APP_LOADER);

    add_extra_initiated_classes(table);
  }
}

Array<InstanceKlass*>* ClassPreloader::record_initiated_classes(ClassesTable* table) {
  ResourceMark rm;
  GrowableArray<InstanceKlass*> tmp_array;

  auto collector = [&] (InstanceKlass* ik, bool need_to_record) {
    if (!need_to_record) {
      return;
    }

    if (CDSConfig::is_dumping_final_static_archive() || !ik->is_shared()) {
      if (SystemDictionaryShared::is_excluded_class(ik)) {
        return;
      }
      ik = (InstanceKlass*)ArchiveBuilder::get_buffered_klass(ik);
    }
    tmp_array.append(ik);
    if (log_is_enabled(Info, cds, preload)) {
      ResourceMark rm;
      const char* loader_name;
      if (table == _platform_initiated_classes) {
        loader_name = "plat ";
      } else {
        loader_name = "app  ";
      }
      log_info(cds, preload)("%s %s (initiated)", loader_name, ik->external_name());
    }
  };
  table->iterate_all(collector);

  return ArchiveUtils::archive_array(&tmp_array);
}

void ClassPreloader::record_initiated_classes(bool is_static_archive) {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_classes : &_dynamic_preloaded_classes;
    table->_platform_initiated = record_initiated_classes(_platform_initiated_classes);
    table->_app_initiated = record_initiated_classes(_app_initiated_classes);
  }
}

void ClassPreloader::record_unregistered_classes() {
  if (CDSConfig::is_dumping_preimage_static_archive()) {
    GrowableArray<InstanceKlass*> unreg_classes;
    GrowableArray<Klass*>* klasses = ArchiveBuilder::current()->klasses();
    for (int i = 0; i < klasses->length(); i++) {
      Klass* k = klasses->at(i);
      if (k->is_instance_klass()) {
        InstanceKlass* ik = InstanceKlass::cast(k);
        if (ik->is_shared_unregistered_class()) {
          unreg_classes.append((InstanceKlass*)ArchiveBuilder::get_buffered_klass(ik));
        }
      }
    }
    _unregistered_classes_from_preimage = ArchiveUtils::archive_array(&unreg_classes);
  } else {
    _unregistered_classes_from_preimage = nullptr;
  }
}

void ClassPreloader::serialize(SerializeClosure* soc, bool is_static_archive) {
  PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_classes : &_dynamic_preloaded_classes;

  soc->do_ptr((void**)&table->_boot);
  soc->do_ptr((void**)&table->_boot2);
  soc->do_ptr((void**)&table->_platform);
  soc->do_ptr((void**)&table->_platform_initiated);
  soc->do_ptr((void**)&table->_app);
  soc->do_ptr((void**)&table->_app_initiated);

  if (is_static_archive) {
    soc->do_ptr((void**)&_unregistered_classes_from_preimage);
  }

  if (table->_boot != nullptr && table->_boot->length() > 0) {
    CDSConfig::set_has_preloaded_classes();
  }

  if (is_static_archive && soc->reading() && UsePerfData) {
    JavaThread* THREAD = JavaThread::current();
    NEWPERFEVENTCOUNTER(_perf_classes_preloaded, SUN_CLS, "preloadedClasses");
    NEWPERFTICKCOUNTERS(_perf_class_preload_counters, SUN_CLS, "classPreload");
  }
}

int ClassPreloader::num_platform_initiated_classes() {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = CDSConfig::is_dumping_dynamic_archive() ? &_dynamic_preloaded_classes : &_static_preloaded_classes;
    return table->_platform_initiated->length();
  }
  return 0;
}

int ClassPreloader::num_app_initiated_classes() {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = CDSConfig::is_dumping_dynamic_archive() ? &_dynamic_preloaded_classes : &_static_preloaded_classes;
    return table->_app_initiated->length();
  }
  return 0;
}

volatile bool _class_preloading_finished = false;

bool ClassPreloader::class_preloading_finished() {
  if (!UseSharedSpaces) {
    return true;
  } else {
    // The ConstantPools of preloaded classes have references to other preloaded classes. We don't
    // want any Java code (including JVMCI compiler) to use these classes until all of them
    // are loaded.
    return Atomic::load_acquire(&_class_preloading_finished);
  }
}

// This function is called 4 times:
// preload only java.base classes
// preload boot classes outside of java.base
// preload classes for platform loader
// preload classes for app loader
void ClassPreloader::runtime_preload(JavaThread* current, Handle loader) {
#ifdef ASSERT
  if (loader() == nullptr) {
    static bool first_time = true;
    if (first_time) {
      // FIXME -- assert that no java code has been executed up to this point.
      //
      // Reason: Here, only vmClasses have been loaded. However, their CP might
      // have some pre-resolved entries that point to classes that are loaded
      // only by this function! Any Java bytecode that uses such entries will
      // fail.
    }
    first_time = false;
  }
#endif // ASSERT
  if (UseSharedSpaces) {
    if (loader() != nullptr && !SystemDictionaryShared::has_platform_or_app_classes()) {
      // Non-boot classes might have been disabled due to command-line mismatch.
      Atomic::release_store(&_class_preloading_finished, true);
      return;
    }
    ResourceMark rm(current);
    ExceptionMark em(current);
    runtime_preload(&_static_preloaded_classes, loader, current);
    if (!current->has_pending_exception()) {
      runtime_preload(&_dynamic_preloaded_classes, loader, current);
    }
    _preload_javabase_only = false;

    if (loader() != nullptr && loader() == SystemDictionary::java_system_loader()) {
      Atomic::release_store(&_class_preloading_finished, true);
    }
  }
  assert(!current->has_pending_exception(), "VM should have exited due to ExceptionMark");

  if (loader() != nullptr && loader() == SystemDictionary::java_system_loader()) {
    if (PrintTrainingInfo) {
      tty->print_cr("==================== archived_training_data ** after all classes preloaded ====================");
      TrainingData::print_archived_training_data_on(tty);
    }

    if (log_is_enabled(Info, cds, jit)) {
      CDSAccess::test_heap_access_api();
    }

    if (CDSConfig::is_dumping_final_static_archive()) {
      assert(_unregistered_classes_from_preimage != nullptr, "must be");
      for (int i = 0; i < _unregistered_classes_from_preimage->length(); i++) {
        InstanceKlass* ik = _unregistered_classes_from_preimage->at(i);
        SystemDictionaryShared::init_dumptime_info(ik);
        SystemDictionaryShared::add_unregistered_class(current, ik);
      }
    }
  }
}

void ClassPreloader::runtime_preload(PreloadedKlasses* table, Handle loader, TRAPS) {
  PerfTraceTime timer(_perf_class_preload_counters);
  Array<InstanceKlass*>* preloaded_classes;
  Array<InstanceKlass*>* initiated_classes = nullptr;
  const char* loader_name;
  ClassLoaderData* loader_data = ClassLoaderData::class_loader_data(loader());

  // ResourceMark is missing in the code below due to JDK-8307315
  ResourceMark rm(THREAD);
  if (loader() == nullptr) {
    if (_preload_javabase_only) {
      loader_name = "boot ";
      preloaded_classes = table->_boot;
    } else {
      loader_name = "boot2";
      preloaded_classes = table->_boot2;
    }
  } else if (loader() == SystemDictionary::java_platform_loader()) {
    initiated_classes = table->_platform_initiated;
    preloaded_classes = table->_platform;
    loader_name = "plat ";
  } else {
    assert(loader() == SystemDictionary::java_system_loader(), "must be");
    initiated_classes = table->_app_initiated;
    preloaded_classes = table->_app;
    loader_name = "app  ";
  }

  if (initiated_classes != nullptr) {
    MonitorLocker mu1(SystemDictionary_lock);

    for (int i = 0; i < initiated_classes->length(); i++) {
      InstanceKlass* ik = initiated_classes->at(i);
      assert(ik->is_loaded(), "must have already been loaded by a parent loader");
      if (log_is_enabled(Info, cds, preload)) {
        ResourceMark rm;
        const char* defining_loader = (ik->class_loader() == nullptr ? "boot" : "plat");
        log_info(cds, preload)("%s %s (initiated, defined by %s)", loader_name, ik->external_name(),
                               defining_loader);
      }
      SystemDictionary::preload_class(THREAD, ik, loader_data);
    }
  }

  if (preloaded_classes != nullptr) {
    for (int i = 0; i < preloaded_classes->length(); i++) {
      if (UsePerfData) {
        _perf_classes_preloaded->inc();
      }
      InstanceKlass* ik = preloaded_classes->at(i);
      if (log_is_enabled(Info, cds, preload)) {
        ResourceMark rm;
        log_info(cds, preload)("%s %s%s", loader_name, ik->external_name(),
                               ik->is_loaded() ? " (already loaded)" : "");
      }
      // FIXME Do not load proxy classes if FMG is disabled.

      if (!ik->is_loaded()) {
        if (ik->is_hidden()) {
          preload_archived_hidden_class(loader, ik, loader_name, CHECK);
        } else {
          InstanceKlass* actual;
          if (loader() == nullptr) {
            if (!Universe::is_fully_initialized()) {
              runtime_preload_class_quick(ik, loader_data, Handle(), CHECK);
              actual = ik;
            } else {
              actual = SystemDictionary::load_instance_class(ik->name(), loader, CHECK);
            }
          } else {
            // Note: we are not adding the locker objects into java.lang.ClassLoader::parallelLockMap, but
            // that should be harmless.
            actual = SystemDictionaryShared::find_or_load_shared_class(ik->name(), loader, CHECK);
          }

          if (actual != ik) {
            jvmti_agent_error(ik, actual, "preloaded");
          }
          assert(actual->is_loaded(), "must be");
        }
      }

      // FIXME assert - if FMG, package must be archived
    }

    if (!_preload_javabase_only) {
      // The java.base classes needs to wait till ClassPreloader::init_javabase_preloaded_classes()
      for (int i = 0; i < preloaded_classes->length(); i++) {
        InstanceKlass* ik = preloaded_classes->at(i);
        if (ik->has_preinitialized_mirror()) {
          ik->initialize_from_cds(CHECK);
        } else if (PrelinkSharedClasses && ik->verified_at_dump_time()) {
          ik->link_class(CHECK);
        }
      }
    }
  }

  if (!_preload_javabase_only) {
    HeapShared::initialize_default_subgraph_classes(loader, CHECK);
  }

#if 0
  // Hmm, does JavacBench crash if this block is enabled??
  if (VerifyDuringStartup) {
    VM_Verify verify_op;
    VMThread::execute(&verify_op);
  }
#endif
}

void ClassPreloader::preload_archived_hidden_class(Handle class_loader, InstanceKlass* ik,
                                                   const char* loader_name, TRAPS) {
  DEBUG_ONLY({
      assert(ik->super() == vmClasses::Object_klass(), "must be");
      for (int i = 0; i < ik->local_interfaces()->length(); i++) {
        assert(ik->local_interfaces()->at(i)->is_loaded(), "must be");
      }
    });

  ClassLoaderData* loader_data = ClassLoaderData::class_loader_data(class_loader());
  if (class_loader() == nullptr) {
    ik->restore_unshareable_info(loader_data, Handle(), NULL, CHECK);
  } else {
    PackageEntry* pkg_entry = CDSProtectionDomain::get_package_entry_from_class(ik, class_loader);
    Handle protection_domain =
        CDSProtectionDomain::init_security_info(class_loader, ik, pkg_entry, CHECK);
    ik->restore_unshareable_info(loader_data, protection_domain, pkg_entry, CHECK);
  }
  SystemDictionary::load_shared_class_misc(ik, loader_data);
  ik->add_to_hierarchy(THREAD);
}

void ClassPreloader::runtime_preload_class_quick(InstanceKlass* ik, ClassLoaderData* loader_data, Handle domain, TRAPS) {
  assert(!ik->is_loaded(), "sanity");

#ifdef ASSERT
  {
    InstanceKlass* super = ik->java_super();
    if (super != nullptr) {
      assert(super->is_loaded(), "must have been loaded");
    }
    Array<InstanceKlass*>* intfs = ik->local_interfaces();
    for (int i = 0; i < intfs->length(); i++) {
      assert(intfs->at(i)->is_loaded(), "must have been loaded");
    }
  }
#endif

  ik->restore_unshareable_info(loader_data, domain, nullptr, CHECK);
  SystemDictionary::load_shared_class_misc(ik, loader_data);

  // We are adding to the dictionary but can get away without
  // holding SystemDictionary_lock, as no other threads will be loading
  // classes at the same time.
  assert(!Universe::is_fully_initialized(), "sanity");
  Dictionary* dictionary = loader_data->dictionary();
  dictionary->add_klass(THREAD, ik->name(), ik);
  ik->add_to_hierarchy(THREAD);
  assert(ik->is_loaded(), "Must be in at least loaded state");
}

void ClassPreloader::jvmti_agent_error(InstanceKlass* expected, InstanceKlass* actual, const char* type) {
  if (actual->is_shared() && expected->name() == actual->name() &&
      LambdaFormInvokers::may_be_regenerated_class(expected->name())) {
    // For the 4 regenerated classes (such as java.lang.invoke.Invokers$Holder) there's one
    // in static archive and one in dynamic archive. If the dynamic archive is loaded, we
    // load the one from the dynamic archive.
    return;
  }
  ResourceMark rm;
  log_error(cds)("Unable to resolve %s class from CDS archive: %s", type, expected->external_name());
  log_error(cds)("Expected: " INTPTR_FORMAT ", actual: " INTPTR_FORMAT, p2i(expected), p2i(actual));
  log_error(cds)("JVMTI class retransformation is not supported when archive was generated with -XX:+PreloadSharedClasses.");
  MetaspaceShared::unrecoverable_loading_error();
}

void ClassPreloader::init_javabase_preloaded_classes(TRAPS) {
  Array<InstanceKlass*>* preloaded_classes = _static_preloaded_classes._boot;
  if (preloaded_classes != nullptr) {
    for (int i = 0; i < preloaded_classes->length(); i++) {
      InstanceKlass* ik = preloaded_classes->at(i);
      if (ik->has_preinitialized_mirror()) {
        ik->initialize_from_cds(CHECK);
      }
    }
  }

  // Initialize java.base classes in the default subgraph.
  HeapShared::initialize_default_subgraph_classes(Handle(), CHECK);
}

void ClassPreloader::replay_training_at_init(Array<InstanceKlass*>* preloaded_classes, TRAPS) {
  if (preloaded_classes != nullptr) {
    for (int i = 0; i < preloaded_classes->length(); i++) {
      InstanceKlass* ik = preloaded_classes->at(i);
      if (ik->has_preinitialized_mirror() && ik->is_initialized() && !ik->has_init_deps_processed()) {
        CompilationPolicy::replay_training_at_init(ik, CHECK);
      }
    }
  }
}

void ClassPreloader::replay_training_at_init_for_preloaded_classes(TRAPS) {
  if (CDSConfig::has_preloaded_classes() && TrainingData::have_data()) {
    replay_training_at_init(_static_preloaded_classes._boot,     CHECK);
    replay_training_at_init(_static_preloaded_classes._boot2,    CHECK);
    replay_training_at_init(_static_preloaded_classes._platform, CHECK);
    replay_training_at_init(_static_preloaded_classes._app,      CHECK);

    CompilationPolicy::replay_training_at_init(false, CHECK);
  }
}

void ClassPreloader::print_counters() {
  if (UsePerfData && _perf_class_preload_counters != nullptr) {
    LogStreamHandle(Info, init) log;
    if (log.is_enabled()) {
      log.print_cr("ClassPreloader:");
      log.print_cr("  preload:           %ldms (elapsed) %ld (thread) / %ld events",
                   _perf_class_preload_counters->elapsed_counter_value_ms(),
                   _perf_class_preload_counters->thread_counter_value_ms(),
                   _perf_classes_preloaded->get_value());
    }
  }
}
