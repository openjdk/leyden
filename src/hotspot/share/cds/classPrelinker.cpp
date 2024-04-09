/*
 * Copyright (c) 2022, 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/cdsConfig.hpp"
#include "cds/classPrelinker.hpp"
#include "cds/classPreloader.hpp"
#include "cds/classListWriter.hpp"
#include "cds/heapShared.hpp"
#include "cds/lambdaFormInvokers.inline.hpp"
#include "cds/regeneratedClasses.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderExt.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "interpreter/bytecode.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/perfData.inline.hpp"
#include "runtime/timer.hpp"
#include "runtime/signature.hpp"

ClassPrelinker::ClassesTable* ClassPrelinker::_processed_classes = nullptr;
ClassPrelinker::ClassesTable* ClassPrelinker::_vm_classes = nullptr;
int ClassPrelinker::_num_vm_klasses = 0;

bool ClassPrelinker::is_vm_class(InstanceKlass* ik) {
  return (_vm_classes->get(ik) != nullptr);
}

void ClassPrelinker::add_one_vm_class(InstanceKlass* ik) {
  bool created;
  ClassPreloader::add_preloaded_klass(ik);
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
  _processed_classes = new (mtClass)ClassesTable();
  for (auto id : EnumRange<vmClassID>{}) {
    add_one_vm_class(vmClasses::klass_at(id));
  }
}

void ClassPrelinker::dispose() {
  assert(_vm_classes != nullptr, "must be");
  delete _vm_classes;
  delete _processed_classes;
  _vm_classes = nullptr;
  _processed_classes = nullptr;
}

// Returns true if we CAN PROVE that cp_index will always resolve to
// the same information at both dump time and run time. This is a
// necessary (but not sufficient) condition for pre-resolving cp_index
// during CDS archive assembly.
bool ClassPrelinker::is_resolution_deterministic(ConstantPool* cp, int cp_index) {
  assert(!is_in_archivebuilder_buffer(cp), "sanity");

  if (cp->tag_at(cp_index).is_klass()) {
    // We require cp_index to be already resolved. This is fine for now, are we
    // currently archive only CP entries that are already resolved.
    Klass* resolved_klass = cp->resolved_klass_at(cp_index);
    return resolved_klass != nullptr && is_klass_resolution_deterministic(cp->pool_holder(), resolved_klass);
  } else if (cp->tag_at(cp_index).is_invoke_dynamic()) {
    return is_indy_resolution_deterministic(cp, cp_index);
  } else if (cp->tag_at(cp_index).is_field() ||
             cp->tag_at(cp_index).is_method() ||
             cp->tag_at(cp_index).is_interface_method()) {
    int klass_cp_index = cp->uncached_klass_ref_index_at(cp_index);
    if (!cp->tag_at(klass_cp_index).is_klass()) {
      // Not yet resolved
      return false;
    }
    Klass* k = cp->resolved_klass_at(klass_cp_index);
    if (!is_klass_resolution_deterministic(cp->pool_holder(), k)) {
      return false;
    }

    if (!k->is_instance_klass()) { // FIXME -- remove this check, and add a test case
      // FIXME: is it valid to have a non-instance klass in method refs?
      return false;
    }

    // Here, We don't check if this entry can actually be resolved to a valid Field/Method.
    // This method should be called by the ConstantPool to check Fields/Methods that
    // have already been successfully resolved.
    return true;
  } else {
    return false;
  }
}

bool ClassPrelinker::is_klass_resolution_deterministic(InstanceKlass* cp_holder, Klass* resolved_klass) {
  assert(!is_in_archivebuilder_buffer(cp_holder), "sanity");
  assert(!is_in_archivebuilder_buffer(resolved_klass), "sanity");

  if (resolved_klass->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(resolved_klass);

    if (!ik->is_shared() && SystemDictionaryShared::is_excluded_class(ik)) {
      return false;
    }

    if (cp_holder->is_subtype_of(ik)) {
      // All super types of ik will be resolved in ik->class_loader() before
      // ik is defined in this loader, so it's safe to archive the resolved klass reference.
      return true;
    }

    if (ClassPreloader::is_preloaded_class(ik)) {
      if (cp_holder->is_shared_platform_class()) {
        ClassPreloader::add_initiated_klass(cp_holder, ik);
        return true;
      } else if (cp_holder->is_shared_app_class()) {
        ClassPreloader::add_initiated_klass(cp_holder, ik);
        return true;
      } else if (cp_holder->is_shared_boot_class()) {
        assert(ik->class_loader() == nullptr, "a boot class can reference only boot classes");
        return true;
      } else if (cp_holder->is_hidden() && cp_holder->class_loader() == nullptr) { // FIXME -- use better checks!
        return true;
      }
    }
  } else if (resolved_klass->is_objArray_klass()) {
    Klass* elem = ObjArrayKlass::cast(resolved_klass)->bottom_klass();
    if (elem->is_instance_klass()) {
      return is_klass_resolution_deterministic(cp_holder, InstanceKlass::cast(elem));
    } else if (elem->is_typeArray_klass()) {
      return true;
    }
  } else if (resolved_klass->is_typeArray_klass()) {
    return true;
  }
  
  return false;
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
  int cp_index = cp->to_cp_index(raw_index, bc);

  if (cp->is_resolved(raw_index, bc)) {
    return;
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
    is_static = " *** static";
    break;
  case Bytecodes::_getfield:
  case Bytecodes::_putfield:
    InterpreterRuntime::resolve_get_put(bc, raw_index, mh, cp, false /*initialize_holder*/, CHECK);
    break;

  case Bytecodes::_invokestatic:
    if (!VM_Version::supports_fast_class_init_checks()) {
      return; // Do not resolve since interpreter lacks fast clinit barriers support
    }
    InterpreterRuntime::cds_resolve_invoke(bc, raw_index, mh, cp, CHECK);
    is_static = " *** static";
    break;

  case Bytecodes::_invokevirtual:
  case Bytecodes::_invokespecial:
  case Bytecodes::_invokeinterface:
    InterpreterRuntime::cds_resolve_invoke(bc, raw_index, mh, cp, CHECK);
    break;

  case Bytecodes::_invokehandle:
    InterpreterRuntime::cds_resolve_invokehandle(raw_index, cp, CHECK);
    break;

  default:
    ShouldNotReachHere();
  }

  if (log_is_enabled(Trace, cds, resolve)) {
    ResourceMark rm(THREAD);
    bool resolved = cp->is_resolved(raw_index, bc);
    Symbol* name = cp->name_ref_at(raw_index, bc);
    Symbol* signature = cp->signature_ref_at(raw_index, bc);
    log_trace(cds, resolve)("%s %s [%3d] %s%s -> %s.%s:%s%s",
                            (resolved ? "Resolved" : "Failed to resolve"),
                            Bytecodes::name(bc), cp_index, ik->external_name(), is_regen,
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
    if (preresolve_list->at(cp_index) == true && !rie->is_resolved() && is_indy_resolution_deterministic(cp(), cp_index)) {
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

bool ClassPrelinker::is_indy_resolution_deterministic(ConstantPool* cp, int cp_index) {
  assert(cp->tag_at(cp_index).is_invoke_dynamic(), "sanity");
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

void ClassPrelinker::serialize(SerializeClosure* soc, bool is_static_archive) {
  if (is_static_archive) {
    soc->do_ptr((void**)&_final_image_eager_linkage);
  }
}
