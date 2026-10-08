/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/aotClassLocation.hpp"
#include "cds/aotLinkedClassBulkLoader.hpp"
#include "cds/aotLogging.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/archiveUtils.inline.hpp"
#include "cds/customLoaderSupport.hpp"
#include "cds/heapShared.hpp"
#include "cds/serializeClosure.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/compactHashtable.hpp"
#include "classfile/systemDictionary.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/resourceArea.hpp"
#include "oops/array.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/symbol.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/resizableHashTable.hpp"

#if INCLUDE_CDS_JAVA_HEAP

ClassLoaderAotIdTable::LoaderIdTable* ClassLoaderAotIdTable::_loader_id_table = nullptr;
ClassLoaderAotIdTable::LoaderIdToCLDMap* ClassLoaderAotIdTable::_id_cld_map = nullptr;

void ClassLoaderAotIdTable::create_table() {
  if (!CDSConfig::supports_custom_loaders()) {
    // nothing to do if custom loader support is not enabled
    return;
  }
  assert(_loader_id_table == nullptr, "loader id table already created");
  assert(_id_cld_map == nullptr, "id to cld map already created");
  _loader_id_table = new (mtClass) LoaderIdTable();
  _id_cld_map = new (mtClass) LoaderIdToCLDMap();
}

bool ClassLoaderAotIdTable::reserve_id(Symbol* id) {
  MutexLocker mu(ClassLoaderAotIdTable_lock, Mutex::_no_safepoint_check_flag);
  bool created = false;
  _loader_id_table->put_if_absent(id, &created);
  return created;
}

void ClassLoaderAotIdTable::unreserve_id(Symbol* id) {
  assert(_loader_id_table->contains(id), "id is not present in the table");
  _loader_id_table->remove(id);
}

bool ClassLoaderAotIdTable::add_entry(Symbol* id, ClassLoaderData* cld) {
  bool result = _id_cld_map->put(id, cld);
  return result;
}

ClassLoaderData* ClassLoaderAotIdTable::get_cld(Symbol* id) {
  ClassLoaderData** cld = _id_cld_map->get(id);
  if (cld != nullptr) {
    return *cld;
  }
  return nullptr;
}

bool ClassLoaderAotIdTable::contains(Symbol* id) {
  return get_cld(id) != nullptr ? true : false;
}

void ClassLoaderAotIdTable::all_symbols_do(MetaspaceClosure* it) {
  if (_id_cld_map != nullptr) {
    _id_cld_map->iterate_all([&](Symbol*& loader_id, ClassLoaderData*& cld) {
      it->push(&loader_id);
    });
  }
}

static const unsigned INITIAL_TABLE_SIZE = 997; // prime number
static const unsigned MAX_TABLE_SIZE     = 10000;

static AOTLinkedCustomLoaderClassesMap* _custom_loader_classes_map = nullptr;
static ArchivedCustomLoaderInfoMap _archived_custom_loader_info_map;

ArchivedCustomLoaderInfoMap* CustomLoaderSupport::archived_cl_info_map() {
  return &_archived_custom_loader_info_map;
}

Array<AOTClassLocation*>* CustomLoaderInfo::archive_classpath(ClassLoaderData* cld) {
  GrowableArrayView<AOTClassLocation*>* locations = cld->aot_locations();
  assert(locations != nullptr, "AOTClassLocation not set");
  Array<AOTClassLocation*>* archived_copy = ArchiveBuilder::new_ro_array<AOTClassLocation*>(locations->length());
  for (int i = 0; i < locations->length(); i++) {
    archived_copy->at_put(i, locations->at(i)->write_to_archive());
    ArchivePtrMarker::mark_pointer((address*)archived_copy->adr_at(i));
  }
  return archived_copy;
}

CustomLoaderInfo* CustomLoaderInfo::allocate(Symbol* aot_id, ClassLoaderData* cld, GrowableArrayView<InstanceKlass*>* table) {
  ArchiveBuilder* builder = ArchiveBuilder::current();
  assert(ArchiveBuilder::current() != nullptr, "ArchiveBuilder is not yet created");

  Array<AOTClassLocation*>* locations = archive_classpath(cld);

  Array<InstanceKlass*>* class_list = ArchiveUtils::archive_array(table);
  ArchivedClassLoaderData* archived_cld = ClassLoaderDataShared::get_archived_cld(aot_id);
  assert(archived_cld != nullptr, "ArchivedClassLoaderData is missing");

  CustomLoaderInfo* cl_info = (CustomLoaderInfo*)ArchiveBuilder::ro_region_alloc(sizeof(CustomLoaderInfo));
  cl_info->init(builder->get_buffered_addr(aot_id), locations, class_list, archived_cld);
  cl_info->mark_pointers();
  return cl_info;
}

bool CustomLoaderInfo::check_classpath() {
  for (int i = 0; i < _cp_locations->length(); i++) {
    AOTClassLocation* location = _cp_locations->at(i);
    const char* archived_path = location->path();
    if (!location->check(archived_path, true)) {
      ResourceMark rm;
      char* loader_id_str = aot_id()->as_C_string();
      aot_log_warning(aot)("Classpath check failed for classloader with id %s for entry %s", loader_id_str, archived_path);
      return false;
    }
  }
  return true;
}

