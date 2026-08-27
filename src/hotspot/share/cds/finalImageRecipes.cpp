/*
 * Copyright (c) 2024, 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/aotClassLinker.hpp"
#include "cds/aotClassLocation.hpp"
#include "cds/aotConstantPoolResolver.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/archiveUtils.inline.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsProtectionDomain.hpp"
#include "cds/customLoaderSupport.hpp"
#include "cds/finalImageRecipes.hpp"
#include "cds/unregisteredClasses.hpp"
#include "cds/urlClassLoaderSupport.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/javaStackTraceClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"

static const unsigned INITIAL_TABLE_SIZE = 997; // prime number
static const unsigned MAX_TABLE_SIZE     = 10000;

static void mark_pointers_in_array(Array<InstanceKlassRecipe>* array) {
  if (array == nullptr) {
    return;
  }
  for (int i = 0; i < array->length(); i++) {
    InstanceKlassRecipe* recipe = array->adr_at(i);
    recipe->mark_pointers();
  }
}

class AotSafeLoaderClassRecipeMap: public ResizeableHashTable<Symbol*, ClassRecipeList*, AnyObj::C_HEAP, mtClass>
{
  using ResizeableHashTableBase = ResizeableHashTable<Symbol*, ClassRecipeList*, AnyObj::C_HEAP, mtClass>;
private:
public:
  AotSafeLoaderClassRecipeMap(unsigned size, unsigned max_size) : ResizeableHashTableBase(size, max_size) {}

  void add_class_recipe(Symbol* loader_id, InstanceKlassRecipe* ikr) {
    assert(loader_id != nullptr, "sanity check");
    ClassRecipeList** class_recipes_ptr = get(loader_id);
    ClassRecipeList* class_recipes = nullptr;
    if (class_recipes_ptr != nullptr) {
      class_recipes = *class_recipes_ptr;
    } else {
      class_recipes = new ClassRecipeList(1000);
      put(loader_id, class_recipes);
    }
    class_recipes->append(*ikr);
  }
};

AotSafeLoaderClassRecipeMap* _aot_safe_loader_class_recipes = nullptr;
FinalImageRecipes* _final_image_recipes = nullptr;

void InstanceKlassRecipes::log_recorded_classes(Array<InstanceKlassRecipe>* list) {
  if (list->length() != 0) {
    const char* category = AOTClassLinker::class_category_name(list->adr_at(0)->instance_klass());
    log_info(aot, link)("recorded %d class(es) for category %s", list->length(), category);
  }
}

void InstanceKlassRecipes::mark_pointers() {
  ArchivePtrMarker::mark_pointer(&_boot1);
  mark_pointers_in_array(_boot1);
  ArchivePtrMarker::mark_pointer(&_boot2);
  mark_pointers_in_array(_boot2);
  ArchivePtrMarker::mark_pointer(&_platform);
  mark_pointers_in_array(_platform);
  ArchivePtrMarker::mark_pointer(&_app);
  mark_pointers_in_array(_app);
  ArchivePtrMarker::mark_pointer(&_aot_unsafe_custom_loader_classes);
  mark_pointers_in_array(_aot_unsafe_custom_loader_classes);
}

void ClassLoaderRecipe::mark_pointers() {
  ArchivePtrMarker::mark_pointer(aot_id_addr());
  ArchivePtrMarker::mark_pointer(parent_aot_id_addr());
  ArchivePtrMarker::mark_pointer(cp_locations_addr());
  ArchivePtrMarker::mark_pointer(class_recipes_addr());
  mark_pointers_in_array(_class_recipes);
}

ClassLoaderRecipe* ClassLoaderRecipe::allocate(Symbol* aot_id, Symbol* parent_id, Array<InstanceKlassRecipe>* recipes, Array<AOTClassLocation*>* cp_locations) {
  size_t archived_size = sizeof(ClassLoaderRecipe); // +1 for null character
  ClassLoaderRecipe* archived_recipe = (ClassLoaderRecipe*)ArchiveBuilder::ro_region_alloc(archived_size);
  archived_recipe->init(aot_id, parent_id, recipes, cp_locations);
  return archived_recipe;
}

bool ClassLoaderRecipe::verify_classpath() {
  for (int i = 0; i < _cp_locations->length(); i++) {
    AOTClassLocation* location = _cp_locations->at(i);
    const char* archived_path = location->path();
    if (!location->check(archived_path, true)) {
      aot_log_warning(aot)("URLClassLoader classpath validation failed");
      return false;
    }
  }
  return true;

}

void* FinalImageRecipes::operator new(size_t size) throw() {
  return ArchiveBuilder::current()->ro_region_alloc(size);
}

Array<AOTClassLocation*>* FinalImageRecipes::archive_classpath(ClassLoaderData* cld) {
  GrowableArrayView<AOTClassLocation*>* locations = cld->aot_locations();
  assert(locations != nullptr, "AOTClassLocation not set");
  Array<AOTClassLocation*>* archived_copy = ArchiveBuilder::new_ro_array<AOTClassLocation*>(locations->length());
  for (int i = 0; i < locations->length(); i++) {
    archived_copy->at_put(i, locations->at(i)->write_to_archive());
    ArchivePtrMarker::mark_pointer((address*)archived_copy->adr_at(i));
  }
  return archived_copy;
}

void FinalImageRecipes::record_class_recipes() {
  _class_recipes = (InstanceKlassRecipes*)ArchiveBuilder::ro_region_alloc(sizeof(InstanceKlassRecipes));
  ArchivePtrMarker::mark_pointer(&_class_recipes);
  write_recipes(_class_recipes);
  record_aot_safe_loader_classes();
  _class_recipes->mark_pointers();
}

void FinalImageRecipes::record_aot_safe_loader_classes() {
  _aot_safe_loader_class_recipes = new (mtClass) AotSafeLoaderClassRecipeMap(INITIAL_TABLE_SIZE, MAX_TABLE_SIZE);

  GrowableArray<Klass*>* all_classes = ArchiveBuilder::current()->klasses();
  for (int i = 0; i < all_classes->length(); i++) {
    Klass* k = all_classes->at(i);
    if (k->is_instance_klass() && InstanceKlass::cast(k)->defined_by_aot_safe_custom_loader()) {
      InstanceKlass* ik = InstanceKlass::cast(k);
      int flags = 0;
      Array<int>* cp_recipe = record_recipe_for_constantpool(ik, flags);
      InstanceKlassRecipe ikr(ArchiveBuilder::current()->get_buffered_addr(ik), cp_recipe, flags);

      Symbol* loader_id = ik->classloader_aot_id();
      assert(loader_id != nullptr, "must be");
      _aot_safe_loader_class_recipes->add_class_recipe(loader_id, &ikr);
    }
  }
  if (log_is_enabled(Info, aot, load)) {
    _aot_safe_loader_class_recipes->iterate_all([&](Symbol* loader_id, ClassRecipeList* table) {
      ResourceMark rm;
      for (int i = 0; i < table->length(); i++) {
        InstanceKlassRecipe* ikr = table->adr_at(i);
        InstanceKlass* ik = ikr->instance_klass();
        log_info(aot, load)("category %s[%d] %s", loader_id->as_C_string(), i, ik->external_name());
      }
    });
  }
  GrowableArray<ClassLoaderRecipe*> cl_recipes;
  // Either the entries be written in parent-first order, or the code for loading the entries be updated to
  // ensure parent loader's class recipes are processed first.
  // It is fine for now because only the bootloader or system loader can be the parent, and they both are processed
  // before any custom loader.
  _aot_safe_loader_class_recipes->iterate_all([&](Symbol*& loader_id, ClassRecipeList*& class_recipes) {
    InstanceKlass* source_ik = ArchiveBuilder::current()->get_source_addr(class_recipes->at(0).instance_klass());
    ClassLoaderData* cld = source_ik->class_loader_data();
    Symbol *parent_id = cld->parent_aot_id();
    assert(parent_id != nullptr, "Parent loader must have aot-id");
    Array<AOTClassLocation*>* cp_locations = archive_classpath(cld);
    ClassLoaderRecipe* cl_recipe = ClassLoaderRecipe::allocate(ArchiveBuilder::current()->get_buffered_addr(loader_id),
                                                               ArchiveBuilder::current()->get_buffered_addr(parent_id),
                                                               ArchiveUtils::archive_array(class_recipes), cp_locations);
    cl_recipes.append(cl_recipe);
  });
  _class_loader_recipes = ArchiveUtils::archive_array(&cl_recipes);
  ArchivePtrMarker::mark_pointer(&_class_loader_recipes);
}

void FinalImageRecipes::write_recipes(InstanceKlassRecipes* table) {
  ResourceMark rm;
  GrowableArray<Klass*>* all_classes = ArchiveBuilder::current()->klasses();
  GrowableArray<InstanceKlassRecipe> boot1_list, boot2_list, plat_list, app_list, aot_unsafe_cl_list;

  for (int i = 0; i < all_classes->length(); i++) {
    Klass* k = all_classes->at(i);
    if (k->is_instance_klass()) {
      InstanceKlass* ik = InstanceKlass::cast(k);

      // InstanceKlassRecipes for custom loader classes are written later
      if (ik->defined_by_aot_safe_custom_loader()) {
        continue;
      }

      oop class_loader = ik->class_loader();
      int flags = 0;
      Array<int>* cp_recipe = record_recipe_for_constantpool(ik, flags);
      InstanceKlassRecipe recipe(ArchiveBuilder::current()->get_buffered_addr(ik), cp_recipe, flags);
      GrowableArray<InstanceKlassRecipe>* list = nullptr;

      if (SystemDictionary::is_boot_class_loader(class_loader)) {
        if (ik->module() == ModuleEntryTable::javabase_moduleEntry()) {
          list = &boot1_list;
        } else {
          list = &boot2_list;
        }
      } else if (SystemDictionary::is_platform_class_loader(class_loader)) {
        list = &plat_list;
      } else if (SystemDictionary::is_system_class_loader(class_loader)) {
        list = &app_list;
      } else if (!ik->defined_by_aot_safe_custom_loader()) {
        list = &aot_unsafe_cl_list;
      }
      assert(list != nullptr, "sanity check");
      list->append(recipe);
      const char* category = AOTClassLinker::class_category_name(ik);
      log_info(aot, load)("category %s[%d] %s", category, list->length()-1, ik->external_name());
    }
  }

  table->set_boot1(ArchiveUtils::archive_array(&boot1_list));
  table->set_boot2(ArchiveUtils::archive_array(&boot2_list));
  table->set_platform(ArchiveUtils::archive_array(&plat_list));
  table->set_app(ArchiveUtils::archive_array(&app_list));
  table->set_aot_unsafe_custom_loader_classes(ArchiveUtils::archive_array(&aot_unsafe_cl_list));
}

Array<int>* FinalImageRecipes::record_recipe_for_constantpool(InstanceKlass* ik, int& flags) {
  ConstantPool* cp = ik->constants();
  ConstantPoolCache* cp_cache = cp->cache();
  GrowableArray<int> cp_indices;

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

  if (cp_indices.length() > 0) {
    LogStreamHandle(Trace, aot, resolve) log;
    if (log.is_enabled()) {
      log.print("ConstantPool entries for %s to be pre-resolved:", ik->external_name());
      for (int i = 0; i < cp_indices.length(); i++) {
        log.print(" %d", cp_indices.at(i));
      }
      log.print("\n");
    }
    return ArchiveUtils::archive_array(&cp_indices);
  } else {
    return nullptr;
  }
}

void FinalImageRecipes::load_classes_in_table(Array<InstanceKlassRecipe>* recipes, const char* category_name,
                                              Handle loader, Array<AOTClassLocation*>* class_locations, TRAPS) {
  KlassMirrorDataCache mirror_data_cache(loader, class_locations, CHECK);

  for (int i = 0; i < recipes->length(); i++) {
    InstanceKlass* ik = recipes->adr_at(i)->instance_klass();
    if (ik->is_hidden()) {
      continue;
    }
    if (log_is_enabled(Info, aot, load)) {
      ResourceMark rm(THREAD);
      log_info(aot, load)("%-5s %s%s", category_name, ik->external_name(),
                          ik->is_hidden() ? " (hidden)" : "");
    }
    InstanceKlass* loaded_ik = SystemDictionary::find_instance_klass(THREAD, ik->name(), loader);
    if (loaded_ik == nullptr) {
      int index = ik->shared_classpath_index();
      PackageEntry* pkg_entry = KlassMirrorData::get_package_entry(ik, mirror_data_cache, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        log_warning(aot, load)("Exception occurred in getting package entry");
        CLEAR_PENDING_EXCEPTION;
        continue;
      }
      Handle pd;
      if (loader() != nullptr) {
        pd = KlassMirrorData::get_protection_domain(ik, pkg_entry, mirror_data_cache, THREAD);
        if (HAS_PENDING_EXCEPTION) {
          log_warning(aot, load)("Exception occurred in getting protection domain");
          CLEAR_PENDING_EXCEPTION;
          continue;
        }
      }
      SystemDictionary::load_class_from_preimage(loader, ik, pkg_entry, pd, CHECK);
      precond(SystemDictionary::find_instance_klass(THREAD, ik->name(), loader) == ik);
    } else {
      assert(loaded_ik == ik, "must be");
    }
  }
}

// Initiate loading of the <classes> in the <initiating_loader>. The <classes> should have already been loaded
// by a parent loader of the <initiating_loader>. This is necessary for handling pre-resolved CP entries.
//
// For example, we initiate the loading of java/lang/String in the AppClassLoader. This will allow
// any App classes to have a pre-resolved ConstantPool entry that references java/lang/String.
//
// TODO: we can limit the number of initiated classes to only those that are actually referenced by
// AOT-linked classes loaded by <initiating_loader>.
void FinalImageRecipes::initiate_loading(JavaThread* current, const char* category_name,
                                         Handle initiating_loader, Array<InstanceKlassRecipe>* recipes) {
  if (recipes == nullptr) {
    return;
  }

  ClassLoaderData* loader_data = ClassLoaderData::class_loader_data(initiating_loader());
  MonitorLocker mu1(SystemDictionary_lock);

  for (int i = 0; i < recipes->length(); i++) {
    InstanceKlass* ik = recipes->adr_at(i)->instance_klass();
    assert(ik->is_loaded(), "must have already been loaded by a parent loader");
    assert(ik->class_loader() != initiating_loader(), "must be a parent loader");
    assert(ik->class_loader() == nullptr ||
           ik->class_loader() == SystemDictionary::java_platform_loader() ||
           ik->class_loader() == SystemDictionary::java_system_loader(), "must be");
    if (ik->is_public() && !ik->is_hidden()) {
      if (log_is_enabled(Info, aot, load)) {
        ResourceMark rm(current);
        const char* defining_loader = (ik->class_loader() == nullptr ? "boot" : ik->class_loader() == SystemDictionary::java_platform_loader() ? "plat" : "app");
        log_info(aot, load)("%-5s %s (initiated, defined by %s)", category_name, ik->external_name(),
                            defining_loader);
      }
      SystemDictionary::add_to_initiating_loader(current, ik, loader_data);
    }
  }
}

void FinalImageRecipes::initiate_loading(JavaThread* current, Handle initiating_loader) {
  MonitorLocker mu(SystemDictionary_lock);
  SystemDictionary::mark_as_initiating_loader_of_parent_classes(current, initiating_loader);
}


void FinalImageRecipes::load_builtin_loader_classes(TRAPS) {
  Handle h_platform_loader(THREAD, SystemDictionary::java_platform_loader());
  Handle h_system_loader(THREAD, SystemDictionary::java_system_loader());

  load_classes_in_table(_class_recipes->boot1(), "boot1", Handle(), AOTClassLocationConfig::runtime()->class_locations(), CHECK);
  load_classes_in_table(_class_recipes->boot2(), "boot2", Handle(), AOTClassLocationConfig::runtime()->class_locations(), CHECK);

  initiate_loading(THREAD, "plat", h_platform_loader, _class_recipes->boot1());
  initiate_loading(THREAD, "plat", h_platform_loader, _class_recipes->boot2());
  load_classes_in_table(_class_recipes->platform(), "plat", h_platform_loader, AOTClassLocationConfig::runtime()->class_locations(), CHECK);

  initiate_loading(THREAD, "app", h_system_loader, _class_recipes->boot1());
  initiate_loading(THREAD, "app", h_system_loader, _class_recipes->boot2());
  initiate_loading(THREAD, "app", h_system_loader, _class_recipes->platform());
  load_classes_in_table(_class_recipes->app(), "app", h_system_loader, AOTClassLocationConfig::runtime()->class_locations(), CHECK);
}

void FinalImageRecipes::load_aot_safe_custom_loader_classes(TRAPS) {
  URLClassLoaderSupport::initialize(CHECK);
  _class_loader_recipes->iterate_all([&](ClassLoaderRecipe** cl_recipe_ptr) {
    ClassLoaderRecipe* cl_recipe = *cl_recipe_ptr;
    ResourceMark rm;
    char* loader_id_str = cl_recipe->aot_id()->as_C_string();
    Symbol* parent_id = cl_recipe->parent_aot_id();
    assert(ClassLoaderAotIdTable::contains(parent_id), "parent id is not yet registered");

    if (!cl_recipe->verify_classpath()) {
      // TODO: Does it make sense to just skip loading the classes for this classloader, instead of failing to create the cache?
      aot_log_warning(aot)("ClassLoaderRecipe associated with loader id %s will not be processed", loader_id_str);
      AOTMetaspace::unrecoverable_writing_error("Unable to create AOTCache");
      return;
    }

    Handle urlclassloader = URLClassLoaderSupport::create_urlclassloader(parent_id, cl_recipe->cp_locations(), CHECK);
    assert(urlclassloader.not_null(), "must be");
    ClassLoaderData* cld = SystemDictionary::register_loader(urlclassloader);

    initiate_loading(THREAD, urlclassloader);

    load_classes_in_table(cl_recipe->class_recipes(), loader_id_str, urlclassloader, cl_recipe->cp_locations(), CHECK);
  });
}

void FinalImageRecipes::load_aot_unsafe_custom_loader_classes(TRAPS) {
  Array<InstanceKlassRecipe>* recipes = _class_recipes->aot_unsafe_custom_loader_classes();
  for (int i = 0; i < recipes->length(); i++) {
    InstanceKlass* ik = recipes->adr_at(i)->instance_klass();
    SystemDictionaryShared::init_dumptime_info(ik);
    SystemDictionaryShared::add_unregistered_class(THREAD, ik);
    SystemDictionaryShared::copy_unregistered_class_size_and_crc32(ik);
  }
}

void exit_on_exception(JavaThread* current) {
  assert(current->has_pending_exception(), "precondition");
  ResourceMark rm(current);
  if (current->pending_exception()->is_a(vmClasses::OutOfMemoryError_klass())) {
    log_error(aot)("Out of memory. Please run with a larger Java heap, current MaxHeapSize = "
                   "%zuM", MaxHeapSize/M);
  } else {
    oop message = java_lang_Throwable::message(current->pending_exception());
    log_error(aot)("%s: %s", current->pending_exception()->klass()->external_name(),
                   message == nullptr ? "(no message)" : java_lang_String::as_utf8_string(message));
  }
  vm_exit_during_initialization("Unexpected exception when loading aot-linked classes.");
}

// Some cached heap objects may hold references to methods in aot-linked
// classes (via MemberName). We need to make sure all classes are
// linked before executing any bytecode.
void FinalImageRecipes::link_classes(JavaThread* current) {
  link_classes_impl(current);
  if (current->has_pending_exception()) {
    exit_on_exception(current);
  }
}

void FinalImageRecipes::link_classes_impl(TRAPS) {
  link_classes_in_table(_class_recipes->boot1(), CHECK);
  link_classes_in_table(_class_recipes->boot2(), CHECK);
  link_classes_in_table(_class_recipes->platform(), CHECK);
  link_classes_in_table(_class_recipes->app(), CHECK);
  _class_loader_recipes->iterate_all([&](ClassLoaderRecipe** cl_recipe) {
    link_classes_in_table((*cl_recipe)->class_recipes(), CHECK);
  });
}

void FinalImageRecipes::link_classes_in_table(Array<InstanceKlassRecipe>* recipes, TRAPS) {
  if (recipes != nullptr) {
    for (int i = 0; i < recipes->length(); i++) {
      // NOTE: CDSConfig::is_preserving_verification_constraints() is required
      // when storing ik in the AOT cache. This means we don't have to verify
      // ik at all.
      //
      // Without is_preserving_verification_constraints(), ik->link_class() may cause
      // class loading, which may result in invocation of ClassLoader::loadClass() calls,
      // which CANNOT happen because we are not ready to execute any Java byecodes yet
      // at this point.
      InstanceKlass* ik = recipes->adr_at(i)->instance_klass();
      ik->link_class(CHECK);
      if (ik->has_aot_safe_initializer() && (recipes->adr_at(i)->flags() & WAS_INITED) != 0) {
        assert(ik->class_loader() == nullptr, "supported only for boot classes for now");
        ResourceMark rm(THREAD);
        log_info(aot, init)("Initializing %s", ik->external_name());
        ik->initialize(CHECK);
      }
    }
  }
}

void FinalImageRecipes::load_and_link_all_classes(TRAPS) {
  /* Built-in loader classes come first */
  load_builtin_loader_classes(CHECK);
  /* Now load custom loader classes */
  load_aot_safe_custom_loader_classes(CHECK);
  load_aot_unsafe_custom_loader_classes(CHECK);
  link_classes(THREAD);
}

