/*
 * Copyright (c) 2022, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/classListWriter.hpp"
#include "cds/heapShared.hpp"
#include "cds/lambdaFormInvokers.inline.hpp"
#include "cds/regeneratedClasses.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/classLoaderExt.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "compiler/compilationPolicy.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "interpreter/bytecode.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/trainingData.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/perfData.hpp"
#include "runtime/timer.hpp"
#include "runtime/signature.hpp"
#include "runtime/vmOperations.hpp"
#include "services/management.hpp"

ClassPrelinker::ClassesTable* ClassPrelinker::_processed_classes = nullptr;
ClassPrelinker::ClassesTable* ClassPrelinker::_vm_classes = nullptr;
ClassPrelinker::ClassesTable* ClassPrelinker::_preloaded_classes = nullptr;
ClassPrelinker::ClassesTable* ClassPrelinker::_platform_initiated_classes = nullptr;
ClassPrelinker::ClassesTable* ClassPrelinker::_app_initiated_classes = nullptr;
int ClassPrelinker::_num_vm_klasses = 0;
bool ClassPrelinker::_record_javabase_only = true;
bool ClassPrelinker::_preload_javabase_only = true;
ClassPrelinker::PreloadedKlasses ClassPrelinker::_static_preloaded_klasses;
ClassPrelinker::PreloadedKlasses ClassPrelinker::_dynamic_preloaded_klasses;
Array<InstanceKlass*>* ClassPrelinker::_unregistered_klasses_from_preimage = nullptr;

static PerfCounter* _perf_classes_preloaded = nullptr;
static PerfTickCounters* _perf_class_preload_counters = nullptr;

bool ClassPrelinker::is_vm_class(InstanceKlass* ik) {
  return (_vm_classes->get(ik) != nullptr);
}

bool ClassPrelinker::is_preloaded_class(InstanceKlass* ik) {
  return (_preloaded_classes->get(ik) != nullptr);
}

void ClassPrelinker::add_one_vm_class(InstanceKlass* ik) {
  bool created;
  _preloaded_classes->put_if_absent(ik, &created);
  _vm_classes->put_if_absent(ik, &created);
  if (created) {
    ++ _num_vm_klasses;
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

void ClassPrelinker::initialize() {
  assert(_vm_classes == nullptr, "must be");
  _vm_classes = new (mtClass)ClassesTable();
  _preloaded_classes = new (mtClass)ClassesTable();
  _processed_classes = new (mtClass)ClassesTable();
  _platform_initiated_classes = new (mtClass)ClassesTable();
  _app_initiated_classes = new (mtClass)ClassesTable();
  for (auto id : EnumRange<vmClassID>{}) {
    add_one_vm_class(vmClasses::klass_at(id));
  }
  if (_static_preloaded_klasses._boot != nullptr && !CDSConfig::is_dumping_final_static_archive()) {
    assert(CDSConfig::is_dumping_dynamic_archive(), "must be");
    add_preloaded_klasses(_static_preloaded_klasses._boot);
    add_preloaded_klasses(_static_preloaded_klasses._boot2);
    add_preloaded_klasses(_static_preloaded_klasses._platform);
    add_preloaded_klasses(_static_preloaded_klasses._app);

    add_unrecorded_initiated_klasses(_platform_initiated_classes, _static_preloaded_klasses._platform_initiated);
    add_unrecorded_initiated_klasses(_app_initiated_classes, _static_preloaded_klasses._app_initiated);
  }

  // Record all the initiated classes that we used during dump time. This covers the verification constraints and
  // (resolved) class loader constraints.
  add_initiated_klasses_for_loader(ClassLoaderData::class_loader_data_or_null(SystemDictionary::java_platform_loader()),
                                   "platform", _platform_initiated_classes);
  add_initiated_klasses_for_loader(ClassLoaderData::class_loader_data_or_null(SystemDictionary::java_system_loader()),
                                   "app", _app_initiated_classes);
}

void ClassPrelinker::add_preloaded_klasses(Array<InstanceKlass*>* klasses) {
  for (int i = 0; i < klasses->length(); i++) {
    assert(klasses->at(i)->is_shared() && klasses->at(i)->is_loaded(), "must be");
    _preloaded_classes->put_when_absent(klasses->at(i), true);
  }
}

void ClassPrelinker::add_unrecorded_initiated_klasses(ClassesTable* table, Array<InstanceKlass*>* klasses) {
  // These initiated classes are already recorded in the static archive. There's no need to
  // record them again for the dynamic archive.
  assert(CDSConfig::is_dumping_dynamic_archive(), "must be");
  bool need_to_record = false;
  for (int i = 0; i < klasses->length(); i++) {
    InstanceKlass* ik = klasses->at(i);
    table->put_when_absent(ik, need_to_record);
  }
}

void ClassPrelinker::add_extra_initiated_klasses(PreloadedKlasses* table) {
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
          add_initiated_klass(_app_initiated_classes, "app", ik);
        }
      }
    }
  }
}

class ClassPrelinker::RecordInitiatedClassesClosure : public KlassClosure {
  ClassLoaderData* _loader_data;
  const char* _loader_name;
  ClassesTable* _table;
 public:
  RecordInitiatedClassesClosure(ClassLoaderData* loader_data, const char* loader_name, ClassesTable* table) :
    _loader_data(loader_data), _loader_name(loader_name), _table(table) {}
  virtual void do_klass(Klass* k) {
    if (k->is_instance_klass() && k->class_loader_data() != _loader_data) {
      add_initiated_klass(_table, _loader_name, InstanceKlass::cast(k));
    }
  }
};

void ClassPrelinker::add_initiated_klasses_for_loader(ClassLoaderData* loader_data, const char* loader_name, ClassesTable* table) {
  if (loader_data != nullptr) {
    MonitorLocker mu1(SystemDictionary_lock);
    RecordInitiatedClassesClosure mk(loader_data, loader_name, table);  
    loader_data->dictionary()->all_entries_do(&mk);
  }
}

// ik has a reference to target:
//    - target is a declared supertype of ik, or
//    - one of the constant pool entries in ik references target
void ClassPrelinker::add_initiated_klass(InstanceKlass* ik, InstanceKlass* target) {
  if (ik->shared_class_loader_type() == target->shared_class_loader_type()) {
    return;
  }

  if (SystemDictionary::is_platform_class_loader(ik->class_loader())) {
    add_initiated_klass(_platform_initiated_classes, "platform", target);
  } else {
    assert(SystemDictionary::is_system_class_loader(ik->class_loader()), "must be");
    add_initiated_klass(_app_initiated_classes, "app", target);
  }
}

void ClassPrelinker::add_initiated_klass(ClassesTable* initiated_classes, const char* loader_name, InstanceKlass* target) {
  bool need_to_record = true;
  bool created;
  initiated_classes->put_if_absent(target, need_to_record, &created);
  if (created && log_is_enabled(Trace, cds, resolve)) {
    ResourceMark rm;
    log_trace(cds, resolve)("%s loader initiated %s", loader_name, target->external_name());
  }
}

void ClassPrelinker::dispose() {
  assert(_vm_classes != nullptr, "must be");
  delete _vm_classes;
  delete _processed_classes;
  delete _platform_initiated_classes;
  delete _app_initiated_classes;
  _vm_classes = nullptr;
  _processed_classes = nullptr;
  _platform_initiated_classes = nullptr;
  _app_initiated_classes = nullptr;
}

bool ClassPrelinker::can_archive_resolved_klass(ConstantPool* cp, int cp_index) {
  assert(!is_in_archivebuilder_buffer(cp), "sanity");
  assert(cp->tag_at(cp_index).is_klass(), "must be resolved");

  Klass* resolved_klass = cp->resolved_klass_at(cp_index);
  assert(resolved_klass != nullptr, "must be");

  return can_archive_resolved_klass(cp->pool_holder(), resolved_klass);
}

bool ClassPrelinker::can_archive_resolved_klass(InstanceKlass* cp_holder, Klass* resolved_klass) {
  assert(!is_in_archivebuilder_buffer(cp_holder), "sanity");
  assert(!is_in_archivebuilder_buffer(resolved_klass), "sanity");

#if 0
  if (cp_holder->is_hidden()) {
    // TODO - what is needed for hidden classes?
    return false;
  }
#endif

#if 0
  if (DumpSharedSpaces && LambdaFormInvokers::may_be_regenerated_class(resolved_klass->name())) {
    // Hack -- there's a copy of the regenerated class in both dynamic and static archive.
    // When dynamic archive is loaded, we don't want pre-resolved CP entries in the static
    // archive to point to the wrong class.
    return false;
  }
#endif

  if (resolved_klass->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(resolved_klass);

    if (cp_holder->is_subtype_of(ik)) {
      // All super types of ik will be resolved in ik->class_loader() before
      // ik is defined in this loader, so it's safe to archive the resolved klass reference.
      return true;
    }

    if (is_vm_class(cp_holder)) {
      return is_vm_class(ik);
    } else if (is_preloaded_class(ik)) {
      if (cp_holder->is_shared_platform_class()) {
        add_initiated_klass(cp_holder, ik);
        return true;
      } else if (cp_holder->is_shared_app_class()) {
        add_initiated_klass(cp_holder, ik);
        return true;
      } else if (cp_holder->is_shared_boot_class()) {
        assert(ik->class_loader() == nullptr, "a boot class can reference only boot classes");
        return true;
      } else if (cp_holder->is_hidden() && cp_holder->class_loader() == nullptr) { // FIXME -- use better checks!
        return true;
      }
    }

    // TODO -- allow objArray classes, too
  }

  return false;
}

Klass* ClassPrelinker::get_fmi_ref_resolved_archivable_klass(ConstantPool* cp, int cp_index) {
  assert(!is_in_archivebuilder_buffer(cp), "sanity");

  int klass_cp_index = cp->uncached_klass_ref_index_at(cp_index);
  if (!cp->tag_at(klass_cp_index).is_klass()) {
    // Not yet resolved
    return nullptr;
  }
  Klass* k = cp->resolved_klass_at(klass_cp_index);
  if (!can_archive_resolved_klass(cp->pool_holder(), k)) {
    // When we access this field at runtime, the target klass may
    // have a different definition.
    return nullptr;
  }
  return k;
}

bool ClassPrelinker::can_archive_resolved_method(ConstantPool* cp, int cp_index) {
  assert(cp->tag_at(cp_index).is_method() ||
         cp->tag_at(cp_index).is_interface_method(), "must be");
  return get_fmi_ref_resolved_archivable_klass(cp, cp_index) != nullptr;
}

bool ClassPrelinker::can_archive_resolved_field(ConstantPool* cp, int cp_index) {
  assert(cp->tag_at(cp_index).is_field(), "must be");

  Klass* k = get_fmi_ref_resolved_archivable_klass(cp, cp_index);
  if (k == nullptr) {
    return false;
  }

  Symbol* field_name = cp->uncached_name_ref_at(cp_index);
  Symbol* field_sig = cp->uncached_signature_ref_at(cp_index);
  fieldDescriptor fd;
  if (k->find_field(field_name, field_sig, &fd) == NULL) {
    return false;
  }

  return true;
}

void ClassPrelinker::dumptime_resolve_constants(InstanceKlass* ik, TRAPS) {
  if (!ik->is_linked()) {
    return;
  }
  bool first_time;
  _processed_classes->put_if_absent(ik, &first_time);
  if (!first_time) {
    // We have already resolved the constants in class, so no need to do it again.
    return;
  }

  constantPoolHandle cp(THREAD, ik->constants());
  for (int cp_index = 1; cp_index < cp->length(); cp_index++) { // Index 0 is unused
    if (cp->tag_at(cp_index).value() == JVM_CONSTANT_String) {
      resolve_string(cp, cp_index, CHECK); // may throw OOM when interning strings.
    }
  }

  // Normally, we don't want to archive any CP entries that were not resolved
  // in the training run. Otherwise the AOT/JIT may inline too much code that has not
  // been executed.
  //
  // However, we want to aggressively resolve all klass/field/method constants for
  // LambdaForm Invoker Holder classes, Lambda Proxy classes, and LambdaForm classes,
  // so that the compiler can inline through them.
  if (SystemDictionaryShared::is_builtin_loader(ik->class_loader_data())) {
    bool eager_resolve = false;

    if (LambdaFormInvokers::may_be_regenerated_class(ik->name())) {
      eager_resolve = true;
    }
    if (ik->is_hidden() && HeapShared::is_archivable_hidden_klass(ik)) {
      eager_resolve = true;
    }

    if (eager_resolve) {
      preresolve_class_cp_entries(THREAD, ik, nullptr);
      preresolve_field_and_method_cp_entries(THREAD, ik, nullptr);
    }
  }
}

// This works only for the boot/platform/app loaders
Klass* ClassPrelinker::find_loaded_class(Thread* current, oop class_loader, Symbol* name) {
  HandleMark hm(current);
  Handle h_loader(current, class_loader);
  Klass* k = SystemDictionary::find_instance_or_array_klass(current, name,
                                                            h_loader,
                                                            Handle());
  if (k != nullptr) {
    return k;
  }
  if (h_loader() == SystemDictionary::java_system_loader()) {
    return find_loaded_class(current, SystemDictionary::java_platform_loader(), name);
  } else if (h_loader() == SystemDictionary::java_platform_loader()) {
    return find_loaded_class(current, nullptr, name);
  } else {
    assert(h_loader() == nullptr, "This function only works for boot/platform/app loaders %p %p %p",
           cast_from_oop<address>(h_loader()),
           cast_from_oop<address>(SystemDictionary::java_system_loader()),
           cast_from_oop<address>(SystemDictionary::java_platform_loader()));
  }

  return nullptr;
}

Klass* ClassPrelinker::find_loaded_class(Thread* current, ConstantPool* cp, int class_cp_index) {
  Symbol* name = cp->klass_name_at(class_cp_index);
  return find_loaded_class(current, cp->pool_holder()->class_loader(), name);
}

#if INCLUDE_CDS_JAVA_HEAP
void ClassPrelinker::resolve_string(constantPoolHandle cp, int cp_index, TRAPS) {
  if (CDSConfig::is_dumping_heap()) {
    int cache_index = cp->cp_to_object_index(cp_index);
    ConstantPool::string_at_impl(cp, cp_index, cache_index, CHECK);
  }
}
#endif

void ClassPrelinker::preresolve_class_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list) {
  if (!PreloadSharedClasses) {
    return;
  }
  if (!SystemDictionaryShared::is_builtin_loader(ik->class_loader_data())) {
    return;
  }

  JavaThread* THREAD = current;
  constantPoolHandle cp(THREAD, ik->constants());
  for (int cp_index = 1; cp_index < cp->length(); cp_index++) {
    if (cp->tag_at(cp_index).value() == JVM_CONSTANT_UnresolvedClass) {
      if (preresolve_list != nullptr && preresolve_list->at(cp_index) == false) {
        // This class was not resolved during trial run. Don't attempt to resolve it. Otherwise
        // the compiler may generate less efficient code.
        continue;
      }
      if (find_loaded_class(current, cp(), cp_index) == nullptr) {
        // Do not resolve any class that has not been loaded yet
        continue;
      }
      Klass* resolved_klass = cp->klass_at(cp_index, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        CLEAR_PENDING_EXCEPTION; // just ignore
      } else {
        log_trace(cds, resolve)("Resolved class  [%3d] %s -> %s", cp_index, ik->external_name(),
                                resolved_klass->external_name());
      }
    }
  }
}

void ClassPrelinker::preresolve_field_and_method_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list) {
  JavaThread* THREAD = current;
  constantPoolHandle cp(THREAD, ik->constants());
  if (cp->cache() == nullptr) {
    return;
  }
  for (int i = 0; i < ik->methods()->length(); i++) {
    Method* m = ik->methods()->at(i);
    BytecodeStream bcs(methodHandle(THREAD, m));
    while (!bcs.is_last_bytecode()) {
      bcs.next();
      Bytecodes::Code raw_bc = bcs.raw_code();
      switch (raw_bc) {
      case Bytecodes::_getstatic:
      case Bytecodes::_putstatic:
      case Bytecodes::_getfield:
      case Bytecodes::_putfield:
        maybe_resolve_fmi_ref(ik, m, raw_bc, bcs.get_index_u2(), preresolve_list, THREAD);
        if (HAS_PENDING_EXCEPTION) {
          CLEAR_PENDING_EXCEPTION; // just ignore
        }
        break;
      case Bytecodes::_invokehandle:
      case Bytecodes::_invokespecial:
      case Bytecodes::_invokevirtual:
      case Bytecodes::_invokeinterface:
      case Bytecodes::_invokestatic:
        maybe_resolve_fmi_ref(ik, m, raw_bc, bcs.get_index_u2(), preresolve_list, THREAD);
        if (HAS_PENDING_EXCEPTION) {
          CLEAR_PENDING_EXCEPTION; // just ignore
        }
        break;
      default:
        break;
      }
    }
  }
}

void ClassPrelinker::maybe_resolve_fmi_ref(InstanceKlass* ik, Method* m, Bytecodes::Code bc, int raw_index,
                                           GrowableArray<bool>* preresolve_list, TRAPS) {
  methodHandle mh(THREAD, m);
  constantPoolHandle cp(THREAD, ik->constants());
  HandleMark hm(THREAD);
  int cp_index;

  if (bc == Bytecodes::_invokehandle  ||
      bc == Bytecodes::_invokestatic  ||
      bc == Bytecodes::_invokespecial ||
      bc == Bytecodes::_invokevirtual ||
      bc == Bytecodes::_invokeinterface) {
    ResolvedMethodEntry* method_entry = cp->cache()->resolved_method_entry_at(raw_index);
    if (method_entry->is_resolved(bc)) {
      return;
    }
    cp_index = method_entry->constant_pool_index();
  } else {
    assert(bc == Bytecodes::_getfield  || bc == Bytecodes::_putfield ||
           bc == Bytecodes::_getstatic || bc == Bytecodes::_putstatic, "must be");
    cp_index = cp->cache()->resolved_field_entry_at(raw_index)->constant_pool_index();
  }

  if (preresolve_list != nullptr && preresolve_list->at(cp_index) == false) {
    // This field wasn't resolved during the trial run. Don't attempt to resolve it. Otherwise
    // the compiler may generate less efficient code.
    return;
  }

  int klass_cp_index = cp->uncached_klass_ref_index_at(cp_index);
  if (find_loaded_class(THREAD, cp(), klass_cp_index) == nullptr) {
    // Do not resolve any field/methods from a class that has not been loaded yet.
    return;
  }
  Klass* resolved_klass = cp->klass_ref_at(raw_index, bc, CHECK);

  const char* ref_kind = "";
  const char* is_static = "";
  const char* is_regen = "";

  if (RegeneratedClasses::is_a_regenerated_object((address)ik)) {
    is_regen = " (regenerated)";
  }

  switch (bc) {
  case Bytecodes::_getstatic:
  case Bytecodes::_putstatic:
    if (!VM_Version::supports_fast_class_init_checks()) {
      return; // Do not resolve since interpreter lacks fast clinit barriers support
    }
    InterpreterRuntime::resolve_get_put(bc, raw_index, mh, cp, false /*initialize_holder*/, CHECK);
    ref_kind = "field ";
    is_static = " *** static";
    break;
  case Bytecodes::_getfield:
  case Bytecodes::_putfield:
    InterpreterRuntime::resolve_get_put(bc, raw_index, mh, cp, false /*initialize_holder*/, CHECK);
    ref_kind = "field ";
    break;

  case Bytecodes::_invokestatic:
    if (!VM_Version::supports_fast_class_init_checks()) {
      return; // Do not resolve since interpreter lacks fast clinit barriers support
    }
    InterpreterRuntime::cds_resolve_invoke(bc, raw_index, mh, cp, CHECK);
    ref_kind = "method";
    is_static = " *** static";
    break;

  case Bytecodes::_invokevirtual:
  case Bytecodes::_invokespecial:
    InterpreterRuntime::cds_resolve_invoke(bc, raw_index, mh, cp, CHECK);
    ref_kind = "method";
    break;

  case Bytecodes::_invokeinterface:
    InterpreterRuntime::cds_resolve_invoke(bc, raw_index, mh, cp, CHECK);
    ref_kind = "interface method";
    break;

  case Bytecodes::_invokehandle:
    InterpreterRuntime::cds_resolve_invokehandle(raw_index, cp, CHECK);
    ref_kind = "method";
    break;
    break;

  default:
    ShouldNotReachHere();
  }

  if (log_is_enabled(Trace, cds, resolve)) {
    ResourceMark rm(THREAD);
    Symbol* name = cp->name_ref_at(raw_index, bc);
    Symbol* signature = cp->signature_ref_at(raw_index, bc);
    log_trace(cds, resolve)("Resolved %s [%3d] %s%s -> %s.%s:%s%s", ref_kind, cp_index,
                            ik->external_name(), is_regen,
                            resolved_klass->external_name(),
                            name->as_C_string(), signature->as_C_string(), is_static);
  }
}