bool CustomLoaderInfo::match_classpath(const char* runtime_cp) {
  URLClassLoaderClassLocationStream uccs(runtime_cp);
  if (uccs.size() > _cp_locations->length()) {
    aot_log_warning(aot)("URLClassLoader classpath validation failed (reason: runtime classpath has more elements than the archived classpath)");
    return false;
  }
  if (uccs.size() < _cp_locations->length()) {
    aot_log_warning(aot)("URLClassLoader classpath validation failed (reason: runtime classpath has fewer elements than the archived classpath)");
    return false;
  }
  uccs.start();
  for (int i = 0; i < _cp_locations->length(); i++) {
    AOTClassLocation* location = _cp_locations->at(i);
    const char* archived_path = location->path();
    const char* runtime_path = uccs.get_next();
    if (!os::same_files(location->path(), runtime_path)) {
      aot_log_warning(aot)("URLClassLoader classpath validation failed (reason: same file check failed)");
      return false;
    }
  }
  return true;
}

void AOTLinkedCustomLoaderClassesMap::add_class(Symbol* loader_id, InstanceKlass* ik) {
  assert(loader_id != nullptr, "sanity check");
  ClassList** class_list_ptr = get(loader_id);
  ClassList* class_list = nullptr;
  if (class_list_ptr != nullptr) {
    class_list = *class_list_ptr;
  } else {
    class_list = new ClassList(1000);
    put(loader_id, class_list);
  }
  class_list->append(ik);
}

void AOTLinkedCustomLoaderClassesMap::write_to_archive(ArchivedCustomLoaderInfoMap* archived_map, const char* map_name) {
  CompactHashtableStats stats;
  CompactHashtableWriter writer(number_of_entries(), &stats);
  iterate_all([&] (Symbol*& loader_id, ClassList*& table) {
    ClassLoaderData* cld = table->at(0)->class_loader_data();
    assert(cld->aot_identity() == loader_id, "must be");
    CustomLoaderInfo* cl_info = CustomLoaderInfo::allocate(loader_id, cld, table);
    unsigned int hash = Symbol::symbol_hash(loader_id);
    writer.add(hash, AOTCompressedPointers::encode_not_null((address)cl_info));
  });
  writer.dump(archived_map, map_name);
}

void CustomLoaderSupport::initialize() {
  assert(CDSConfig::supports_custom_loaders(), "sanity check");
  if (CDSConfig::is_dumping_final_static_archive()) {
    _custom_loader_classes_map = new (mtClass) AOTLinkedCustomLoaderClassesMap(INITIAL_TABLE_SIZE, MAX_TABLE_SIZE);
  }
}

void CustomLoaderSupport::add_to_custom_loader_map(InstanceKlass* ik) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be in assembly phase");
  assert(CDSConfig::supports_custom_loaders(), "custom loader support is not enabled");
  assert(_custom_loader_classes_map != nullptr, "must be");

  Symbol* loader_id = ik->class_loader_data()->aot_identity();
  if (loader_id != nullptr) {
    _custom_loader_classes_map->add_class(loader_id, ik);
  }
}

void CustomLoaderSupport::archive_custom_loader_info() {
  assert(CDSConfig::supports_custom_loaders(), "custom loader support is not enabled");
  assert(_custom_loader_classes_map != nullptr, "sanity check");

  _custom_loader_classes_map->write_to_archive(&_archived_custom_loader_info_map, "archived custom loader info");

  if (log_is_enabled(Info, aot, link)) {
    ResourceMark rm;
    _custom_loader_classes_map->iterate_all([&](Symbol* loader_id, ClassList* class_list) {
      log_info(aot, link)("wrote %d classes for class loader with id=\"%s\"", class_list->length(), loader_id->as_C_string());
      for (int i = 0; i < class_list->length(); i++) {
        InstanceKlass* ik = class_list->at(i);
        log_info(aot, link)("  %s", ik->external_name());
      }
    });
  }
}

void CustomLoaderSupport::serialize_custom_loader_info_map_header(SerializeClosure* soc) {
  _archived_custom_loader_info_map.serialize_header(soc);
}

CustomLoaderInfo* CustomLoaderSupport::get_archived_classloader_info(Symbol* aot_id) {
  assert(CDSConfig::supports_custom_loaders(), "custom loader support is not enabled");
  CustomLoaderInfo* cl_info = _archived_custom_loader_info_map.get_loader_info(aot_id);
  return cl_info;
}

// Used only during production run
bool CustomLoaderSupport::is_scratch_loader(oop loader) {
  assert(CDSConfig::is_using_aot_linked_classes(), "sanity check");
  assert(CDSConfig::supports_custom_loaders(), "custom loader support is not enabled");

  bool found = false;
  _archived_custom_loader_info_map.iterate([&](CustomLoaderInfo* cl_info) {
    if (HeapShared::get_root(cl_info->archived_loader_obj_index()) == loader) {
      found = true;
      // match found; stop iterating
      return false;
    }
    // continue iterating
    return true;
  });
  return found;
}

bool CustomLoaderSupport::patch_loader_and_link_classes(Handle h_loader, Symbol* aot_id, const char* classpath) {
  JavaThread* THREAD = JavaThread::current();
  ClassLoaderData* loader_data = ClassLoaderAotIdTable::get_cld(aot_id);
  // It is possible that ClassLoaderData for this aot_id was not created by AOTLinkedClassBulkLoader::preload_classes_for_custom_loaders()
  // due to classpath check failure. If so, return failure.
  if (loader_data == nullptr) {
    return false;
  }
  CustomLoaderInfo* cl_info = _archived_custom_loader_info_map.get_loader_info(aot_id);
  assert(cl_info != nullptr, "cannot be null");
  if (!cl_info->match_classpath(classpath)) {
    return false;
  }
  loader_data->patch_class_loader_object(h_loader);
  AOTLinkedClassBulkLoader::patch_loader_in_preloaded_classes(loader_data, h_loader, cl_info, CHECK_AND_CLEAR_(false));
  AOTLinkedClassBulkLoader::link_classes_for_loader(loader_data, cl_info, CHECK_AND_CLEAR_(false));
  return true;
}

#endif // INCLUDE_CDS_JAVA_HEAP