void FinalImageRecipes::apply_cp_recipes_for_class(JavaThread* current, InstanceKlassRecipe* ikr) {
  InstanceKlass* ik = ikr->instance_klass();
  Array<int>* cp_indices = ikr->cp_recipe();
  int flags = ikr->flags();
  if (cp_indices != nullptr) {
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

void FinalImageRecipes::apply_recipes_for_constantpool(JavaThread* current) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");

  _class_recipes->iterate_builtin_class_recipes([&](InstanceKlassRecipe* ikr) {
    apply_cp_recipes_for_class(current, ikr);
  });

  _class_loader_recipes->iterate_all([&](ClassLoaderRecipe** cl_recipe) {
    (*cl_recipe)->class_recipes()->iterate_all([&](InstanceKlassRecipe* ikr) {
      apply_cp_recipes_for_class(current, ikr);
    });
  });
}

void FinalImageRecipes::record_recipes() {
  assert(CDSConfig::is_dumping_preimage_static_archive(), "must be");
  _final_image_recipes = new FinalImageRecipes();
  _final_image_recipes->record_class_recipes();
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
  load_and_link_all_classes(CHECK);
  apply_recipes_for_constantpool(THREAD);
}

void FinalImageRecipes::serialize(SerializeClosure* soc) {
  soc->do_ptr((void**)&_final_image_recipes);
}
