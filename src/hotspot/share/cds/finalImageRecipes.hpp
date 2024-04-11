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

#ifndef SHARE_CDS_FINALIMAGERECIPES_HPP
#define SHARE_CDS_FINALIMAGERECIPES_HPP

#include "oops/oopsHierarchy.hpp"
#include "utilities/exceptions.hpp"

class InstanceKlass;
class Klass;

template <typename T> class GrowableArray;
template <typename T> class Array;

// This class is used only by the "one step training workflow". It records the
// "recipes" for creating the final CDS image.
// - The recipes are recorded when CDSConfig::is_dumping_preimage_static_archive() is true;
// - The recipes are applied when CDSConfig::is_dumping_final_static_archive() is true;
class FinalImageRecipes {
  // The classes who have resolved at least one indy CP entry during the training run.
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

  FinalImageRecipes() : _indy_klasses(nullptr), _indy_cp_indices(nullptr),
                        _reflect_klasses(nullptr), _reflect_flags(nullptr),
                        _dynamic_proxy_classes(nullptr) {}

  void* operator new(size_t size) throw();

  // Called when dumping preimage
  void record_recipes_impl();

  // Called when dumping final image
  void apply_recipes_for_dynamic_proxies(TRAPS);
  void apply_recipes_for_invokedynamic(TRAPS);
  void apply_recipes_for_reflection_data(JavaThread* current);

public:
  static void serialize(SerializeClosure* soc, bool is_static_archive);

  // Called when dumping preimage
  static void add_dynamic_proxy_class(oop loader, const char* proxy_name, objArrayOop interfaces, int access_flags);
  static void add_reflection_data_flags(InstanceKlass* ik, TRAPS);
  static void record_recipes();

  // Called when dumping final image
  static void apply_recipes(TRAPS);
};

#endif // SHARE_CDS_FINALIMAGERECIPES_HPP