void ClassPrelinker::preresolve_indy_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list) {
  JavaThread* THREAD = current;
  constantPoolHandle cp(THREAD, ik->constants());
  if (!ArchiveInvokeDynamic || cp->cache() == nullptr) {
    return;
  }

  assert(preresolve_list != nullptr, "preresolve_indy_cp_entries() should not be called for "
         "regenerated LambdaForm Invoker classes, which should not have indys anyway.");

  Array<ResolvedIndyEntry>* indy_entries = cp->cache()->resolved_indy_entries();
  for (int i = 0; i < indy_entries->length(); i++) {
    ResolvedIndyEntry* rie = indy_entries->adr_at(i);
    int cp_index = rie->constant_pool_index();
    if (preresolve_list->at(cp_index) == true && !rie->is_resolved() && is_indy_archivable(cp(), cp_index)) {
      InterpreterRuntime::cds_resolve_invokedynamic(ConstantPool::encode_invokedynamic_index(i), cp, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        CLEAR_PENDING_EXCEPTION; // just ignore
      }
    }
  }
}

static GrowableArrayCHeap<char*, mtClassShared>* _invokedynamic_filter = nullptr;

static bool has_clinit(InstanceKlass* ik) {
  if (ik->class_initializer() != nullptr) {
    return true;
  }
  InstanceKlass* super = ik->java_super();
  if (super != nullptr && has_clinit(super)) {
    return true;
  }
  Array<InstanceKlass*>* interfaces = ik->local_interfaces();
  int num_interfaces = interfaces->length();
  for (int index = 0; index < num_interfaces; index++) {
    InstanceKlass* intf = interfaces->at(index);
    if (has_clinit(intf)) {
      return true;
    }
  }
  return false;
}

