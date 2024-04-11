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
#include "cds/cdsConfig.hpp"
#include "cds/classPrelinker.hpp"
#include "cds/finalImageRecipes.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "runtime/handles.inline.hpp"

GrowableArray<InstanceKlass*>* FinalImageRecipes::_tmp_reflect_klasses = nullptr;
GrowableArray<int>* FinalImageRecipes::_tmp_reflect_flags = nullptr;
GrowableArray<FinalImageRecipes::TmpDynamicProxyClassInfo>* FinalImageRecipes::_tmp_dynamic_proxy_classes = nullptr;
static FinalImageRecipes* _final_image_recipes = nullptr;

void* FinalImageRecipes::operator new(size_t size) throw() {
  return ArchiveBuilder::current()->ro_region_alloc(size);
}

void FinalImageRecipes::record_recipes_impl() {
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

void FinalImageRecipes::apply_recipes_for_invokedynamic(TRAPS) {
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

void FinalImageRecipes::apply_recipes_for_reflection_data(JavaThread* current) {
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

void FinalImageRecipes::add_reflection_data_flags(InstanceKlass* ik, TRAPS) {
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

void FinalImageRecipes::apply_recipes_for_dynamic_proxies(TRAPS) {
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

void FinalImageRecipes::record_recipes() {
  _final_image_recipes = new FinalImageRecipes();
  _final_image_recipes->record_recipes_impl();
}

void FinalImageRecipes::apply_recipes(TRAPS) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");

  if (_final_image_recipes != nullptr) {
    _final_image_recipes->apply_recipes_for_invokedynamic(CHECK);
    _final_image_recipes->apply_recipes_for_reflection_data(THREAD);
    _final_image_recipes->apply_recipes_for_dynamic_proxies(CHECK);
  }

  // Set it to null as we don't need to write this table into the final image.
  _final_image_recipes = nullptr;
}

void FinalImageRecipes::serialize(SerializeClosure* soc, bool is_static_archive) {
  if (is_static_archive) {
    soc->do_ptr((void**)&_final_image_recipes);
  }
}
