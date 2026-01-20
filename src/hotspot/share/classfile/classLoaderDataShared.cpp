 /*
 * Copyright (c) 2020, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/aotLogging.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/heapShared.inline.hpp"
#include "cds/serializeClosure.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/classLoaderDataShared.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/modules.hpp"
#include "classfile/packageEntry.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "logging/log.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/safepoint.hpp"

#if INCLUDE_CDS_JAVA_HEAP

bool ClassLoaderDataShared::_full_module_graph_loaded = false;

class ArchivedClassLoaderData {
  Array<PackageEntry*>* _packages;
  Array<ModuleEntry*>* _modules;
  ModuleEntry* _unnamed_module;
  Symbol* _aot_identity;

  void assert_valid(ClassLoaderData* loader_data) {
    // loader_data may be null if the boot layer has loaded no modules for the platform or
    // system loaders (e.g., if you create a custom JDK image with only java.base).
    if (loader_data != nullptr) {
      assert(!loader_data->has_class_mirror_holder(),
             "loaders for non-strong hidden classes not supported");
    }
  }
public:
  ArchivedClassLoaderData() : _packages(nullptr), _modules(nullptr), _unnamed_module(nullptr) {}

  void iterate_symbols(ClassLoaderData* loader_data, MetaspaceClosure* closure);
  void allocate(ClassLoaderData* loader_data);
  void init_archived_entries(ClassLoaderData* loader_data);
  ModuleEntry* unnamed_module() {
    return _unnamed_module;
  }
  Symbol* aot_identity() { return _aot_identity; }

  void serialize(SerializeClosure* f) {
    f->do_ptr(&_packages);
    f->do_ptr(&_modules);
    f->do_ptr(&_unnamed_module);
  }

  void restore(ClassLoaderData* loader_data, bool do_entries, bool do_oops);
  void clear_archived_oops();
};

static ArchivedClassLoaderData _archived_boot_loader_data;
static ArchivedClassLoaderData _archived_platform_loader_data;
static ArchivedClassLoaderData _archived_system_loader_data;

static ModuleEntry* _archived_javabase_moduleEntry = nullptr;
static int _platform_loader_root_index = -1;
static int _system_loader_root_index = -1;

static bool archived_cld_map_equals_fn(ArchivedClassLoaderData* archived_cld, Symbol* sym, int len) {
  return archived_cld->aot_identity()->equals(sym);
}

using ArchivedAOTIdToCLDataMap = OffsetCompactHashtable<Symbol*, ArchivedClassLoaderData*, archived_cld_map_equals_fn>;
static ArchivedAOTIdToCLDataMap _aotid_to_archived_cld_map;

void ArchivedClassLoaderData::iterate_symbols(ClassLoaderData* loader_data, MetaspaceClosure* closure) {
  assert(CDSConfig::is_dumping_full_module_graph(), "must be");
  assert_valid(loader_data);
  if (loader_data != nullptr) {
    loader_data->packages()->iterate_symbols(closure);
    loader_data->modules() ->iterate_symbols(closure);
    loader_data->unnamed_module()->iterate_symbols(closure);
  }
}

void ArchivedClassLoaderData::allocate(ClassLoaderData* loader_data) {
  assert(CDSConfig::is_dumping_full_module_graph(), "must be");
  assert_valid(loader_data);
  if (loader_data != nullptr) {
    // We can't create hashtables at dump time because the hashcode depends on the
    // address of the Symbols, which may be relocated at runtime due to ASLR.
    // So we store the packages/modules in Arrays. At runtime, we create
    // the hashtables using these arrays.
    _packages = loader_data->packages()->allocate_archived_entries();
    if (loader_data->modules(false) != nullptr) {
      _modules = loader_data->modules(false)->allocate_archived_entries();
    }
    _unnamed_module = loader_data->unnamed_module()->allocate_archived_entry();
    if (!loader_data->is_builtin_class_loader_data()) {
      _aot_identity = loader_data->aot_identity();
    }
    ArchivePtrMarker::mark_pointer((address*)&_packages);
    ArchivePtrMarker::mark_pointer((address*)&_modules);
    ArchivePtrMarker::mark_pointer((address*)&_unnamed_module);
    ArchivePtrMarker::mark_pointer((address*)&_aot_identity);
  }
}

void ArchivedClassLoaderData::init_archived_entries(ClassLoaderData* loader_data) {
  assert(CDSConfig::is_dumping_full_module_graph(), "must be");
  assert_valid(loader_data);
  if (loader_data != nullptr) {
    loader_data->packages()->init_archived_entries(_packages);
    if (loader_data->modules(false) != nullptr) {
      loader_data->modules(false) ->init_archived_entries(_modules);
    }
    _unnamed_module->init_as_archived_entry();
    if (!loader_data->is_builtin_class_loader_data()) {
      _aot_identity = ArchiveBuilder::get_buffered_symbol(_aot_identity);
      assert(_aot_identity != nullptr, "sanity check");
    }
  }
}

void ArchivedClassLoaderData::restore(ClassLoaderData* loader_data, bool do_entries, bool do_oops) {
  assert(CDSConfig::is_using_full_module_graph(), "must be");
  assert_valid(loader_data);
  if (_modules != nullptr) { // Could be null if we have archived no modules for platform/system loaders
    ModuleEntryTable* modules = loader_data->modules();
    PackageEntryTable* packages = loader_data->packages();

    MutexLocker m1(Module_lock);
    if (do_entries) {
      modules->load_archived_entries(loader_data, _modules);
      packages->load_archived_entries(_packages);
    }
    if (do_oops) {
      modules->restore_archived_oops(loader_data, _modules);
      if (_unnamed_module != nullptr) {
        oop module_oop = _unnamed_module->module_oop();
        assert(module_oop != nullptr, "must be already set");
        assert(_unnamed_module == java_lang_Module::module_entry(module_oop), "must be already set");
        assert(loader_data->class_loader() == java_lang_Module::loader(module_oop), "must be set in dump time");
      }
    }
  }
}

void ArchivedClassLoaderData::clear_archived_oops() {
  assert(!CDSConfig::is_using_full_module_graph(), "must be");
  if (_modules != nullptr) {
    for (int i = 0; i < _modules->length(); i++) {
      _modules->at(i)->clear_archived_oops();
    }
    if (_unnamed_module != nullptr) {
      _unnamed_module->clear_archived_oops();
    }
  }
}

// ------------------------------

void ClassLoaderDataShared::load_archived_platform_and_system_class_loaders() {

#if INCLUDE_CDS_JAVA_HEAP
  // The streaming object loader prefers loading the class loader related objects before
  //  the CLD constructor which has a NoSafepointVerifier.
  if (!HeapShared::is_loading_streaming_mode()) {
    return;
  }

  // Ensure these class loaders are eagerly materialized before their CLDs are created.
  HeapShared::get_root(_platform_loader_root_index, false /* clear */);
  HeapShared::get_root(_system_loader_root_index, false /* clear */);

  if (Universe::is_module_initialized() || !CDSConfig::is_using_full_module_graph()) {
    return;
  }

  // When using the full module graph, we need to load unnamed modules too.
  ModuleEntry* platform_loader_module_entry = _archived_platform_loader_data.unnamed_module();
  if (platform_loader_module_entry != nullptr) {
    platform_loader_module_entry->preload_archived_oops();
  }

  ModuleEntry* system_loader_module_entry = _archived_system_loader_data.unnamed_module();
  if (system_loader_module_entry != nullptr) {
    system_loader_module_entry->preload_archived_oops();
  }