bool ClassPrelinker::is_indy_archivable(ConstantPool* cp, int cp_index) {
  if (!ArchiveInvokeDynamic || !HeapShared::can_write()) {
    return false;
  }

  if (!SystemDictionaryShared::is_builtin(cp->pool_holder())) {
    return false;
  }

  int bsm = cp->bootstrap_method_ref_index_at(cp_index);
  int bsm_ref = cp->method_handle_index_at(bsm);
  Symbol* bsm_name = cp->uncached_name_ref_at(bsm_ref);
  Symbol* bsm_signature = cp->uncached_signature_ref_at(bsm_ref);
  Symbol* bsm_klass = cp->klass_name_at(cp->uncached_klass_ref_index_at(bsm_ref));

  // We currently support only string concat and LambdaMetafactory::metafactory()

  if (bsm_klass->equals("java/lang/invoke/StringConcatFactory") &&
      bsm_name->equals("makeConcatWithConstants")) {
    return true;
  }

  if (bsm_klass->equals("java/lang/invoke/LambdaMetafactory") &&
      ((bsm_name->equals("metafactory")    && bsm_signature->equals("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;")) ||
       (bsm_name->equals("altMetafactory") && bsm_signature->equals("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;")))) {
    SignatureStream ss(cp->uncached_signature_ref_at(cp_index));
    ss.skip_to_return_type();
    Symbol* type = ss.as_symbol(); // This is the interface type implemented by the lambda proxy
    InstanceKlass* holder = cp->pool_holder();
    Klass* k = find_loaded_class(Thread::current(), holder->class_loader(), type);
    if (k == nullptr) {
      return false;
    }
    if (!k->is_interface()) {
      // Might be a class not generated by javac
      return false;
    }

    if (has_clinit(InstanceKlass::cast(k))) {
      // We initialize the class of the archived lambda proxy at VM start-up, which will also initialize
      // the interface that it implements. If that interface has a clinit method, we can potentially
      // change program execution order. See test/hotspot/jtreg/runtime/cds/appcds/indy/IndyMiscTests.java
      if (log_is_enabled(Debug, cds, resolve)) {
        ResourceMark rm;
        log_debug(cds, resolve)("Cannot resolve Lambda proxy of interface type %s", k->external_name());
      }
      return false;
    }

    return true;
  }

  return false;
}

