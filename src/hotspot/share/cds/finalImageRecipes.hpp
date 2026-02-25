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

class InstanceKlass;
class Klass;

template <typename T> class GrowableArray;
template <typename T> class Array;

class InstanceKlassRecipe {
private:
  InstanceKlass* _ik;
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

class FinalImageRecipeTable {
private:
  Array<InstanceKlassRecipe>* _boot1; // boot classes in java.base module
  Array<InstanceKlassRecipe>* _boot2; // boot classes in all other (named and unnamed) modules,
                                 // including classes from -Xbootclasspath/a
  Array<InstanceKlassRecipe>* _platform;
  Array<InstanceKlassRecipe>* _app;

  Array<InstanceKlassRecipe>* _aot_unsafe_custom_loader_classes;

  template<typename Function>
  void iterate_array(Function fn, Array<InstanceKlassRecipe>* array) {
    if (array != nullptr) {
      for (int i = 0; i < array->length(); i++) {
        fn(array->adr_at(i));
      }
    }
  }

public:
  FinalImageRecipeTable() :
    _boot1(nullptr), _boot2(nullptr),
    _platform(nullptr), _app(nullptr),
    _aot_unsafe_custom_loader_classes(nullptr) {}

  Array<InstanceKlassRecipe>* boot1()    const { return _boot1;    }
  Array<InstanceKlassRecipe>* boot2()    const { return _boot2;    }
  Array<InstanceKlassRecipe>* platform() const { return _platform; }
  Array<InstanceKlassRecipe>* app()      const { return _app;      }
  Array<InstanceKlassRecipe>* aot_unsafe_custom_loader_classes() const { return _aot_unsafe_custom_loader_classes; }

  void set_boot1   (Array<InstanceKlassRecipe>* value) { _boot1    = value; }
  void set_boot2   (Array<InstanceKlassRecipe>* value) { _boot2    = value; }
  void set_platform(Array<InstanceKlassRecipe>* value) { _platform = value; }
  void set_app     (Array<InstanceKlassRecipe>* value) { _app      = value; }
  void set_aot_unsafe_custom_loader_classes(Array<InstanceKlassRecipe>* value) { _aot_unsafe_custom_loader_classes = value; }

  template<typename Function>
  void iterate_builtin_classes(Function fn) {
    iterate_array(fn, _boot1);
    iterate_array(fn, _boot2);
    iterate_array(fn, _platform);
    iterate_array(fn, _app);
  }
  template<typename Function>
  void iterate_all_classes(Function fn) {
    iterate_builtin_classes(fn);
    iterate_array(fn, _aot_unsafe_custom_loader_classes);
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
  //Array<Klass*>* _all_klasses;
  FinalImageRecipeTable* _class_table;

  // For each klass k _all_klasses->at(i): _cp_recipes->at(i) lists all the {klass,field,method,indy}
  // cp indices that were resolved for k during the training run; _flags->at(i) has extra info about k.
  Array<Array<int>*>* _cp_recipes;
  Array<int>* _flags;

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

  FinalImageRecipes() : _class_table(nullptr), _cp_recipes(nullptr), _flags(nullptr),
                        _reflect_klasses(nullptr), _reflect_flags(nullptr),
                        _dynamic_proxy_classes(nullptr) {}

  void* operator new(size_t size) throw();

  // Called when dumping preimage
  void record_all_classes();
  void record_aot_safe_custom_loader_classes();
  Array<int>* record_recipe_for_constantpool(InstanceKlass* ik, int& flags);
  //void record_recipes_for_constantpool();
  void record_recipes_for_reflection_data();
  void record_recipes_for_dynamic_proxies();

  // Called when dumping final image
  void load_builtin_loader_classes(TRAPS);
  void load_aot_safe_custom_loader_classes(TRAPS);
  void load_aot_unsafe_custom_loader_classes(TRAPS);
  void load_classes_in_table(Array<InstanceKlassRecipe>* classes, const char* category_name, Handle loader, TRAPS);
  void initiate_loading(JavaThread* current, const char* category_name, Handle initiating_loader, Array<InstanceKlassRecipe>* classes);
  void link_classes(JavaThread* current);
  void link_classes_impl(TRAPS);
  void link_classes_in_table(Array<InstanceKlassRecipe>* classes, TRAPS);

  void apply_recipes_impl(TRAPS);
  void load_and_link_all_classes(TRAPS);
  void apply_cp_recipes_for_class(JavaThread* current, InstanceKlassRecipe* ikr);
  void apply_recipes_for_constantpool(JavaThread* current);
  void apply_recipes_for_reflection_data(JavaThread* current);
  void apply_recipes_for_dynamic_proxies(TRAPS);

  Array<InstanceKlassRecipe>* write_classes(oop class_loader, bool is_javabase, bool is_builtin_loader);

  static void exit_on_exception(JavaThread* current);

public:
  static void serialize(SerializeClosure* soc);

  // Called when dumping preimage
  static void add_dynamic_proxy_class(oop loader, const char* proxy_name, objArrayOop interfaces, int access_flags);
  static void add_reflection_data_flags(InstanceKlass* ik, TRAPS);
  static void record_recipes();

  // Called when dumping final image
  static void apply_recipes(TRAPS);
};

#endif // SHARE_CDS_FINALIMAGERECIPES_HPP