#endif
}

static ClassLoaderData* null_class_loader_data() {
  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  assert(loader_data != nullptr, "must be");
  return loader_data;
}

static ClassLoaderData* java_platform_loader_data_or_null() {
  return ClassLoaderData::class_loader_data_or_null(SystemDictionary::java_platform_loader());
}

static ClassLoaderData* java_system_loader_data_or_null() {
  return ClassLoaderData::class_loader_data_or_null(SystemDictionary::java_system_loader());
}

// ModuleEntryTables (even if empty) are required for iterate_symbols() to scan the
// platform/system loaders inside the CDS safepoint, but the tables can be created only
// when outside of safepoints. Let's do that now.
void ClassLoaderDataShared::ensure_module_entry_tables_exist() {
  assert(!SafepointSynchronize::is_at_safepoint(), "sanity");
  ensure_module_entry_table_exists(SystemDictionary::java_platform_loader());
  ensure_module_entry_table_exists(SystemDictionary::java_system_loader());
}

void ClassLoaderDataShared::ensure_module_entry_table_exists(oop class_loader) {
  Handle h_loader(JavaThread::current(), class_loader);
  ModuleEntryTable* met = Modules::get_module_entry_table(h_loader);
  assert(met != nullptr, "sanity");
}

class CLDSymbolsIterator : public CLDClosure {
  MetaspaceClosure* _closure;
 public:
  CLDSymbolsIterator(MetaspaceClosure* closure) : _closure(closure) {}
  void do_cld(ClassLoaderData* cld) {
    if (cld->aot_identity() != nullptr) {
      cld->packages()->iterate_symbols(_closure);
      if (cld->modules(false) != nullptr) {
        cld->modules(false)->iterate_symbols(_closure);
      }
      cld->unnamed_module()->iterate_symbols(_closure);
    }
  }
};

void ClassLoaderDataShared::iterate_symbols(MetaspaceClosure* closure) {
  assert(CDSConfig::is_dumping_full_module_graph(), "must be");
  _archived_boot_loader_data.iterate_symbols    (null_class_loader_data(), closure);
  _archived_platform_loader_data.iterate_symbols(java_platform_loader_data_or_null(), closure);
  _archived_system_loader_data.iterate_symbols  (java_system_loader_data_or_null(), closure);

  CLDSymbolsIterator sym_iterator(closure);
  ClassLoaderDataGraph::cld_do(&sym_iterator);
}

using ArchivedCLDToCLDMap = HashTable<ArchivedClassLoaderData*, ClassLoaderData*, 5, AnyObj::C_HEAP, mtClassShared>;
ArchivedCLDToCLDMap* _archived_cld_to_runtime_cld_map = nullptr;

class ArchiveAOTCompatibleLoaders : public CLDClosure {
 public:
  void do_cld(ClassLoaderData* cld) {
    if (!cld->is_builtin_class_loader_data() && cld->aot_identity() != nullptr) {
      ArchivedClassLoaderData* archived_entry = (ArchivedClassLoaderData*)ArchiveBuilder::rw_region_alloc(sizeof(ArchivedClassLoaderData));
      archived_entry->allocate(cld);
      _archived_cld_to_runtime_cld_map->put(archived_entry, cld);
    }
  }
};

void ClassLoaderDataShared::allocate_archived_tables() {
  assert(CDSConfig::is_dumping_full_module_graph(), "must be");
  _archived_boot_loader_data.allocate    (null_class_loader_data());
  _archived_platform_loader_data.allocate(java_platform_loader_data_or_null());
  _archived_system_loader_data.allocate  (java_system_loader_data_or_null());

  _archived_cld_to_runtime_cld_map = new (mtClassShared) ArchivedCLDToCLDMap();
  if (CDSConfig::supports_custom_loaders()) {
    ArchiveAOTCompatibleLoaders archive_closure;
    ClassLoaderDataGraph::cld_do(&archive_closure);
  }
}

void ClassLoaderDataShared::init_archived_tables() {
  assert(CDSConfig::is_dumping_full_module_graph(), "must be");
  _archived_boot_loader_data.init_archived_entries    (null_class_loader_data());
  _archived_platform_loader_data.init_archived_entries(java_platform_loader_data_or_null());
  _archived_system_loader_data.init_archived_entries  (java_system_loader_data_or_null());

  _archived_cld_to_runtime_cld_map->iterate_all([&](ArchivedClassLoaderData* &archived_cld, ClassLoaderData* &cld) {
    archived_cld->init_archived_entries(cld);
  });

  _archived_javabase_moduleEntry = ModuleEntry::get_archived_entry(ModuleEntryTable::javabase_moduleEntry());

  _platform_loader_root_index = HeapShared::append_root(SystemDictionary::java_platform_loader());
  _system_loader_root_index = HeapShared::append_root(SystemDictionary::java_system_loader());
}

void ClassLoaderDataShared::write_cld_table() {
  CompactHashtableStats stats;
  _aotid_to_archived_cld_map.reset();
  CompactHashtableWriter writer(_archived_cld_to_runtime_cld_map->number_of_entries(), &stats);
  _archived_cld_to_runtime_cld_map->iterate_all([&](ArchivedClassLoaderData* &archived_cld, ClassLoaderData* &cld) {
    ResourceMark rm;
    Symbol* aot_id = cld->aot_identity();
    assert(aot_id != nullptr, "sanity check");
    ArchiveBuilder* builder = ArchiveBuilder::current();
    assert(builder->get_buffered_addr(aot_id) != nullptr, "Symbol %s is not in the buffer", aot_id->as_C_string());
    unsigned int hash = Symbol::symbol_hash(aot_id);
    u4 delta = builder->buffer_to_offset_u4((address)archived_cld);
    writer.add(hash, delta);
    if (log_is_enabled(Trace, aot, hashtables)) {
      log_trace(aot, hashtables)("archived cld dictionary: %s", aot_id->as_C_string());
    }
  });
  writer.dump(&_aotid_to_archived_cld_map, "aot_cld_map dictionary");
}

void ClassLoaderDataShared::serialize(SerializeClosure* f) {
  _archived_boot_loader_data.serialize(f);
  _archived_platform_loader_data.serialize(f);
  _archived_system_loader_data.serialize(f);
  _aotid_to_archived_cld_map.serialize_header(f);
  f->do_ptr(&_archived_javabase_moduleEntry);
  f->do_int(&_platform_loader_root_index);
  f->do_int(&_system_loader_root_index);
}

ModuleEntry* ClassLoaderDataShared::archived_boot_unnamed_module() {
  if (CDSConfig::is_using_full_module_graph()) {
    return _archived_boot_loader_data.unnamed_module();
#if 0
    Symbol* boot_loader_aot_identity = SymbolTable::new_symbol("BOOT");
    unsigned int hash = Symbol::symbol_hash(boot_loader_aot_identity);
    ArchivedClassLoaderData* archived_cld = _aotid_to_archived_cld_map.lookup(boot_loader_aot_identity, hash, 0);
    assert(archived_cld != nullptr, "sanity check");
    return archived_cld->unnamed_module();
#endif
  } else {
    return nullptr;
  }
}

ModuleEntry* ClassLoaderDataShared::archived_unnamed_module(ClassLoaderData* loader_data) {
  ModuleEntry* archived_module = nullptr;

  if (CDSConfig::is_using_full_module_graph()) {
    if (!Universe::is_module_initialized()) {
      precond(_platform_loader_root_index >= 0);
      precond(_system_loader_root_index >= 0);

      if (loader_data->class_loader() == HeapShared::get_root(_platform_loader_root_index)) {
        archived_module = _archived_platform_loader_data.unnamed_module();
      } else if (loader_data->class_loader() == HeapShared::get_root(_system_loader_root_index)) {
        archived_module = _archived_system_loader_data.unnamed_module();
      }
    } else if (loader_data->aot_identity() != nullptr) {
      unsigned int hash = Symbol::symbol_hash(loader_data->aot_identity());
      ArchivedClassLoaderData* archived_cld = _aotid_to_archived_cld_map.lookup(loader_data->aot_identity(), hash, 0);
      if (archived_cld != nullptr) {
        archived_module = archived_cld->unnamed_module();
      }
    }
  }
  return archived_module;
}

void ClassLoaderDataShared::clear_archived_oops() {
  assert(!CDSConfig::is_using_full_module_graph(), "must be");
  _archived_boot_loader_data.clear_archived_oops();
  _archived_platform_loader_data.clear_archived_oops();
  _archived_system_loader_data.clear_archived_oops();

  _aotid_to_archived_cld_map.iterate([&](ArchivedClassLoaderData* archived_cld) {
    archived_cld->clear_archived_oops();
  });
  if (_platform_loader_root_index >= 0) {
    HeapShared::clear_root(_platform_loader_root_index);
    HeapShared::clear_root(_system_loader_root_index);
  }
}