#ifdef ASSERT
bool ClassPrelinker::is_in_archivebuilder_buffer(address p) {
  if (!Thread::current()->is_VM_thread() || ArchiveBuilder::current() == nullptr) {
    return false;
  } else {
    return ArchiveBuilder::current()->is_in_buffer_space(p);
  }
}
#endif

bool ClassPrelinker::is_in_javabase(InstanceKlass* ik) {
  if (ik->is_hidden() && HeapShared::is_lambda_form_klass(ik)) {
    return true;
  }

  return (ik->module() != nullptr &&
          ik->module()->name() != nullptr &&
          ik->module()->name()->equals("java.base"));
}

class ClassPrelinker::PreloadedKlassRecorder : StackObj {
  int _loader_type;
  ResourceHashtable<InstanceKlass*, bool, 15889, AnyObj::RESOURCE_AREA, mtClassShared> _seen_klasses;
  GrowableArray<InstanceKlass*> _list;
  bool loader_type_matches(InstanceKlass* ik) {
    InstanceKlass* buffered_ik = ArchiveBuilder::current()->get_buffered_addr(ik);
    return buffered_ik->shared_class_loader_type() == _loader_type;
  }

  void maybe_record(InstanceKlass* ik) {
    bool created;
    _seen_klasses.put_if_absent(ik, true, &created);
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
      assert(HeapShared::is_lambda_form_klass(ik) || HeapShared::is_lambda_proxy_klass(ik), "must be");
    }

    if (ClassPrelinker::is_vm_class(ik)) {
      // vmClasses are loaded in vmClasses::resolve_all() at the very beginning
      // of VM bootstrap, before ClassPrelinker::runtime_preload() is called.
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
      add_initiated_klass(ik, s);
    }

    Array<InstanceKlass*>* interfaces = ik->local_interfaces();
    int num_interfaces = interfaces->length();
    for (int index = 0; index < num_interfaces; index++) {
      InstanceKlass* intf = interfaces->at(index);
      maybe_record(intf);
      add_initiated_klass(ik, intf);
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
  PreloadedKlassRecorder(int loader_type) : _loader_type(loader_type),  _seen_klasses(), _list() {}

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

Array<InstanceKlass*>* ClassPrelinker::record_preloaded_klasses(int loader_type) {
  ResourceMark rm;
  PreloadedKlassRecorder recorder(loader_type);
  recorder.iterate();
  return recorder.to_array();
}

void ClassPrelinker::record_preloaded_klasses(bool is_static_archive) {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_klasses : &_dynamic_preloaded_klasses;

    _record_javabase_only = true;
    table->_boot     = record_preloaded_klasses(ClassLoader::BOOT_LOADER);
    _record_javabase_only = false;
    table->_boot2    = record_preloaded_klasses(ClassLoader::BOOT_LOADER);

    table->_platform = record_preloaded_klasses(ClassLoader::PLATFORM_LOADER);
    table->_app      = record_preloaded_klasses(ClassLoader::APP_LOADER);

    add_extra_initiated_klasses(table);
  }
}

Array<InstanceKlass*>* ClassPrelinker::record_initiated_klasses(ClassesTable* table) {
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

void ClassPrelinker::record_initiated_klasses(bool is_static_archive) {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_klasses : &_dynamic_preloaded_klasses;
    table->_platform_initiated = record_initiated_klasses(_platform_initiated_classes);
    table->_app_initiated = record_initiated_klasses(_app_initiated_classes);
  }
}

void ClassPrelinker::record_unregistered_klasses() {
  if (CDSConfig::is_dumping_preimage_static_archive()) {
    GrowableArray<InstanceKlass*> unreg_klasses;
    GrowableArray<Klass*>* klasses = ArchiveBuilder::current()->klasses();
    for (int i = 0; i < klasses->length(); i++) {
      Klass* k = klasses->at(i);
      if (k->is_instance_klass()) {
        InstanceKlass* ik = InstanceKlass::cast(k);
        if (ik->is_shared_unregistered_class()) {
          unreg_klasses.append((InstanceKlass*)ArchiveBuilder::get_buffered_klass(ik));
        }
      }
    }
    _unregistered_klasses_from_preimage = ArchiveUtils::archive_array(&unreg_klasses);
  } else {
    _unregistered_klasses_from_preimage = nullptr;
  }
}

// This class is used only by the "one step training workflow". An instance of this
// this class is stored in the pre-image. It contains information about the
// class metadata that can be eagerly linked inside the final-image.
class FinalImageEagerLinkage {
  // The klasses who have resolved at least one indy CP entry during the training run.
  // _indy_cp_indices[i] is a list of all resolved CP entries for _indy_klasses[i].
  Array<InstanceKlass*>* _indy_klasses;
  Array<Array<int>*>*    _indy_cp_indices;

  // The RefectionData for  _reflect_klasses[i] should be initialized with _reflect_flags[i]
  Array<InstanceKlass*>* _reflect_klasses;
  Array<int>*            _reflect_flags;

  static GrowableArray<InstanceKlass*>* _tmp_reflect_klasses;
  static GrowableArray<int>* _tmp_reflect_flags;

  struct TmpDynamicProxyClassInfo {
    int _loader_type;
    int _access_flags;
    const char* _proxy_name;
    GrowableArray<Klass*>* _interfaces;
  };

  struct DynamicProxyClassInfo {
    int _loader_type;
    int _access_flags;
    const char* _proxy_name;
    Array<Klass*>* _interfaces;
  };

  Array<DynamicProxyClassInfo>* _dynamic_proxy_classes;

  static GrowableArray<TmpDynamicProxyClassInfo>* _tmp_dynamic_proxy_classes;

public:
  FinalImageEagerLinkage() : _indy_klasses(nullptr), _indy_cp_indices(nullptr),
                             _reflect_klasses(nullptr), _reflect_flags(nullptr),
                             _dynamic_proxy_classes(nullptr) {}

  void* operator new(size_t size) throw() {
    return ArchiveBuilder::current()->ro_region_alloc(size);
  }

  // These are called when dumping preimage
  static void record_reflection_data_flags_for_preimage(InstanceKlass* ik, TRAPS);
  static void record_dynamic_proxy_class(oop loader, const char* proxy_name, objArrayOop interfaces, int access_flags);
  void record_linkage_in_preimage();

  // Called when dumping final image
  void resolve_indys_in_final_image(TRAPS);
  void archive_reflection_data_in_final_image(JavaThread* current);
  void archive_dynamic_proxies(TRAPS);
};

static FinalImageEagerLinkage* _final_image_eager_linkage = nullptr;

GrowableArray<InstanceKlass*>* FinalImageEagerLinkage::_tmp_reflect_klasses = nullptr;
GrowableArray<int>* FinalImageEagerLinkage::_tmp_reflect_flags = nullptr;
GrowableArray<FinalImageEagerLinkage::TmpDynamicProxyClassInfo>* FinalImageEagerLinkage::_tmp_dynamic_proxy_classes = nullptr;

void FinalImageEagerLinkage::record_reflection_data_flags_for_preimage(InstanceKlass* ik, TRAPS) {
  assert(CDSConfig::is_dumping_preimage_static_archive(), "must be");
  if (SystemDictionaryShared::is_builtin_loader(ik->class_loader_data()) && !ik->is_hidden() &&
      java_lang_Class::has_reflection_data(ik->java_mirror())) {
    int rd_flags = ClassPrelinker::class_reflection_data_flags(ik, CHECK);
    if (_tmp_reflect_klasses == nullptr) {
      _tmp_reflect_klasses = new (mtClassShared) GrowableArray<InstanceKlass*>(100, mtClassShared);
      _tmp_reflect_flags = new (mtClassShared) GrowableArray<int>(100, mtClassShared);
    }
    _tmp_reflect_klasses->append(ik);
    _tmp_reflect_flags->append(rd_flags);
  }
}

void FinalImageEagerLinkage::record_linkage_in_preimage() {
  assert(CDSConfig::is_dumping_preimage_static_archive(), "must be");
  ResourceMark rm;
  GrowableArray<Klass*>* klasses = ArchiveBuilder::current()->klasses();

  // ArchiveInvokeDynamic
  GrowableArray<InstanceKlass*> tmp_indy_klasses;
  GrowableArray<Array<int>*> tmp_indy_cp_indices;
  int total_indys_to_resolve = 0;
  for (int i = 0; i < klasses->length(); i++) {
    Klass* k = klasses->at(i);
    if (k->is_instance_klass()) {
      InstanceKlass* ik = InstanceKlass::cast(k);
      GrowableArray<int> indices;

      if (ik->constants()->cache() != nullptr) {
        Array<ResolvedIndyEntry>* tmp_indy_entries = ik->constants()->cache()->resolved_indy_entries();
        if (tmp_indy_entries != nullptr) {
          for (int i = 0; i < tmp_indy_entries->length(); i++) {
            ResolvedIndyEntry* rie = tmp_indy_entries->adr_at(i);
            int cp_index = rie->constant_pool_index();
            if (rie->is_resolved()) {
              indices.append(cp_index);
            }
          }
        }
      }

      if (indices.length() > 0) {
        tmp_indy_klasses.append(ArchiveBuilder::current()->get_buffered_addr(ik));
        tmp_indy_cp_indices.append(ArchiveUtils::archive_array(&indices));
        total_indys_to_resolve += indices.length();
      }
    }
  }

  assert(tmp_indy_klasses.length() == tmp_indy_cp_indices.length(), "must be");
  if (tmp_indy_klasses.length() > 0) {
    _indy_klasses = ArchiveUtils::archive_array(&tmp_indy_klasses);
    _indy_cp_indices = ArchiveUtils::archive_array(&tmp_indy_cp_indices);

    ArchivePtrMarker::mark_pointer(&_indy_klasses);
    ArchivePtrMarker::mark_pointer(&_indy_cp_indices);
  }
  log_info(cds)("%d indies in %d classes will be resolved in final CDS image", total_indys_to_resolve, tmp_indy_klasses.length());

  // ArchiveReflectionData
  int reflect_count = 0;
  if (_tmp_reflect_klasses != nullptr) {
    for (int i = _tmp_reflect_klasses->length() - 1; i >= 0; i--) {
      InstanceKlass* ik = _tmp_reflect_klasses->at(i);
      if (SystemDictionaryShared::is_excluded_class(ik)) {
        _tmp_reflect_klasses->remove_at(i);
        _tmp_reflect_flags->remove_at(i);
      } else {
        _tmp_reflect_klasses->at_put(i, ArchiveBuilder::current()->get_buffered_addr(ik));
      }
    }
    if (_tmp_reflect_klasses->length() > 0) {
      _reflect_klasses = ArchiveUtils::archive_array(_tmp_reflect_klasses);
      _reflect_flags = ArchiveUtils::archive_array(_tmp_reflect_flags);

      ArchivePtrMarker::mark_pointer(&_reflect_klasses);
      ArchivePtrMarker::mark_pointer(&_reflect_flags);
      reflect_count = _tmp_reflect_klasses->length();
    }
  }
  log_info(cds)("ReflectionData of %d classes will be archived in final CDS image", reflect_count);

  // Dynamic Proxies
  if (_tmp_dynamic_proxy_classes != nullptr && ArchiveDynamicProxies) {
    int len = _tmp_dynamic_proxy_classes->length();
    _dynamic_proxy_classes = ArchiveBuilder::new_ro_array<DynamicProxyClassInfo>(len);
    ArchivePtrMarker::mark_pointer(&_dynamic_proxy_classes);
    for (int i = 0; i < len; i++) {
      TmpDynamicProxyClassInfo* tmp_info = _tmp_dynamic_proxy_classes->adr_at(i);
      DynamicProxyClassInfo* info = _dynamic_proxy_classes->adr_at(i);
      info->_loader_type = tmp_info->_loader_type;
      info->_access_flags = tmp_info->_access_flags;
      info->_proxy_name = ArchiveBuilder::current()->ro_strdup(tmp_info->_proxy_name);

      ResourceMark rm;
      GrowableArray<Klass*> buffered_interfaces;
      for (int j = 0; j < tmp_info->_interfaces->length(); j++) {
        buffered_interfaces.append(ArchiveBuilder::current()->get_buffered_addr(tmp_info->_interfaces->at(j)));
      }
      info->_interfaces = ArchiveUtils::archive_array(&buffered_interfaces);

      ArchivePtrMarker::mark_pointer(&info->_proxy_name);
      ArchivePtrMarker::mark_pointer(&info->_interfaces);
      ArchiveBuilder::alloc_stats()->record_dynamic_proxy_class();
    }
  }
}

void FinalImageEagerLinkage::resolve_indys_in_final_image(TRAPS) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");

