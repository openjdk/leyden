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

#ifndef SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP
#define SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP

#include "cds/archiveUtils.hpp"
#include "classfile/classLoaderDataShared.hpp"
#include "classfile/compactHashtable.hpp"
#include "oops/array.hpp"
#include "oops/symbol.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/hashTable.hpp"
#include "utilities/resizableHashTable.hpp"

class AOTClassLocation;
class MetaspaceClosure;
class SerializeClosure;

class ClassLoaderAotIdTable : AllStatic {
private:
  static const int TABLE_SIZE = 17; // prime number
  using LoaderIdTable = HashTable<Symbol*, ClassLoaderData*, TABLE_SIZE, AnyObj::C_HEAP, mtClass>;
  static LoaderIdTable* _loader_id_table;
public:
  static void create_table();
  static bool reserve_id(Symbol* id);
  static void unreserve_id(Symbol* id);
  static bool add_entry(Symbol* id, ClassLoaderData* cld);
  static ClassLoaderData* get_cld(Symbol* id);
  static bool contains(Symbol* id);
};

// Created in assembly phase to record custom class loader information.
class CustomLoaderInfo {
private:
  Symbol* _aot_id;
  Array<AOTClassLocation*>* _cp_locations;
  Array<InstanceKlass*>* _class_list;
  ArchivedClassLoaderData _archived_cld;

  void mark_pointers() {
    ArchivePtrMarker::mark_pointer(aot_id_addr());
    ArchivePtrMarker::mark_pointer(locations_addr());
    ArchivePtrMarker::mark_pointer(class_list_addr());
    ArchivePtrMarker::mark_pointer(_archived_cld.packages_addr());
    ArchivePtrMarker::mark_pointer(_archived_cld.modules_addr());
    ArchivePtrMarker::mark_pointer(_archived_cld.unnamed_module_addr());
  }

  // private constructor; use allocate() to create an instance
  CustomLoaderInfo() {}

  static Array<AOTClassLocation*>* archive_classpath(ClassLoaderData* cld);

public:
  static CustomLoaderInfo* allocate(Symbol* aot_id, ClassLoaderData* cld, GrowableArrayView<InstanceKlass*>* class_list);

  void init(Symbol* aot_id, Array<AOTClassLocation*>* locations, Array<InstanceKlass*>* class_list, ArchivedClassLoaderData* archived_cld) {
    _aot_id = aot_id;
    _cp_locations = locations;
    _class_list = class_list;
    _archived_cld = *archived_cld;
  }

  Symbol* aot_id() const { return _aot_id; }
  Array<AOTClassLocation*>* locations() const { return _cp_locations; }
  Array<InstanceKlass*>* class_list() const { return _class_list; }
  ArchivedClassLoaderData* archived_cld() { return &_archived_cld; }
  int archived_loader_obj_index() const { return _archived_cld.archived_loader_obj_index(); }

  address* aot_id_addr() const { return (address*)&_aot_id; }
  address* locations_addr() const { return (address*)&_cp_locations; }
  address* class_list_addr() const { return (address*)&_class_list; }

  bool verify_classpath(const char* classpath);
};

inline bool custom_loader_info_equals(CustomLoaderInfo* cl_info, Symbol* aot_id, int unused) {
  return cl_info->aot_id()->equals(aot_id);
}

class ArchivedCustomLoaderInfoMap : public OffsetCompactHashtable<Symbol*, CustomLoaderInfo*, custom_loader_info_equals>
{
public:
  CustomLoaderInfo* get_loader_info(Symbol* aot_id) {
    unsigned int hash = Symbol::symbol_hash(aot_id);
    return lookup(aot_id, hash, /*unused*/ 0);
  }
};

using ClassList = GrowableArrayCHeap<InstanceKlass*, mtClassShared>;

class AOTLinkedCustomLoaderClassesMap : public ResizeableHashTable<Symbol*, ClassList*, AnyObj::C_HEAP, mtClass>
{
  using ResizeableHashTableBase = ResizeableHashTable<Symbol*, ClassList*, AnyObj::C_HEAP, mtClass>;

public:
  AOTLinkedCustomLoaderClassesMap(unsigned size, unsigned max_size) : ResizeableHashTableBase(size, max_size) {}

  void add_class(Symbol* loader_id, InstanceKlass* ik);
  void write_to_archive(ArchivedCustomLoaderInfoMap* archived_map, const char* map_name);
};

class CustomLoaderSupport: AllStatic {
public:
  static void initialize();
  static void add_to_custom_loader_map(InstanceKlass* ik);
  static CustomLoaderInfo* find_loader_info(Symbol* aot_id, const char* classpath);
  static void archive_custom_loader_info();
  static void all_symbols_do(MetaspaceClosure* it);
  static void serialize_custom_loader_info_map_header(SerializeClosure* soc);
  static CustomLoaderInfo* get_archived_classloader_info(Symbol* aot_id);
  static bool is_scratch_loader(oop loader);
};

#endif // SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP
