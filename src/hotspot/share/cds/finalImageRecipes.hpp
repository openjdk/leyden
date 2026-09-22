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

#ifndef SHARE_CDS_FINALIMAGERECIPES_HPP
#define SHARE_CDS_FINALIMAGERECIPES_HPP

#include "oops/oopsHierarchy.hpp"
#include "utilities/exceptions.hpp"

class AOTClassLocation;
class InstanceKlass;
class Klass;

template <typename T> class GrowableArray;
template <typename T> class Array;

class InstanceKlassRecipe {
private:
  InstanceKlass* _ik;
  // For each klass k _cp_recipe lists all the {klass,field,method,indy}
  // cp indices that were resolved for k during the training run; _flags has extra info about k.
  Array<int>* _cp_recipe;
  int _flags;
public:
  InstanceKlassRecipe() : _ik(nullptr), _cp_recipe(nullptr), _flags(0) {} // required by GrowableArray
  InstanceKlassRecipe(InstanceKlass* ik, Array<int>* cp_recipe, int flags) :
    _ik(ik), _cp_recipe(cp_recipe), _flags(flags) {}

  InstanceKlass* instance_klass() const { return _ik; }
  Array<int>* cp_recipe() const { return _cp_recipe; }
  int flags() const { return _flags; }

  void mark_pointers() {
    ArchivePtrMarker::mark_pointer(&_ik);
    ArchivePtrMarker::mark_pointer(&_cp_recipe);
  }
};

using ClassRecipeList = GrowableArrayCHeap<InstanceKlassRecipe, mtClassShared>;

class ClassLoaderRecipe {
private:
  Symbol* _aot_id;
  // Parent's id is used to identify the parent of the scratch loader during assembly phase
  // See URLClassLoaderSupport::create_urlclassloader()
  Symbol* _parent_aot_id;
  Array<AOTClassLocation*>* _cp_locations;
  Array<InstanceKlassRecipe>* _class_recipes;

  address* aot_id_addr() const { return (address*)&_aot_id; }
  address* parent_aot_id_addr() const { return (address*)&_parent_aot_id; }
  address* cp_locations_addr() const { return (address*)&_cp_locations; }
  address* class_recipes_addr() const { return (address*)&_class_recipes; }

  // private constructor as this class instances are allocated by reserving memory in AOTCache
  ClassLoaderRecipe() {}

  void init(Symbol* aot_id, Symbol* parent_id, Array<InstanceKlassRecipe>* class_recipes, Array<AOTClassLocation*>* cp_locations) {
    _aot_id = aot_id;
    _parent_aot_id = parent_id;
    _cp_locations = cp_locations;
    _class_recipes = class_recipes;
    mark_pointers();
  }

  void mark_pointers();

public:
  static ClassLoaderRecipe* allocate(Symbol* aot_id, Symbol* parent_id, Array<InstanceKlassRecipe>* class_recipes, Array<AOTClassLocation*>* cp_locations);

  Symbol* aot_id() const { return _aot_id; }
  Symbol* parent_aot_id() const { return _parent_aot_id; }
  Array<AOTClassLocation*>* cp_locations() const { return _cp_locations; }
  Array<InstanceKlassRecipe>* class_recipes() const { return _class_recipes; }

  bool verify_classpath();
};

class InstanceKlassRecipes {
private:
  Array<InstanceKlassRecipe>* _boot1; // boot classes in java.base module
  Array<InstanceKlassRecipe>* _boot2; // boot classes in all other (named and unnamed) modules,
                                      // including classes from -Xbootclasspath/a
  Array<InstanceKlassRecipe>* _platform;
  Array<InstanceKlassRecipe>* _app;

  Array<InstanceKlassRecipe>* _aot_unsafe_custom_loader_classes;

  static void log_recorded_classes(Array<InstanceKlassRecipe>* list);
public:
  InstanceKlassRecipes() :
    _boot1(nullptr), _boot2(nullptr),
    _platform(nullptr), _app(nullptr),
    _aot_unsafe_custom_loader_classes(nullptr) {}

  Array<InstanceKlassRecipe>* boot1()    const { return _boot1;    }
  Array<InstanceKlassRecipe>* boot2()    const { return _boot2;    }
  Array<InstanceKlassRecipe>* platform() const { return _platform; }
  Array<InstanceKlassRecipe>* app()      const { return _app;      }
  Array<InstanceKlassRecipe>* aot_unsafe_custom_loader_classes() const { return _aot_unsafe_custom_loader_classes; }

  void set_boot1   (Array<InstanceKlassRecipe>* value) { log_recorded_classes(value); _boot1    = value; }
  void set_boot2   (Array<InstanceKlassRecipe>* value) { log_recorded_classes(value); _boot2    = value; }
  void set_platform(Array<InstanceKlassRecipe>* value) { log_recorded_classes(value); _platform = value; }
  void set_app     (Array<InstanceKlassRecipe>* value) { log_recorded_classes(value); _app      = value; }
  void set_aot_unsafe_custom_loader_classes(Array<InstanceKlassRecipe>* value) { log_recorded_classes(value); _aot_unsafe_custom_loader_classes = value; }

  template<typename Function>
  void iterate_builtin_class_recipes(Function fn) {
    _boot1->iterate_all(fn);
    _boot2->iterate_all(fn);
    _platform->iterate_all(fn);
    _app->iterate_all(fn);
  }
  template<typename Function>
  void iterate_classes_recipes(Function fn) {
    iterate_builtin_class_recipes(fn);
    _aot_unsafe_custom_loader_classes->iterate_all(fn);
  }
  void mark_pointers();
};

// This class is used for transferring information from the AOTConfiguration file (aka the "preimage")
// to the JVM that creates the AOTCache (aka the "final image").
//   - The recipes are recorded when CDSConfig::is_dumping_preimage_static_archive() is true.
//   - The recipes are applied when CDSConfig::is_dumping_final_static_archive() is true.
// The following information are recorded:
//   - The list of all classes that are stored in the AOTConfiguration file.
//   - The list of all classes that require AOT resolution of invokedynamic call sites.
class FinalImageRecipes {
  static constexpr int CP_RESOLVE_CLASS            = 0x1 << 0; // CP has preresolved class entries
  static constexpr int CP_RESOLVE_FIELD_AND_METHOD = 0x1 << 1; // CP has preresolved field/method entries
  static constexpr int CP_RESOLVE_INDY             = 0x1 << 2; // CP has preresolved indy entries
  static constexpr int WAS_INITED                  = 0x1 << 3; // Class was initialized during training run

  // A list of all the archived classes from the preimage. We want to transfer all of these
  // into the final image.
  InstanceKlassRecipes* _class_recipes;
  Array<ClassLoaderRecipe*>* _class_loader_recipes;

  void* operator new(size_t size) throw();

  // Called when dumping preimage
  void record_class_recipes();
  void record_aot_safe_loader_classes();
  Array<int>* record_recipe_for_constantpool(InstanceKlass* ik, int& flags);
  void write_recipes(InstanceKlassRecipes* table);

  // Called when dumping final image
  void load_builtin_loader_classes(TRAPS);
  void load_aot_safe_custom_loader_classes(TRAPS);
  void load_aot_unsafe_custom_loader_classes(TRAPS);
  void load_classes_in_table(Array<InstanceKlassRecipe>* classes, const char* category_name, Handle loader, Array<AOTClassLocation*>* class_locations, TRAPS);
  void initiate_loading(JavaThread* current, const char* category_name, Handle initiating_loader, Array<InstanceKlassRecipe>* classes);
  void initiate_loading(JavaThread* current, Handle initiating_loader);
  void link_classes(JavaThread* current);
  void link_classes_impl(TRAPS);
  void link_classes_in_table(Array<InstanceKlassRecipe>* classes, TRAPS);

  void apply_recipes_impl(TRAPS);
  void load_and_link_all_classes(TRAPS);
  void apply_cp_recipes_for_class(JavaThread* current, InstanceKlassRecipe* ikr);
  void apply_recipes_for_constantpool(JavaThread* current);

  static Array<AOTClassLocation*>* archive_classpath(ClassLoaderData* cld);
public:
  static void serialize(SerializeClosure* soc);
  // Called when dumping preimage
  static void record_recipes();
  // Called when dumping final image
  static void apply_recipes(TRAPS);
};

#endif // SHARE_CDS_FINALIMAGERECIPES_HPP