  if (_indy_klasses != nullptr) {
    assert(_indy_cp_indices != nullptr, "must be");
    for (int i = 0; i < _indy_klasses->length(); i++) {
      InstanceKlass* ik = _indy_klasses->at(i);
      ConstantPool* cp = ik->constants();
      Array<int>* cp_indices = _indy_cp_indices->at(i);
      GrowableArray<bool> preresolve_list(cp->length(), cp->length(), false);
      for (int j = 0; j < cp_indices->length(); j++) {
        preresolve_list.at_put(cp_indices->at(j), true);
      }
      ClassPrelinker::preresolve_indy_cp_entries(THREAD, ik, &preresolve_list);
    }
  }
}

void FinalImageEagerLinkage::archive_reflection_data_in_final_image(JavaThread* current) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");

  if (_reflect_klasses != nullptr) {
    assert(_reflect_flags != nullptr, "must be");
    for (int i = 0; i < _reflect_klasses->length(); i++) {
      InstanceKlass* ik = _reflect_klasses->at(i);
      int rd_flags = _reflect_flags->at(i);
      ClassPrelinker::generate_reflection_data(current, ik, rd_flags);
    }
  }
}

void FinalImageEagerLinkage::record_dynamic_proxy_class(oop loader, const char* proxy_name, objArrayOop interfaces, int access_flags) {
  int loader_type;
  if (loader == nullptr) {
    loader_type = ClassLoader::BOOT_LOADER;
  } else if (loader == SystemDictionary::java_platform_loader()) {
    loader_type = ClassLoader::PLATFORM_LOADER;
  } else if (loader == SystemDictionary::java_system_loader()) {
    loader_type = ClassLoader::APP_LOADER;
  } else {
    return;
  }

  if (_tmp_dynamic_proxy_classes == nullptr) {
    _tmp_dynamic_proxy_classes = new (mtClassShared) GrowableArray<TmpDynamicProxyClassInfo>(32, mtClassShared);
  }

  TmpDynamicProxyClassInfo info;
  info._loader_type = loader_type;
  info._access_flags = access_flags;
  info._proxy_name = os::strdup(proxy_name);
  info._interfaces = new (mtClassShared) GrowableArray<Klass*>(interfaces->length(), mtClassShared);
  for (int i = 0; i < interfaces->length(); i++) {
    Klass* intf = java_lang_Class::as_Klass(interfaces->obj_at(i));
    info._interfaces->append(intf);
  }
  _tmp_dynamic_proxy_classes->append(info);
}

void FinalImageEagerLinkage::archive_dynamic_proxies(TRAPS) {
  if (ArchiveDynamicProxies && _dynamic_proxy_classes != nullptr) {
    for (int proxy_index = 0; proxy_index < _dynamic_proxy_classes->length(); proxy_index++) {
      DynamicProxyClassInfo* info = _dynamic_proxy_classes->adr_at(proxy_index);

      Handle loader(THREAD, ArchiveUtils::builtin_loader_from_type(info->_loader_type));

      oop proxy_name_oop = java_lang_String::create_oop_from_str(info->_proxy_name, CHECK);
      Handle proxy_name(THREAD, proxy_name_oop);

      int num_intfs = info->_interfaces->length();
      objArrayOop interfaces_oop = oopFactory::new_objArray(vmClasses::Class_klass(), num_intfs, CHECK);
      objArrayHandle interfaces(THREAD, interfaces_oop);
      for (int intf_index = 0; intf_index < num_intfs; intf_index++) {
        Klass* k = info->_interfaces->at(intf_index);
        assert(k->java_mirror() != nullptr, "must be loaded");
        interfaces()->obj_at_put(intf_index, k->java_mirror());
      }

      ClassPrelinker::define_dynamic_proxy_class(loader, proxy_name, interfaces, info->_access_flags, CHECK);
    }
  }
}

void ClassPrelinker::record_reflection_data_flags_for_preimage(InstanceKlass* ik, TRAPS) {
  FinalImageEagerLinkage::record_reflection_data_flags_for_preimage(ik, THREAD);
}

void ClassPrelinker::record_final_image_eager_linkage() {
  _final_image_eager_linkage = new FinalImageEagerLinkage();
  _final_image_eager_linkage->record_linkage_in_preimage();
}

void ClassPrelinker::apply_final_image_eager_linkage(TRAPS) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");

  if (_final_image_eager_linkage != nullptr) {
    _final_image_eager_linkage->resolve_indys_in_final_image(CHECK);
    _final_image_eager_linkage->archive_reflection_data_in_final_image(THREAD);
    _final_image_eager_linkage->archive_dynamic_proxies(CHECK);
  }

  // Set it to null as we don't need to write this table into the final image.
  _final_image_eager_linkage = nullptr;
}

int ClassPrelinker::class_reflection_data_flags(InstanceKlass* ik, TRAPS) {
  assert(java_lang_Class::has_reflection_data(ik->java_mirror()), "must be");

  HandleMark hm(THREAD);
  JavaCallArguments args(Handle(THREAD, ik->java_mirror()));
  JavaValue result(T_INT);
  JavaCalls::call_special(&result,
                          vmClasses::Class_klass(),
                          vmSymbols::encodeReflectionData_name(),
                          vmSymbols::void_int_signature(),
                          &args, CHECK_0);
  int flags = result.get_jint();
  log_info(cds)("Encode ReflectionData: %s (flags=0x%x)", ik->external_name(), flags);
  return flags;
}

void ClassPrelinker::generate_reflection_data(JavaThread* current, InstanceKlass* ik, int rd_flags) {
  log_info(cds)("Generate ReflectionData: %s (flags=" INT32_FORMAT_X ")", ik->external_name(), rd_flags);
  JavaThread* THREAD = current; // for exception macros
  JavaCallArguments args(Handle(THREAD, ik->java_mirror()));
  args.push_int(rd_flags);
  JavaValue result(T_OBJECT);
  JavaCalls::call_special(&result,
                          vmClasses::Class_klass(),
                          vmSymbols::generateReflectionData_name(),
                          vmSymbols::int_void_signature(),
                          &args, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    Handle exc_handle(THREAD, PENDING_EXCEPTION);
    CLEAR_PENDING_EXCEPTION;

    log_warning(cds)("Exception during Class::generateReflectionData() call for %s", ik->external_name());
    LogStreamHandle(Debug, cds) log;
    if (log.is_enabled()) {
      java_lang_Throwable::print_stack_trace(exc_handle, &log);
    }
  }
}

Klass* ClassPrelinker::resolve_boot_klass_or_fail(const char* class_name, TRAPS) {
  Handle class_loader;
  Handle protection_domain;
  TempNewSymbol class_name_sym = SymbolTable::new_symbol(class_name);
  return SystemDictionary::resolve_or_fail(class_name_sym, class_loader, protection_domain, true, THREAD);
}

