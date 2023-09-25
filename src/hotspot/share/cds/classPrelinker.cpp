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
#include "cds/cdsConfig.hpp"
#include "cds/classPrelinker.hpp"
#include "cds/heapShared.hpp"
#include "cds/lambdaFormInvokers.inline.hpp"
#include "cds/regeneratedClasses.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "interpreter/bytecode.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/vmOperations.hpp"

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
    assert(DynamicDumpSharedSpaces, "must be");
    add_preloaded_klasses(_static_preloaded_klasses._boot);
    add_preloaded_klasses(_static_preloaded_klasses._boot2);
    add_preloaded_klasses(_static_preloaded_klasses._platform);
    add_preloaded_klasses(_static_preloaded_klasses._app);

    // These initiated classes are already recorded in the static archive. There's no need to
    // record them again for the dynamic archive.
    bool need_to_record = false;
    add_initiated_klasses(_platform_initiated_classes, _static_preloaded_klasses._platform_initiated, need_to_record);
    add_initiated_klasses(_app_initiated_classes, _static_preloaded_klasses._app_initiated, need_to_record);
  }
}

void ClassPrelinker::add_preloaded_klasses(Array<InstanceKlass*>* klasses) {
  for (int i = 0; i < klasses->length(); i++) {
    assert(klasses->at(i)->is_shared() && klasses->at(i)->is_loaded(), "must be");
    _preloaded_classes->put_when_absent(klasses->at(i), true);
  }
}

void ClassPrelinker::add_initiated_klasses(ClassesTable* table, Array<InstanceKlass*>* klasses, bool need_to_record) {
  for (int i = 0; i < klasses->length(); i++) {
    InstanceKlass* ik = klasses->at(i);
    table->put_when_absent(ik, need_to_record);
  }
}

