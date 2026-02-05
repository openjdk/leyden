/*
 * Copyright (c) 2024, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/aotConstantPoolResolver.hpp"
#include "cds/aotLinkedClassBulkLoader.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/archiveUtils.inline.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/finalImageRecipes.hpp"
#include "cds/unregisteredClasses.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/symbol.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/resizableHashTable.hpp"

GrowableArray<InstanceKlass*>* FinalImageRecipes::_tmp_reflect_klasses = nullptr;
GrowableArray<int>* FinalImageRecipes::_tmp_reflect_flags = nullptr;
GrowableArray<FinalImageRecipes::TmpDynamicProxyClassInfo>* FinalImageRecipes::_tmp_dynamic_proxy_classes = nullptr;
static FinalImageRecipes* _final_image_recipes = nullptr;

void* FinalImageRecipes::operator new(size_t size) throw() {
  return ArchiveBuilder::current()->ro_region_alloc(size);
}

void FinalImageRecipes::record_all_classes() {
  _all_klasses = ArchiveUtils::archive_array(ArchiveBuilder::current()->klasses());
  ArchivePtrMarker::mark_pointer(&_all_klasses);
}

void FinalImageRecipes::record_recipes_for_constantpool() {
  ResourceMark rm;

  // The recipes are recorded regardless of CDSConfig::is_dumping_{invokedynamic,dynamic_proxies,reflection_data}().
  // If some of these options are not enabled, the corresponding recipes will be
  // ignored during the final image assembly.

  GrowableArray<Array<int>*> tmp_cp_recipes;
  GrowableArray<int> tmp_flags;

  GrowableArray<Klass*>* klasses = ArchiveBuilder::current()->klasses();
  for (int i = 0; i < klasses->length(); i++) {
    GrowableArray<int> cp_indices;
    int flags = 0;

    Klass* k = klasses->at(i);
    if (k->is_instance_klass()) {
      InstanceKlass* ik = InstanceKlass::cast(k);
      ConstantPool* cp = ik->constants();
      ConstantPoolCache* cp_cache = cp->cache();

      if (ik->is_initialized()) {
        flags |= WAS_INITED;
      }

      for (int cp_index = 1; cp_index < cp->length(); cp_index++) { // Index 0 is unused
        if (cp->tag_at(cp_index).value() == JVM_CONSTANT_Class) {
          Klass* k = cp->resolved_klass_at(cp_index);
          if (k->is_instance_klass()) {
            cp_indices.append(cp_index);
            flags |= CP_RESOLVE_CLASS;
          }
        }
      }

      if (cp_cache != nullptr) {
        Array<ResolvedFieldEntry>* field_entries = cp_cache->resolved_field_entries();
        if (field_entries != nullptr) {
          for (int i = 0; i < field_entries->length(); i++) {
            ResolvedFieldEntry* rfe = field_entries->adr_at(i);
            if (rfe->is_resolved(Bytecodes::_getstatic) ||
                rfe->is_resolved(Bytecodes::_putstatic) ||
                rfe->is_resolved(Bytecodes::_getfield) ||
                rfe->is_resolved(Bytecodes::_putfield)) {
              cp_indices.append(rfe->constant_pool_index());
              flags |= CP_RESOLVE_FIELD_AND_METHOD;
            }
          }
        }

        Array<ResolvedMethodEntry>* method_entries = cp_cache->resolved_method_entries();
        if (method_entries != nullptr) {
          for (int i = 0; i < method_entries->length(); i++) {
            ResolvedMethodEntry* rme = method_entries->adr_at(i);
            if (rme->is_resolved(Bytecodes::_invokevirtual) ||
                rme->is_resolved(Bytecodes::_invokespecial) ||
                rme->is_resolved(Bytecodes::_invokeinterface) ||
                rme->is_resolved(Bytecodes::_invokestatic) ||
                rme->is_resolved(Bytecodes::_invokehandle)) {
              cp_indices.append(rme->constant_pool_index());
              flags |= CP_RESOLVE_FIELD_AND_METHOD;
            }
          }
        }

        Array<ResolvedIndyEntry>* indy_entries = cp_cache->resolved_indy_entries();
        if (indy_entries != nullptr) {
          for (int i = 0; i < indy_entries->length(); i++) {
            ResolvedIndyEntry* rie = indy_entries->adr_at(i);
            int cp_index = rie->constant_pool_index();
            if (rie->is_resolved()) {
              cp_indices.append(cp_index);
              flags |= CP_RESOLVE_INDY;
            }
          }
        }
      }
    }

    if (cp_indices.length() > 0) {
      LogStreamHandle(Trace, aot, resolve) log;
      if (log.is_enabled()) {
        log.print("ConstantPool entries for %s to be pre-resolved:", k->external_name());
        for (int i = 0; i < cp_indices.length(); i++) {
          log.print(" %d", cp_indices.at(i));
        }
        log.print("\n");
      }
      tmp_cp_recipes.append(ArchiveUtils::archive_array(&cp_indices));
    } else {
      tmp_cp_recipes.append(nullptr);
    }
    tmp_flags.append(flags);
  }

  _cp_recipes = ArchiveUtils::archive_array(&tmp_cp_recipes);
  ArchivePtrMarker::mark_pointer(&_cp_recipes);

  _flags = ArchiveUtils::archive_array(&tmp_flags);
  ArchivePtrMarker::mark_pointer(&_flags);
}

void FinalImageRecipes::apply_recipes_for_constantpool(JavaThread* current) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");

  for (int i = 0; i < _all_klasses->length(); i++) {
    Array<int>* cp_indices = _cp_recipes->at(i);
    int flags = _flags->at(i);
    if (cp_indices != nullptr) {
      InstanceKlass* ik = InstanceKlass::cast(_all_klasses->at(i));
      if (!strcmp(ik->external_name(), "org.openjdk.aot.testclass.Foo")) {
        log_info(cds)("Applying constant pool recipes for %s", ik->external_name());
      }
      if (ik->is_loaded()) {
        ResourceMark rm(current);
        ConstantPool* cp = ik->constants();
        GrowableArray<bool> preresolve_list(cp->length(), cp->length(), false);
        for (int j = 0; j < cp_indices->length(); j++) {
          preresolve_list.at_put(cp_indices->at(j), true);
        }
        if ((flags & CP_RESOLVE_CLASS) != 0) {
          AOTConstantPoolResolver::preresolve_class_cp_entries(current, ik, &preresolve_list);
        }
        if ((flags & CP_RESOLVE_FIELD_AND_METHOD) != 0) {
          AOTConstantPoolResolver::preresolve_field_and_method_cp_entries(current, ik, &preresolve_list);
        }
        if ((flags & CP_RESOLVE_INDY) != 0) {
          AOTConstantPoolResolver::preresolve_indy_cp_entries(current, ik, &preresolve_list);
        }
      }
    }
  }
}

void FinalImageRecipes::record_recipes_for_reflection_data() {
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
}

void FinalImageRecipes::record_recipes_for_dynamic_proxies() {
  if (_tmp_dynamic_proxy_classes != nullptr) {
    // Remove proxies for excluded classes
    for (int i = _tmp_dynamic_proxy_classes->length() - 1; i >= 0; i--) {
      TmpDynamicProxyClassInfo* tmp_info = _tmp_dynamic_proxy_classes->adr_at(i);
      bool exclude = false;
      for (int j = 0; j < tmp_info->_interfaces->length(); j++) {
        if (SystemDictionaryShared::is_excluded_class(InstanceKlass::cast(tmp_info->_interfaces->at(j)))) {
          exclude = true;
          break;
        }
      }
      if (exclude) {
        _tmp_dynamic_proxy_classes->remove_at(i);
      }
    }
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
        InstanceKlass* intf = InstanceKlass::cast(tmp_info->_interfaces->at(j));
        assert(!SystemDictionaryShared::is_excluded_class(intf), "sanity");
        buffered_interfaces.append(ArchiveBuilder::current()->get_buffered_addr(intf));
      }
      info->_interfaces = ArchiveUtils::archive_array(&buffered_interfaces);

      ArchivePtrMarker::mark_pointer(&info->_proxy_name);
      ArchivePtrMarker::mark_pointer(&info->_interfaces);
      ArchiveBuilder::alloc_stats()->record_dynamic_proxy_class();
    }
  }
}

void FinalImageRecipes::load_all_classes(TRAPS) {
  assert(CDSConfig::is_dumping_final_static_archive(), "sanity");
  UnregisteredClasses::initialize(CHECK);
  Handle class_loader(THREAD, SystemDictionary::java_system_loader());
  for (int i = 0; i < _all_klasses->length(); i++) {
    Klass* k = _all_klasses->at(i);
    int flags = _flags->at(i);
    if (k->is_instance_klass()) {
      InstanceKlass* ik = InstanceKlass::cast(k);
      if (!strcmp(ik->external_name(), "org/openjdk/aot/testclass/Foo")) {
        log_info(cds)("FinalImageRecipes loading class %s", ik->external_name());
      }
      if (ik->defined_by_other_loaders() && !k->is_defined_by_aot_safe_custom_loader()) {
        SystemDictionaryShared::init_dumptime_info(ik);
        SystemDictionaryShared::add_unregistered_class(THREAD, ik);
        SystemDictionaryShared::copy_unregistered_class_size_and_crc32(ik);
      } else if (!ik->is_hidden() && !ik->defined_by_other_loaders()) {
        Klass* actual = SystemDictionary::resolve_or_fail(ik->name(), class_loader, true, CHECK);
        if (actual != ik) {
          ResourceMark rm(THREAD);
          log_error(aot)("Unable to resolve class from CDS archive: %s", ik->external_name());
          log_error(aot)("Expected: " INTPTR_FORMAT ", actual: " INTPTR_FORMAT, p2i(ik), p2i(actual));
          log_error(aot)("Please check if your VM command-line is the same as in the training run");
          AOTMetaspace::unrecoverable_writing_error();
        }
        assert(ik->is_loaded(), "must be");
        ik->link_class(CHECK);

        if (ik->has_aot_safe_initializer() && (flags & WAS_INITED) != 0) {
          //assert(ik->class_loader() == nullptr, "supported only for boot classes for now");
          ik->initialize(CHECK);
        }
      } else if (k->is_defined_by_aot_safe_custom_loader()) {
        // Use UnregisteredClassLoader to load these classes
        Handle unreg_class_loader = UnregisteredClasses::unregistered_class_loader(THREAD);
        assert(unreg_class_loader.not_null(), "must be");
        Klass* actual = SystemDictionary::resolve_or_fail(ik->name(), unreg_class_loader, true, CHECK);
        if (actual != ik) {
          ResourceMark rm(THREAD);
          log_error(aot)("Unable to resolve class from CDS archive: %s", ik->external_name());
          log_error(aot)("Expected: " INTPTR_FORMAT ", actual: " INTPTR_FORMAT, p2i(ik), p2i(actual));
          log_error(aot)("Please check if your VM command-line is the same as in the training run");
          AOTMetaspace::unrecoverable_writing_error();
        }
        assert(ik->is_loaded(), "must be");
        ik->link_class(CHECK);
      }
    }
  }
}

void FinalImageRecipes::apply_recipes_for_reflection_data(JavaThread* current) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");

  if (CDSConfig::is_dumping_reflection_data() && _reflect_klasses != nullptr) {
    assert(_reflect_flags != nullptr, "must be");
    for (int i = 0; i < _reflect_klasses->length(); i++) {
      InstanceKlass* ik = _reflect_klasses->at(i);
      int rd_flags = _reflect_flags->at(i);
      AOTConstantPoolResolver::generate_reflection_data(current, ik, rd_flags);
    }
  }
}

void FinalImageRecipes::add_reflection_data_flags(InstanceKlass* ik, TRAPS) {
  assert(CDSConfig::is_dumping_preimage_static_archive(), "must be");
  if (ik->is_linked() &&
      SystemDictionaryShared::is_builtin_loader(ik->class_loader_data()) && !ik->is_hidden() &&
      java_lang_Class::has_reflection_data(ik->java_mirror())) {
    int rd_flags = AOTConstantPoolResolver::class_reflection_data_flags(ik, CHECK);

    MutexLocker mu(FinalImageRecipes_lock, Mutex::_no_safepoint_check_flag);
    if (_tmp_reflect_klasses == nullptr) {
      _tmp_reflect_klasses = new (mtClassShared) GrowableArray<InstanceKlass*>(100, mtClassShared);
      _tmp_reflect_flags = new (mtClassShared) GrowableArray<int>(100, mtClassShared);
    }
    _tmp_reflect_klasses->append(ik);
    _tmp_reflect_flags->append(rd_flags);
  }
}

void FinalImageRecipes::add_dynamic_proxy_class(oop loader, const char* proxy_name, objArrayOop interfaces, int access_flags) {
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

  MutexLocker mu(FinalImageRecipes_lock, Mutex::_no_safepoint_check_flag);
  if (_tmp_dynamic_proxy_classes == nullptr) {
    _tmp_dynamic_proxy_classes = new (mtClassShared) GrowableArray<TmpDynamicProxyClassInfo>(32, mtClassShared);
  }

  log_info(cds, dynamic, proxy)("Adding proxy name %s", proxy_name);

  TmpDynamicProxyClassInfo info;
  info._loader_type = loader_type;
  info._access_flags = access_flags;
  info._proxy_name = os::strdup(proxy_name);
  info._interfaces = new (mtClassShared) GrowableArray<Klass*>(interfaces->length(), mtClassShared);
  for (int i = 0; i < interfaces->length(); i++) {
    Klass* intf = java_lang_Class::as_Klass(interfaces->obj_at(i));
    info._interfaces->append(InstanceKlass::cast(intf));

    if (log_is_enabled(Info, cds, dynamic, proxy)) {
      ResourceMark rm;
      log_info(cds, dynamic, proxy)("interface[%d] = %s", i, intf->external_name());
    }
  }
  _tmp_dynamic_proxy_classes->append(info);
}

void FinalImageRecipes::apply_recipes_for_dynamic_proxies(TRAPS) {
  if (CDSConfig::is_dumping_dynamic_proxies() && _dynamic_proxy_classes != nullptr) {
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

      AOTConstantPoolResolver::define_dynamic_proxy_class(loader, proxy_name, interfaces, info->_access_flags, CHECK);
    }
  }
}

void FinalImageRecipes::record_recipes() {
  assert(CDSConfig::is_dumping_preimage_static_archive(), "must be");
  _final_image_recipes = new FinalImageRecipes();
  _final_image_recipes->record_all_classes();
  _final_image_recipes->record_recipes_for_constantpool();
  _final_image_recipes->record_recipes_for_reflection_data();
  _final_image_recipes->record_recipes_for_dynamic_proxies();
}

void FinalImageRecipes::apply_recipes(TRAPS) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");
  if (_final_image_recipes != nullptr) {
    _final_image_recipes->apply_recipes_impl(THREAD);
    if (HAS_PENDING_EXCEPTION) {
      log_error(aot)("%s: %s", PENDING_EXCEPTION->klass()->external_name(),
                     java_lang_String::as_utf8_string(java_lang_Throwable::message(PENDING_EXCEPTION)));
      log_error(aot)("Please check if your VM command-line is the same as in the training run");
      AOTMetaspace::unrecoverable_writing_error("Unexpected exception, use -Xlog:aot,exceptions=trace for detail");
    }
  }

  // Set it to null as we don't need to write this table into the final image.
  _final_image_recipes = nullptr;
}

void FinalImageRecipes::apply_recipes_impl(TRAPS) {
  load_all_classes(CHECK);
  apply_recipes_for_constantpool(THREAD);
  apply_recipes_for_reflection_data(CHECK);
  apply_recipes_for_dynamic_proxies(CHECK);
}

void FinalImageRecipes::serialize(SerializeClosure* soc) {
  soc->do_ptr((void**)&_final_image_recipes);
}
