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
#include "cds/aotLinkedClassBulkLoader.hpp"
#include "cds/aotLinkedClassTable.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/archiveUtils.inline.hpp"
#include "cds/cdsAccess.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsProtectionDomain.hpp"
#include "cds/heapShared.hpp"
#include "cds/lambdaFormInvokers.inline.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/classLoaderExt.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "compiler/compilationPolicy.hpp"
#include "gc/shared/gcVMOperations.hpp"
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

Array<InstanceKlass*>* AOTLinkedClassBulkLoader::_unregistered_classes_from_preimage = nullptr;
volatile bool _class_preloading_finished = false;

static PerfCounter* _perf_classes_preloaded = nullptr;
static PerfTickCounters* _perf_class_preload_counters = nullptr;

void AOTLinkedClassBulkLoader::record_unregistered_classes() {
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

void AOTLinkedClassBulkLoader::serialize(SerializeClosure* soc, bool is_static_archive) {
  AOTLinkedClassTable::get(is_static_archive)->serialize(soc);

  if (is_static_archive) {
    soc->do_ptr((void**)&_unregistered_classes_from_preimage);

    if (soc->reading() && UsePerfData) {
      JavaThread* THREAD = JavaThread::current();
      NEWPERFEVENTCOUNTER(_perf_classes_preloaded, SUN_CLS, "preloadedClasses");
      NEWPERFTICKCOUNTERS(_perf_class_preload_counters, SUN_CLS, "classPreload");
    }
  }
}

bool AOTLinkedClassBulkLoader::class_preloading_finished() {
  if (!CDSConfig::is_using_aot_linked_classes()) {
    return true;
  } else {
    // The ConstantPools of preloaded classes have references to other preloaded classes. We don't
    // want any Java code (including JVMCI compiler) to use these classes until all of them
    // are loaded.
    return Atomic::load_acquire(&_class_preloading_finished);
  }
}

void AOTLinkedClassBulkLoader::load_javabase_boot_classes(JavaThread* current) {
  load_impl(current, LoaderKind::BOOT, nullptr);
}

void AOTLinkedClassBulkLoader::load_non_javabase_boot_classes(JavaThread* current) {
  load_impl(current, LoaderKind::BOOT2, nullptr);
}

void AOTLinkedClassBulkLoader::load_platform_classes(JavaThread* current) {
  load_impl(current, LoaderKind::PLATFORM, SystemDictionary::java_platform_loader());
}

void AOTLinkedClassBulkLoader::load_app_classes(JavaThread* current) {
  load_impl(current, LoaderKind::APP, SystemDictionary::java_system_loader());

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

  Atomic::release_store(&_class_preloading_finished, true);
}

void AOTLinkedClassBulkLoader::load_impl(JavaThread* current, LoaderKind loader_kind, oop class_loader_oop) {
  if (!CDSConfig::is_using_aot_linked_classes()) {
    return;
  }

  HandleMark hm(current);
  ResourceMark rm(current);
  ExceptionMark em(current);

  Handle h_loader(current, class_loader_oop);

  load_table(AOTLinkedClassTable::for_static_archive(),  loader_kind, h_loader, current);
  assert(!current->has_pending_exception(), "VM should have exited due to ExceptionMark");

  load_table(AOTLinkedClassTable::for_dynamic_archive(), loader_kind, h_loader, current);
  assert(!current->has_pending_exception(), "VM should have exited due to ExceptionMark");

  if (loader_kind == LoaderKind::BOOT) {
    // Delayed until init_javabase_preloaded_classes
  } else {
    HeapShared::initialize_default_subgraph_classes(h_loader, current);
  }

  if (Universe::is_fully_initialized() && VerifyDuringStartup) {
    // Make sure we're still in a clean slate.
    VM_Verify verify_op;
    VMThread::execute(&verify_op);
  }
}

void AOTLinkedClassBulkLoader::load_table(AOTLinkedClassTable* table, LoaderKind loader_kind, Handle loader, TRAPS) {
  PerfTraceTime timer(_perf_class_preload_counters);

  if (loader_kind != LoaderKind::BOOT) {
    assert(Universe::is_module_initialized(), "sanity");
  }

  switch (loader_kind) {
  case LoaderKind::BOOT:
    load_classes(loader_kind, table->boot(), "boot ", loader, CHECK);
    break;

  case LoaderKind::BOOT2:
    load_classes(loader_kind, table->boot2(), "boot2", loader, CHECK);
    break;

  case LoaderKind::PLATFORM:
    {
      const char* category = "plat ";
      initiate_loading(THREAD, category, loader, table->boot());
      initiate_loading(THREAD, category, loader, table->boot2());

      load_classes(loader_kind, table->platform(), category, loader, CHECK);
    }
    break;
  case LoaderKind::APP:
    {
      const char* category = "app  ";
      initiate_loading(THREAD, category, loader, table->boot());
      initiate_loading(THREAD, category, loader, table->boot2());
      initiate_loading(THREAD, category, loader, table->platform());

      load_classes(loader_kind, table->app(), category, loader, CHECK);
    }
  }
}

void AOTLinkedClassBulkLoader::load_classes(LoaderKind loader_kind, Array<InstanceKlass*>* classes, const char* category, Handle loader, TRAPS) {
  if (classes == nullptr) {
    return;
  }

  ClassLoaderData* loader_data = ClassLoaderData::class_loader_data(loader());

  for (int i = 0; i < classes->length(); i++) {
    if (UsePerfData) {
      _perf_classes_preloaded->inc();
    }
    InstanceKlass* ik = classes->at(i);
    if (log_is_enabled(Info, cds, aot, load)) {
      ResourceMark rm;
      log_info(cds, aot, load)("%s %s%s%s", category, ik->external_name(),
                               ik->is_loaded() ? " (already loaded)" : "",
                               ik->is_hidden() ? " (hidden)" : "");
    }

    if (!ik->is_loaded()) {
      if (ik->is_hidden()) {
        load_hidden_class(loader_data, ik, CHECK);
      } else {
        InstanceKlass* actual;
        if (loader_data == ClassLoaderData::the_null_class_loader_data()) {
          if (!Universe::is_fully_initialized()) {
            load_class_quick(ik, loader_data, Handle(), CHECK);
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
  }


  if (loader_kind == LoaderKind::BOOT) {
    // Delayed until init_javabase_preloaded_classes
  } else {
    maybe_init_or_link(classes, CHECK);
  }
}

// Initiate loading of the <classes> in the <loader>. The <classes> should have already been loaded
// by a parent loader of the <loader>. This is necessary for handling pre-resolved CP entries.
//
// For example, we initiate the loading of java/lang/String in the AppClassLoader. This will allow
// any App classes to have a pre-resolved ConstantPool entry that references java/lang/String.
//
// TODO: we can limit the number of initiated classes to only those that are actually referenced by
// AOT-linked classes loaded by <loader>.
void AOTLinkedClassBulkLoader::initiate_loading(JavaThread* current, const char* category,
                                                Handle loader, Array<InstanceKlass*>* classes) {
  if (classes == nullptr) {
    return;
  }

  ClassLoaderData* loader_data = ClassLoaderData::class_loader_data(loader());
  MonitorLocker mu1(SystemDictionary_lock);

  for (int i = 0; i < classes->length(); i++) {
    InstanceKlass* ik = classes->at(i);
    assert(ik->is_loaded(), "must have already been loaded by a parent loader");
    if (ik->is_public() && !ik->is_hidden()) {
      if (log_is_enabled(Info, cds, aot, load)) {
        ResourceMark rm;
        const char* defining_loader = (ik->class_loader() == nullptr ? "boot" : "plat");
        log_info(cds, aot, load)("%s %s (initiated, defined by %s)", category, ik->external_name(),
                                 defining_loader);
      }
      SystemDictionary::add_to_initiating_loader(current, ik, loader_data);
    }
  }
}

// FIXME -- is this really correct? Do we need a special ClassLoaderData for each hidden class?
void AOTLinkedClassBulkLoader::load_hidden_class(ClassLoaderData* loader_data, InstanceKlass* ik, TRAPS) {
  DEBUG_ONLY({
      assert(ik->java_super()->is_loaded(), "must be");
      for (int i = 0; i < ik->local_interfaces()->length(); i++) {
        assert(ik->local_interfaces()->at(i)->is_loaded(), "must be");
      }
    });

  ik->restore_unshareable_info(loader_data, Handle(), NULL, CHECK);
  SystemDictionary::load_shared_class_misc(ik, loader_data);
  ik->add_to_hierarchy(THREAD);
  assert(ik->is_loaded(), "Must be in at least loaded state");
}

void AOTLinkedClassBulkLoader::load_class_quick(InstanceKlass* ik, ClassLoaderData* loader_data, Handle domain, TRAPS) {
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

  ik->restore_unshareable_info(loader_data, domain, nullptr, CHECK); // TODO: should we use ik->package()?
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

void AOTLinkedClassBulkLoader::jvmti_agent_error(InstanceKlass* expected, InstanceKlass* actual, const char* type) {
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
  log_error(cds)("JVMTI class retransformation is not supported when archive was generated with -XX:+AOTClassLinking.");
  MetaspaceShared::unrecoverable_loading_error();
}

void AOTLinkedClassBulkLoader::init_javabase_preloaded_classes(TRAPS) {
  maybe_init_or_link(AOTLinkedClassTable::for_static_archive()->boot(),  CHECK);
  //maybe_init_or_link(_dynamic_aot_loading_list._boot, CHECK); // TODO

  // Initialize java.base classes in the default subgraph.
  HeapShared::initialize_default_subgraph_classes(Handle(), CHECK);
}

void AOTLinkedClassBulkLoader::maybe_init_or_link(Array<InstanceKlass*>* classes, TRAPS) {
  if (classes != nullptr) {
    for (int i = 0; i < classes->length(); i++) {
      InstanceKlass* ik = classes->at(i);
      if (ik->has_preinitialized_mirror()) {
        ik->initialize_from_cds(CHECK);
      } else if (PrelinkSharedClasses && ik->verified_at_dump_time()) {
        ik->link_class(CHECK);
      }
    }
  }
}

void AOTLinkedClassBulkLoader::replay_training_at_init(Array<InstanceKlass*>* classes, TRAPS) {
  if (classes != nullptr) {
    for (int i = 0; i < classes->length(); i++) {
      InstanceKlass* ik = classes->at(i);
      if (ik->has_preinitialized_mirror() && ik->is_initialized() && !ik->has_init_deps_processed()) {
        CompilationPolicy::replay_training_at_init(ik, CHECK);
      }
    }
  }
}

void AOTLinkedClassBulkLoader::replay_training_at_init_for_preloaded_classes(TRAPS) {
  if (CDSConfig::is_using_aot_linked_classes() && TrainingData::have_data()) {
    AOTLinkedClassTable* table = AOTLinkedClassTable::for_static_archive(); // not applicable for dynamic archive (?? why??)
    replay_training_at_init(table->boot(),     CHECK);
    replay_training_at_init(table->boot2(),    CHECK);
    replay_training_at_init(table->platform(), CHECK);
    replay_training_at_init(table->app(),      CHECK);

    CompilationPolicy::replay_training_at_init(false, CHECK);
  }
}

void AOTLinkedClassBulkLoader::print_counters() {
  if (UsePerfData && _perf_class_preload_counters != nullptr) {
    LogStreamHandle(Info, init) log;
    if (log.is_enabled()) {
      log.print_cr("AOTLinkedClassBulkLoader:");
      log.print_cr("  preload:           " JLONG_FORMAT "ms (elapsed) " JLONG_FORMAT " (thread) / " JLONG_FORMAT " events",
                   _perf_class_preload_counters->elapsed_counter_value_ms(),
                   _perf_class_preload_counters->thread_counter_value_ms(),
                   _perf_classes_preloaded->get_value());
    }
  }
}
