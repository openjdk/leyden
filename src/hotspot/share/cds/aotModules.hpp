 /*
 * Copyright (c) 2020, 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_CDS_AOTMODULES_HPP
#define SHARE_CDS_AOTMODULES_HPP

#include "memory/allStatic.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/handles.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/macros.hpp"

class ClassLoaderData;
class CustomLoaderInfo;
class MetaspaceClosure;
class ModuleEntry;
class PackageEntry;
class SerializeClosure;

// This class is used for storing information of a ClassLoaderData into the AOT cache. In
// the production run, information is copied from here into a live ClassLoaderData,
class AOTClassLoaderData {
  Array<PackageEntry*>* _packages;
  Array<ModuleEntry*>* _modules;
  ModuleEntry* _unnamed_module;
  int _archived_loader_obj_index;

  void assert_valid(ClassLoaderData* loader_data);

public:
  ArchivedClassLoaderData() : _packages(nullptr), _modules(nullptr), _unnamed_module(nullptr), _archived_loader_obj_index(-1) {}

  address* packages_addr() const { return (address*)&_packages; }
  address* modules_addr() const { return (address*)&_modules; }
  address* unnamed_module_addr() const { return (address*)&_unnamed_module; }
  address* archived_loader_obj_index_addr() const { return (address*)&_archived_loader_obj_index; }

  void iterate_roots(MetaspaceClosure* closure);
  void build_tables(ClassLoaderData* loader_data, TRAPS);
  void remove_unshareable_info();
  ModuleEntry* unnamed_module() {
    return _unnamed_module;
  }

  void set_archived_loader_obj_index(int index) { _archived_loader_obj_index = index; }
  int archived_loader_obj_index() const { return _archived_loader_obj_index; }

  void serialize(SerializeClosure* f);
  void restore(ClassLoaderData* loader_data, bool do_entries, bool do_oops);
  void clear_archived_oops();
};

// This class manages the information (such as properties, module oops, ModuleEntry's, and ClassLoaderData's)
// for the archived module system.
class AOTModules : AllStatic {
  static bool _full_module_graph_loaded;
  CDS_JAVA_HEAP_ONLY(static void ensure_module_entry_table_exists(oop class_loader);)

#if INCLUDE_CDS_JAVA_HEAP
  class ArchivedProperty;

  static ArchivedProperty _archived_props[];
  static constexpr size_t num_archived_props();
  static ArchivedProperty& archived_prop(size_t i);
#endif

public:

  static void check_archived_module_oop(oop orig_module_obj) NOT_CDS_JAVA_HEAP_RETURN;
  static void define_archived_modules(Handle h_platform_loader, Handle h_system_loader,
                                      TRAPS) NOT_CDS_JAVA_HEAP_RETURN;
  static void init_archived_modules(JavaThread* current, Handle h_platform_loader, Handle h_system_loader)
                                   NOT_CDS_JAVA_HEAP_RETURN;
  static void dump_archived_module_info() NOT_CDS_JAVA_HEAP_RETURN;
  static void serialize_archived_module_info(SerializeClosure* soc) NOT_CDS_JAVA_HEAP_RETURN;

  static void check_restrictions(TRAPS) NOT_CDS_JAVA_HEAP_RETURN;
  static void load_archived_platform_and_system_class_loaders() NOT_CDS_JAVA_HEAP_RETURN;
  static void restore_archived_modules_for_preloading_classes(JavaThread* current) NOT_CDS_JAVA_HEAP_RETURN;
  static void build_tables(TRAPS) NOT_CDS_JAVA_HEAP_RETURN;
  static void iterate_roots(MetaspaceClosure* closure) NOT_CDS_JAVA_HEAP_RETURN;
  static void remove_unshareable_info() NOT_CDS_JAVA_HEAP_RETURN;
#if INCLUDE_CDS_JAVA_HEAP
  static void ensure_module_entry_tables_exist();
  static void serialize(SerializeClosure* f);
  static void clear_archived_oops();
  static void restore_archived_entries_for_null_class_loader_data();
  static oop  restore_archived_oops_for_null_class_loader_data();
  static void restore_java_platform_loader_from_archive(ClassLoaderData* loader_data);
  static void restore_java_system_loader_from_archive(ClassLoaderData* loader_data);
  static ModuleEntry* archived_boot_unnamed_module();
  static ModuleEntry* archived_unnamed_module(ClassLoaderData* loader_data);
  static void restore_custom_loader_data_from_archive(ClassLoaderData* loader_data, CustomLoaderInfo* cl_info);
  static ArchivedClassLoaderData* get_archived_cld(Symbol* loader_id);
  static void set_archived_index_for(oop loader);
#endif // INCLUDE_CDS_JAVA_HEAP
  static bool is_full_module_graph_loaded() { return _full_module_graph_loaded; }
};

#endif // SHARE_CDS_AOTMODULES_HPP