void ClassPrelinker::trace_dynamic_proxy_class(oop loader, const char* proxy_name, objArrayOop interfaces, int access_flags) {
  if (interfaces->length() < 1) {
    return;
  }
  if (ClassListWriter::is_enabled()) {
    const char* loader_name = ArchiveUtils::builtin_loader_name_or_null(loader);
    if (loader_name != nullptr) {
      stringStream ss;
      ss.print("%s %s %d %d", loader_name, proxy_name, access_flags, interfaces->length());
      for (int i = 0; i < interfaces->length(); i++) {
        oop mirror = interfaces->obj_at(i);
        Klass* k = java_lang_Class::as_Klass(mirror);
        ss.print(" %s", k->name()->as_C_string());
      }
      ClassListWriter w;
      w.stream()->print_cr("@dynamic-proxy %s", ss.freeze());
    }
  }
  if (CDSConfig::is_dumping_preimage_static_archive()) {
    FinalImageEagerLinkage::record_dynamic_proxy_class(loader, proxy_name, interfaces, access_flags);
  }
}

void ClassPrelinker::init_dynamic_proxy_cache(TRAPS) {
  static bool inited = false;
  if (inited) {
    return;
  }
  inited = true;

  Klass* klass = resolve_boot_klass_or_fail("java/lang/reflect/Proxy", CHECK);
  TempNewSymbol method = SymbolTable::new_symbol("initCacheForCDS");
  TempNewSymbol signature = SymbolTable::new_symbol("(Ljava/lang/ClassLoader;Ljava/lang/ClassLoader;)V");

  JavaCallArguments args;
  args.push_oop(Handle(THREAD, SystemDictionary::java_platform_loader()));
  args.push_oop(Handle(THREAD, SystemDictionary::java_system_loader()));
  JavaValue result(T_VOID);
  JavaCalls::call_static(&result,
                         klass,
                         method,
                         signature,
                         &args, CHECK);
}


void ClassPrelinker::define_dynamic_proxy_class(Handle loader, Handle proxy_name, Handle interfaces, int access_flags, TRAPS) {
  if (!CDSConfig::is_dumping_dynamic_proxy() || !ArchiveDynamicProxies) {
    return;
  }
  init_dynamic_proxy_cache(CHECK);

  Klass* klass = resolve_boot_klass_or_fail("java/lang/reflect/Proxy$ProxyBuilder", CHECK);
  TempNewSymbol method = SymbolTable::new_symbol("defineProxyClassForCDS");
  TempNewSymbol signature = SymbolTable::new_symbol("(Ljava/lang/ClassLoader;Ljava/lang/String;[Ljava/lang/Class;I)Ljava/lang/Class;");

  JavaCallArguments args;
  args.push_oop(Handle(THREAD, loader()));
  args.push_oop(Handle(THREAD, proxy_name()));
  args.push_oop(Handle(THREAD, interfaces()));
  args.push_int(access_flags);
  JavaValue result(T_OBJECT);
  JavaCalls::call_static(&result,
                         klass,
                         method,
                         signature,
                         &args, CHECK);

  // Assumptions:
  // FMG is archived, which means -modulepath and -Xbootclasspath are both not specified.
  // All named modules are loaded from the system modules files.
  // TODO: test support for -Xbootclasspath after JDK-8322322. Some of the code below need to be changed.
  // TODO: we just give dummy shared_classpath_index for the generated class so that it will be archived.
  //       The index is not used at runtime (see SystemDictionaryShared::load_shared_class_for_builtin_loader, which
  //       uses a null ProtectionDomain for this class)
  oop mirror = result.get_oop();
  assert(mirror != nullptr, "class must have been generated if not OOM");
  InstanceKlass* ik = InstanceKlass::cast(java_lang_Class::as_Klass(mirror));
  if (ik->is_shared_boot_class() || ik->is_shared_platform_class()) {
    assert(ik->module()->is_named(), "dynamic proxies defined in unnamed modules for boot/platform loaders not supported");
    ik->set_shared_classpath_index(0);
  } else {
    assert(ik->is_shared_app_class(), "must be");
    ik->set_shared_classpath_index(ClassLoaderExt::app_class_paths_start_index());
  }

  ArchiveBuilder::alloc_stats()->record_dynamic_proxy_class();
  if (log_is_enabled(Info, cds, dynamic, proxy)) {
    ResourceMark rm(THREAD);
    stringStream ss;
    const char* prefix = "";
    ss.print("%s (%-7s, cp index = %d) implements ", ik->external_name(),
             ArchiveUtils::builtin_loader_name(loader()), ik->shared_classpath_index());
    objArrayOop intfs = (objArrayOop)interfaces();
    for (int i = 0; i < intfs->length(); i++) {
      oop intf_mirror = intfs->obj_at(i);
      ss.print("%s%s", prefix, java_lang_Class::as_Klass(intf_mirror)->external_name());
      prefix = ", ";
    }

    log_info(cds, dynamic, proxy)("%s", ss.freeze());
  }
}

// Warning -- this is fragile!!!
// This is a hard-coded list of classes that are safe to preinitialize at dump time. It needs
// to be updated if the Java source code changes.
class ForcePreinitClosure : public CLDClosure {
public:
  void do_cld(ClassLoaderData* cld) {
    assert(CDSConfig::is_dumping_invokedynamic(), "sanity");

    static const char* forced_preinit_classes[] = {
      "java/util/HexFormat",
      "jdk/internal/util/ClassFileDumper",
      "java/lang/reflect/ClassFileFormatVersion",
      "java/lang/Character$CharacterCache",
      "java/lang/invoke/Invokers",
      "java/lang/invoke/Invokers$Holder",
      "java/lang/invoke/MethodHandle",
      "java/lang/invoke/MethodHandleStatics",
      "java/lang/invoke/DelegatingMethodHandle",
      "java/lang/invoke/DelegatingMethodHandle$Holder",
      "java/lang/invoke/LambdaForm",
      "java/lang/invoke/LambdaForm$NamedFunction",
      "java/lang/invoke/ClassSpecializer",
      "java/lang/invoke/DirectMethodHandle",
      "java/lang/invoke/DirectMethodHandle$Holder",
      "java/lang/invoke/BoundMethodHandle$Specializer",
      "java/lang/invoke/MethodHandles$Lookup",

    //TODO: these use java.lang.ClassValue$Entry which is a subtype of WeakReference
    //"java/lang/reflect/Proxy$ProxyBuilder",
    //"java/lang/reflect/Proxy",

    // TODO -- need to clear internTable, etc
    //"java/lang/invoke/MethodType",

    // TODO -- these need to link to native code
    //"java/lang/invoke/BoundMethodHandle",
    //"java/lang/invoke/BoundMethodHandle$Holder",
    //"java/lang/invoke/MemberName",
    //"java/lang/invoke/MethodHandleNatives",
      nullptr
    };
    for (Klass* k = cld->klasses(); k != nullptr; k = k->next_link()) {
      if (k->is_instance_klass()) {
        for (const char** classes = forced_preinit_classes; *classes != nullptr; classes++) {
          const char* class_name = *classes;
          if (k->name()->equals(class_name)) {
            ResourceMark rm;
            log_info(cds, init)("Force initialization %s", k->external_name());
            SystemDictionaryShared::force_preinit(InstanceKlass::cast(k));
          }
        }
      }
    }
  }
};

void ClassPrelinker::setup_forced_preinit_classes() {
  if (!CDSConfig::is_dumping_invokedynamic()) {
    return;
  }

  // Collect all loaded ClassLoaderData.
  ForcePreinitClosure closure;
  MutexLocker lock(ClassLoaderDataGraph_lock);
  ClassLoaderDataGraph::loaded_cld_do(&closure);
}

// Initialize a class at dump time, if possible.
void ClassPrelinker::maybe_preinit_class(InstanceKlass* ik, TRAPS) {
  if (ik->is_initialized()) {
    return;
  }

  {
    MutexLocker ml(DumpTimeTable_lock, Mutex::_no_safepoint_check_flag);
    if (!SystemDictionaryShared::can_be_preinited(ik)) {
      return;
    }
  }

  if (log_is_enabled(Info, cds, init)) {
    ResourceMark rm;
    log_info(cds, init)("preinitializing %s", ik->external_name());
  }
  ik->initialize(CHECK);
}

bool ClassPrelinker::can_archive_preinitialized_mirror(InstanceKlass* ik) {
  assert(!ArchiveBuilder::current()->is_in_buffer_space(ik), "must be source klass");
  if (!CDSConfig::is_initing_classes_at_dump_time()) {
    return false;
  }

  if (ik->is_hidden()) {
    return HeapShared::is_archivable_hidden_klass(ik);
  } else {
    return SystemDictionaryShared::can_be_preinited(ik);
  }
}

void ClassPrelinker::serialize(SerializeClosure* soc, bool is_static_archive) {
  PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_klasses : &_dynamic_preloaded_klasses;

  soc->do_ptr((void**)&table->_boot);
  soc->do_ptr((void**)&table->_boot2);
  soc->do_ptr((void**)&table->_platform);
  soc->do_ptr((void**)&table->_platform_initiated);
  soc->do_ptr((void**)&table->_app);
  soc->do_ptr((void**)&table->_app_initiated);

  if (is_static_archive) {
    soc->do_ptr((void**)&_final_image_eager_linkage);
    soc->do_ptr((void**)&_unregistered_klasses_from_preimage);
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

int ClassPrelinker::num_platform_initiated_classes() {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = CDSConfig::is_dumping_dynamic_archive() ? &_dynamic_preloaded_klasses : &_static_preloaded_klasses;
    return table->_platform_initiated->length();
  }
  return 0;
}

int ClassPrelinker::num_app_initiated_classes() {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = CDSConfig::is_dumping_dynamic_archive() ? &_dynamic_preloaded_klasses : &_static_preloaded_klasses;
    return table->_app_initiated->length();
  }
  return 0;
}

volatile bool _class_preloading_finished = false;

bool ClassPrelinker::class_preloading_finished() {
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
void ClassPrelinker::runtime_preload(JavaThread* current, Handle loader) {
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
    runtime_preload(&_static_preloaded_klasses, loader, current);
    if (!current->has_pending_exception()) {
      runtime_preload(&_dynamic_preloaded_klasses, loader, current);
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
      assert(_unregistered_klasses_from_preimage != nullptr, "must be");
      for (int i = 0; i < _unregistered_klasses_from_preimage->length(); i++) {
        InstanceKlass* ik = _unregistered_klasses_from_preimage->at(i);
        SystemDictionaryShared::init_dumptime_info(ik);
        SystemDictionaryShared::add_unregistered_class(current, ik);
      }
    }
  }
}

void ClassPrelinker::jvmti_agent_error(InstanceKlass* expected, InstanceKlass* actual, const char* type) {
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

void ClassPrelinker::runtime_preload(PreloadedKlasses* table, Handle loader, TRAPS) {
  PerfTraceTime timer(_perf_class_preload_counters);
  Array<InstanceKlass*>* preloaded_klasses;
  Array<InstanceKlass*>* initiated_klasses = nullptr;
  const char* loader_name;
  ClassLoaderData* loader_data = ClassLoaderData::class_loader_data(loader());

  // ResourceMark is missing in the code below due to JDK-8307315
  ResourceMark rm(THREAD);
  if (loader() == nullptr) {
    if (_preload_javabase_only) {
      loader_name = "boot ";
      preloaded_klasses = table->_boot;
    } else {
      loader_name = "boot2";
      preloaded_klasses = table->_boot2;
    }
  } else if (loader() == SystemDictionary::java_platform_loader()) {
    initiated_klasses = table->_platform_initiated;
    preloaded_klasses = table->_platform;
    loader_name = "plat ";
  } else {
    assert(loader() == SystemDictionary::java_system_loader(), "must be");
    initiated_klasses = table->_app_initiated;
    preloaded_klasses = table->_app;
    loader_name = "app  ";
  }

  if (initiated_klasses != nullptr) {
    MonitorLocker mu1(SystemDictionary_lock);

    for (int i = 0; i < initiated_klasses->length(); i++) {
      InstanceKlass* ik = initiated_klasses->at(i);
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

  if (preloaded_klasses != nullptr) {
    for (int i = 0; i < preloaded_klasses->length(); i++) {
      if (UsePerfData) {
        _perf_classes_preloaded->inc();
      }
      InstanceKlass* ik = preloaded_klasses->at(i);
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
            actual = SystemDictionary::load_instance_class(ik->name(), loader, CHECK);
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
      // The java.base classes needs to wait till ClassPrelinker::init_javabase_preloaded_classes()
      for (int i = 0; i < preloaded_klasses->length(); i++) {
        InstanceKlass* ik = preloaded_klasses->at(i);
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

void ClassPrelinker::preload_archived_hidden_class(Handle class_loader, InstanceKlass* ik,
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

void ClassPrelinker::init_javabase_preloaded_classes(TRAPS) {
  Array<InstanceKlass*>* preloaded_klasses = _static_preloaded_klasses._boot;
  if (preloaded_klasses != nullptr) {
    for (int i = 0; i < preloaded_klasses->length(); i++) {
      InstanceKlass* ik = preloaded_klasses->at(i);
      if (ik->has_preinitialized_mirror()) {
        ik->initialize_from_cds(CHECK);
      }
    }
  }

  // Initialize java.base classes in the default subgraph.
  HeapShared::initialize_default_subgraph_classes(Handle(), CHECK);
}

void ClassPrelinker::replay_training_at_init_for_javabase_preloaded_classes(TRAPS) {
  Array<InstanceKlass*>* preloaded_klasses = _static_preloaded_klasses._boot;
  if (preloaded_klasses != nullptr) {
    for (int i = 0; i < preloaded_klasses->length(); i++) {
      InstanceKlass* ik = preloaded_klasses->at(i);
      if (ik->is_initialized()) {
        if (log_is_enabled(Debug, cds, init)) {
          ResourceMark rm;
          log_debug(cds, init)("replay training %s", ik->external_name());
        }
        CompilationPolicy::replay_training_at_init(ik, CHECK);
      }
    }
  }
}

void ClassPrelinker::print_counters() {
  if (UsePerfData && _perf_class_preload_counters != nullptr) {
    LogStreamHandle(Info, init) log;
    if (log.is_enabled()) {
      log.print_cr("ClassPrelinker:");
      log.print_cr("  preload:           %ldms (elapsed) %ld (thread) / %ld events",
                   _perf_class_preload_counters->elapsed_counter_value_ms(),
                   _perf_class_preload_counters->thread_counter_value_ms(),
                   _perf_classes_preloaded->get_value());
    }
  }
}