// ik has a reference to target:
//    - target is a declared supertype of ik, or
//    - one of the constant pool entries in ik references target
void ClassPrelinker::maybe_add_initiated_klass(InstanceKlass* ik, InstanceKlass* target) {
  if (ik->class_loader_data() == target->class_loader_data()) {
    return;
  }

  ClassesTable* initiated_classes = nullptr;
  const char* loader_name = nullptr;
  if (SystemDictionary::is_platform_class_loader(ik->class_loader())) {
    loader_name = "platform";
    initiated_classes = _platform_initiated_classes;
  } else {
    assert(SystemDictionary::is_system_class_loader(ik->class_loader()), "must be");
    loader_name = "app";
    initiated_classes = _app_initiated_classes;
  }

  if (initiated_classes != nullptr) {
    bool need_to_record = true;
    bool created;
    initiated_classes->put_if_absent(target, need_to_record, &created);
    if (created && log_is_enabled(Trace, cds, resolve)) {
      ResourceMark rm;
      log_trace(cds, resolve)("%s loader initiated %s -> %s", loader_name, ik->external_name(), target->external_name());
    }
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
        maybe_add_initiated_klass(cp_holder, ik);
        return true;
      } else if (cp_holder->is_shared_app_class()) {
        maybe_add_initiated_klass(cp_holder, ik);
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
  assert(cp->tag_at(cp_index).is_method(), "must be");
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
  if (k->find_field(field_name, field_sig, &fd) == NULL || fd.access_flags().is_static()) {
    // Static field resolution at runtime may trigger initialization, so we can't
    // archive it.
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
Klass* ClassPrelinker::find_loaded_class(JavaThread* current, oop class_loader, Symbol* name) {
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

Klass* ClassPrelinker::find_loaded_class(JavaThread* current, ConstantPool* cp, int class_cp_index) {
  Symbol* name = cp->klass_name_at(class_cp_index);
  return find_loaded_class(current, cp->pool_holder()->class_loader(), name);
}

#if INCLUDE_CDS_JAVA_HEAP
void ClassPrelinker::resolve_string(constantPoolHandle cp, int cp_index, TRAPS) {
  if (!CDSConfig::is_dumping_heap()) {
    return;
  }

  int cache_index = cp->cp_to_object_index(cp_index);
  ConstantPool::string_at_impl(cp, cp_index, cache_index, CHECK);
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
      Bytecodes::Code bc = bcs.raw_code();
      switch (bc) {
      case Bytecodes::_getfield:
      case Bytecodes::_putfield:
        maybe_resolve_fmi_ref(ik, m, bc, bcs.get_index_u2(), preresolve_list, THREAD);
        if (HAS_PENDING_EXCEPTION) {
          CLEAR_PENDING_EXCEPTION; // just ignore
        }
        break;
      case Bytecodes::_invokehandle:
        if (!ArchiveInvokeDynamic || true) { // FIXME this is buggy -- temporarily disabled
          break;
        }
        // fall-through
      case Bytecodes::_invokespecial:
    //case Bytecodes::_invokevirtual: FIXME - This fails with test/hotspot/jtreg/premain/jmh/run.sh
      case Bytecodes::_invokestatic: // This is only for a few specific cases.
        maybe_resolve_fmi_ref(ik, m, bc, bcs.get_index_u2_cpcache(), preresolve_list, THREAD);
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
  ConstantPoolCacheEntry* cp_cache_entry = nullptr;

  assert(bc != Bytecodes::_invokehandle, "this is buggy -- temporarily disabled");
  assert(bc != Bytecodes::_invokevirtual, "this is buggy -- temporarily disabled");

  if (bc == Bytecodes::_invokehandle ||
      bc == Bytecodes::_invokestatic ||
      bc == Bytecodes::_invokespecial ||
      bc == Bytecodes::_invokevirtual) {
    int cpc_index = cp->decode_cpcache_index(raw_index);
    cp_cache_entry = cp->cache()->entry_at(cpc_index);
    if (cp_cache_entry->is_resolved(bc)) {
      return;
    }
    cp_index = cp_cache_entry->constant_pool_index();
  } else {
    assert(bc == Bytecodes::_getfield || bc == Bytecodes::_putfield, "must be");
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
  case Bytecodes::_getfield:
  case Bytecodes::_putfield:
    InterpreterRuntime::resolve_get_put(bc, raw_index, mh, cp, CHECK);
    ref_kind = "field ";
    break;
  case Bytecodes::_invokevirtual:
    InterpreterRuntime::cds_resolve_invoke(bc, raw_index, mh, cp, cp_cache_entry, CHECK);
    ref_kind = "method";
    break;
  case Bytecodes::_invokespecial:
    // TODO Not implemented yet.
    return;
  case Bytecodes::_invokehandle:
    InterpreterRuntime::cds_resolve_invokehandle(raw_index, cp, CHECK);
    ref_kind = "method";
    break;
  case Bytecodes::_invokestatic:
    if (!resolved_klass->name()->equals("java/lang/invoke/MethodHandle") &&
        !resolved_klass->name()->equals("java/lang/invoke/MethodHandleNatives")
/* ||
        !LambdaFormInvokers::may_be_regenerated_class(ik->name())*/) {
      return;
    }
    InterpreterRuntime::cds_resolve_invoke(bc, raw_index, mh, cp, cp_cache_entry, CHECK);
    ref_kind = "method";
    is_static = " *** static";
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
    if (preresolve_list->at(cp_index) == true && !rie->is_resolved() && 
        should_preresolve_invokedynamic(cp(), cp_index)) {
      InterpreterRuntime::cds_resolve_invokedynamic(ConstantPool::encode_invokedynamic_index(i), cp, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        CLEAR_PENDING_EXCEPTION; // just ignore
      }
    }
  }
}

static GrowableArrayCHeap<char*, mtClassShared>* _invokedynamic_filter = nullptr;

bool ClassPrelinker::should_preresolve_invokedynamic(ConstantPool* cp, int cp_index) {
  if (!ArchiveInvokeDynamic || !HeapShared::can_write()) {
    return false;
  }

  // FIXME -- work around JDK-8316802: unresolved classes may be countered by compiler
  if (is_in_javabase(cp->pool_holder())) {
    return false;
  }

  int bsm = cp->bootstrap_method_ref_index_at(cp_index);
  int bsm_ref = cp->method_handle_index_at(bsm);
  Symbol* bsm_name = cp->uncached_name_ref_at(bsm_ref);
  Symbol* bsm_signature = cp->uncached_signature_ref_at(bsm_ref);
  Symbol* bsm_klass = cp->klass_name_at(cp->uncached_klass_ref_index_at(bsm_ref));

  if (bsm_klass->equals("java/lang/invoke/StringConcatFactory") &&
      bsm_name->equals("makeConcatWithConstants")) {
    // Support string concact for now
    return true;
  }

  if (bsm_klass->equals("java/lang/invoke/LambdaMetafactory") &&
      bsm_name->equals("metafactory") &&
      bsm_signature->equals("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;")) {
    // Support only string concact for now
    return true;
  }

  return false;
}

static Array<InstanceKlass*>* _klasses_for_indy_resolution = nullptr;
static Array<Array<int>*>* _cp_index_lists_for_indy_resolution = nullptr;

void ClassPrelinker::preresolve_indys_from_preimage(TRAPS) {
  if (CDSPreimage != nullptr && _klasses_for_indy_resolution != nullptr) {
    assert(_cp_index_lists_for_indy_resolution != nullptr, "must be");
    for (int i = 0; i < _klasses_for_indy_resolution->length(); i++) {
      InstanceKlass* ik = _klasses_for_indy_resolution->at(i);
      ConstantPool* cp = ik->constants();
      Array<int>* cp_indices = _cp_index_lists_for_indy_resolution->at(i);
      GrowableArray<bool> preresolve_list(cp->length(), cp->length(), false);
      for (int j = 0; j < cp_indices->length(); j++) {
        preresolve_list.at_put(cp_indices->at(j), true);
      }
      preresolve_indy_cp_entries(THREAD, ik, &preresolve_list);
    }
  }

  // These aren't needed in the final CDS image
  _klasses_for_indy_resolution = nullptr;
  _cp_index_lists_for_indy_resolution = nullptr;
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
  return (ik->module() != nullptr &&
          ik->module()->name() != nullptr &&
          ik->module()->name()->equals("java.base"));
}

class ClassPrelinker::PreloadedKlassRecorder : StackObj {
  int _loader_type;
  ResourceHashtable<InstanceKlass*, bool, 15889, AnyObj::RESOURCE_AREA, mtClassShared> _seen_klasses;
  GrowableArray<InstanceKlass*> _list;
  bool loader_type_matches(InstanceKlass* k) {
    switch (_loader_type) {
    case ClassLoader::BOOT_LOADER:     return k->is_shared_boot_class();
    case ClassLoader::PLATFORM_LOADER: return k->is_shared_platform_class();
    case ClassLoader::APP_LOADER:      return k->is_shared_app_class();
    default:
      ShouldNotReachHere();
      return false;
    }
  }

  void maybe_record(InstanceKlass* ik) {
    bool created;
    _seen_klasses.put_if_absent(ik, true, &created);
    if (!created) {
      // Already seen this class when we walked the hierarchy of a previous class
      return;
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

    if (ik->is_hidden()) {
      return;
    }

    if (!loader_type_matches(ik)) {
      return;
    }
    if (MetaspaceObj::is_shared(ik)) {
      if (DynamicDumpSharedSpaces) {
        return;
      } else {
        assert(CDSPreimage != nullptr, "must be");
      }
    }

    // Do not preload any module classes that are not from the modules images,
    // since such classes may not be loadable at runtime
    int scp_index = ik->shared_classpath_index();
    assert(scp_index >= 0, "must be");
    SharedClassPathEntry* scp_entry = FileMapInfo::shared_path(scp_index);
    if (scp_entry->in_named_module() && !scp_entry->is_modules_image()) {
      return;
    }

    InstanceKlass* s = ik->java_super();
    if (s != nullptr) {
      maybe_record(s);
      maybe_add_initiated_klass(ik, s);
    }

    Array<InstanceKlass*>* interfaces = ik->local_interfaces();
    int num_interfaces = interfaces->length();
    for (int index = 0; index < num_interfaces; index++) {
      InstanceKlass* intf = interfaces->at(index);
      maybe_record(intf);
      maybe_add_initiated_klass(ik, intf);
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
    return archive_klass_array(&_list);
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
  }
}

template <typename T>
Array<T>* archive_array(GrowableArray<T>* tmp_array) {
  Array<T>* archived_array = ArchiveBuilder::new_ro_array<T>(tmp_array->length());
  for (int i = 0; i < tmp_array->length(); i++) {
    archived_array->at_put(i, tmp_array->at(i));
    if (std::is_pointer<T>::value) {
      ArchivePtrMarker::mark_pointer(archived_array->adr_at(i));
    }
  }

  return archived_array;
}

Array<InstanceKlass*>* ClassPrelinker::archive_klass_array(GrowableArray<InstanceKlass*>* tmp_array) {
  return archive_array(tmp_array);
}

Array<InstanceKlass*>* ClassPrelinker::record_initiated_klasses(ClassesTable* table) {
  ResourceMark rm;
  GrowableArray<InstanceKlass*> tmp_array;

  auto collector = [&] (InstanceKlass* ik, bool need_to_record) {
    if (!need_to_record) {
      return;
    }
    tmp_array.append((InstanceKlass*)ArchiveBuilder::get_buffered_klass(ik));
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

  return archive_klass_array(&tmp_array);
}

void ClassPrelinker::record_initiated_klasses(bool is_static_archive) {
  if (PreloadSharedClasses) {
    PreloadedKlasses* table = (is_static_archive) ? &_static_preloaded_klasses : &_dynamic_preloaded_klasses;
    table->_platform_initiated = record_initiated_klasses(_platform_initiated_classes);
    table->_app_initiated = record_initiated_klasses(_app_initiated_classes);
  }
}

void ClassPrelinker::record_resolved_indys() {
  ResourceMark rm;
  GrowableArray<Klass*>* klasses = ArchiveBuilder::current()->klasses();
  GrowableArray<InstanceKlass*> tmp_klasses;
  GrowableArray<Array<int>*> tmp_cp_index_lists;
  int total_indys_to_resolve = 0;
  for (int i = 0; i < klasses->length(); i++) {
    Klass* k = klasses->at(i);
    if (k->is_instance_klass()) {
      InstanceKlass* ik = InstanceKlass::cast(k);
      GrowableArray<int> indices;

      if (ik->constants()->cache() != nullptr) {
        Array<ResolvedIndyEntry>* indy_entries = ik->constants()->cache()->resolved_indy_entries();
        if (indy_entries != nullptr) {
          for (int i = 0; i < indy_entries->length(); i++) {
            ResolvedIndyEntry* rie = indy_entries->adr_at(i);
            int cp_index = rie->constant_pool_index();
            if (rie->is_resolved()) {
              indices.append(cp_index);
            }
          }
        }
      }

      if (indices.length() > 0) {
        tmp_klasses.append(ArchiveBuilder::current()->get_buffered_addr(ik));
        tmp_cp_index_lists.append(archive_array(&indices));
        total_indys_to_resolve += indices.length();
      }
    }
  }

  assert(tmp_klasses.length() == tmp_cp_index_lists.length(), "must be");
  if (tmp_klasses.length() > 0) {
    _klasses_for_indy_resolution = archive_array(&tmp_klasses);
    _cp_index_lists_for_indy_resolution = archive_array(&tmp_cp_index_lists);
  }
  log_info(cds)("%d indies in %d classes will be resolved in final CDS image", total_indys_to_resolve, tmp_klasses.length());
}

// Warning -- this is fragile!!!
// This is a hard-coded list of classes that are safely to preinitialize at dump time. It needs
// to be updated if the Java source code changes.
class ForcePreinitClosure : public CLDClosure {
public:
  void do_cld(ClassLoaderData* cld) {
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
  if (!ArchiveInvokeDynamic) {
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
    soc->do_ptr((void**)&_klasses_for_indy_resolution);
    soc->do_ptr((void**)&_cp_index_lists_for_indy_resolution);
  }
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

    if (!ModuleEntryTable::javabase_defined()) {
      // For boot phase 1, init_archived_lambda_proxy_classes() is called some time
      // after ClassPrelinker::runtime_preload().
    } else {
      // Exceptions are caughted by ExceptionMark.
      SystemDictionaryShared::init_archived_lambda_proxy_classes(loader, current);
    }
  }

  assert(!current->has_pending_exception(), "VM should have exited due to ExceptionMark");
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
      InstanceKlass* ik = preloaded_klasses->at(i);
      if (log_is_enabled(Info, cds, preload)) {
        ResourceMark rm;
        log_info(cds, preload)("%s %s%s", loader_name, ik->external_name(),
                               ik->is_loaded() ? " (already loaded)" : "");
      }
      if (!ik->is_loaded()) {
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

    if (!_preload_javabase_only) {
      // The java.base classes needs to wait till ClassPrelinker::init_javabase_preloaded_classes()
      for (int i = 0; i < preloaded_klasses->length(); i++) {
        InstanceKlass* ik = preloaded_klasses->at(i);
        if (ik->has_preinitialized_mirror()) {
          ik->initialize_from_cds(CHECK);
        }
      }
    }
  }

#if 0
  // Hmm, does JavacBench crash if this block is disabled??
  if (VerifyDuringStartup) {
    VM_Verify verify_op;
    VMThread::execute(&verify_op);
  }
#endif
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
}