// Must be done before ClassLoader::create_javabase()
void ClassLoaderDataShared::restore_archived_entries_for_null_class_loader_data() {
  precond(CDSConfig::is_using_full_module_graph());
  //unsigned int hash = Symbol::symbol_hash(null_class_loader_data()->aot_identity());
  //ArchivedClassLoaderData* archived_cld = _aotid_to_archived_cld_map.lookup(null_class_loader_data()->aot_identity(), hash, 0);
  //archived_cld->restore(null_class_loader_data(), true, false);
  _archived_boot_loader_data.restore(null_class_loader_data(), true, false);
  ModuleEntryTable::set_javabase_moduleEntry(_archived_javabase_moduleEntry);
  aot_log_info(aot)("use_full_module_graph = true; java.base = " INTPTR_FORMAT,
                    p2i(_archived_javabase_moduleEntry));
}

oop ClassLoaderDataShared::restore_archived_oops_for_null_class_loader_data() {
  assert(CDSConfig::is_using_full_module_graph(), "must be");
  //unsigned int hash = Symbol::symbol_hash(null_class_loader_data()->aot_identity());
  //ArchivedClassLoaderData* archived_cld = _aotid_to_archived_cld_map.lookup(null_class_loader_data()->aot_identity(), hash, 0);
  //archived_cld->restore(null_class_loader_data(), false, true);
  _archived_boot_loader_data.restore(null_class_loader_data(), false, true);
  //null_class_loader_data()->set_restored(true);
  return _archived_javabase_moduleEntry->module_oop();
}

void ClassLoaderDataShared::restore_java_platform_loader_from_archive(ClassLoaderData* loader_data) {
  assert(CDSConfig::is_using_full_module_graph(), "must be");
  //unsigned int hash = Symbol::symbol_hash(loader_data->aot_identity());
  //ArchivedClassLoaderData* archived_cld = _aotid_to_archived_cld_map.lookup(loader_data->aot_identity(), hash, 0);
  //assert(archived_cld != nullptr, "ArchivedClassLoaderData for platform loader not found");
  //archived_cld->restore(loader_data, true, true);
  _archived_platform_loader_data.restore(loader_data, true, true);
  loader_data->set_restored(true);
}

void ClassLoaderDataShared::restore_java_system_loader_from_archive(ClassLoaderData* loader_data) {
  assert(CDSConfig::is_using_full_module_graph(), "must be");
  //unsigned int hash = Symbol::symbol_hash(loader_data->aot_identity());
  //ArchivedClassLoaderData* archived_cld = _aotid_to_archived_cld_map.lookup(loader_data->aot_identity(), hash, 0);
  //assert(archived_cld != nullptr, "ArchivedClassLoaderData for system loader not found");
  //archived_cld->restore(loader_data, true, true);
  _archived_system_loader_data.restore(loader_data, true, true);
  _full_module_graph_loaded = true;
  loader_data->set_restored(true);
}

// This is called before AOTLinkedClassBulkLoader starts preloading classes. It makes sure that
// when we preload any class, its module is already valid.
void ClassLoaderDataShared::restore_archived_modules_for_preloading_classes(JavaThread* current) {
  precond(CDSConfig::is_using_aot_linked_classes());

  precond(_platform_loader_root_index >= 0);
  precond(_system_loader_root_index >= 0);

  Handle h_platform_loader(current, HeapShared::get_root(_platform_loader_root_index));
  Handle h_system_loader(current, HeapShared::get_root(_system_loader_root_index));
  Modules::init_archived_modules(current, h_platform_loader, h_system_loader);
}

void ClassLoaderDataShared::restore_custom_loader_archived_data(ClassLoaderData* loader_data) {
  assert(!loader_data->is_builtin_class_loader_data(), "should not be called for built-in loaders");
  //if already restored, nothing to do
  if (loader_data->restored()) {
    return;
  }
  ResourceMark rm;
  assert(CDSConfig::is_using_full_module_graph(), "must be");
  Symbol* aot_id = loader_data->aot_identity();
  assert(aot_id != nullptr, "sanity check");
  unsigned int hash = Symbol::symbol_hash(aot_id);
  ArchivedClassLoaderData* archived_cld = _aotid_to_archived_cld_map.lookup(aot_id, hash, 0);
  assert(archived_cld != nullptr, "ArchivedClassLoaderData for loader with aot_id=%s not found", aot_id->as_C_string());
  archived_cld->restore(loader_data, true, true);
  loader_data->set_restored(true);
}

#endif // INCLUDE_CDS_JAVA_HEAP
