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

#ifndef SHARE_CDS_CLASSPRELOADER_HPP
#define SHARE_CDS_CLASSPRELOADER_HPP

#include "interpreter/bytecodes.hpp"
#include "oops/oopsHierarchy.hpp"
#include "memory/allStatic.hpp"
#include "memory/allocation.hpp"
#include "runtime/handles.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/macros.hpp"
#include "utilities/resourceHash.hpp"

class InstanceKlass;
class Klass;
class SerializeClosure;
template <typename T> class Array;

class ClassPreloader :  AllStatic {
  class PreloadedKlassRecorder;
  using ClassesTable = ResourceHashtable<InstanceKlass*, bool, 15889, AnyObj::C_HEAP, mtClassShared>;

  // Classes loaded inside vmClasses::resolve_all()
  static ClassesTable* _vm_classes;

  // Classes that will be automatically loaded into system dictionary at
  // VM start-up (this is a superset of _vm_classes)
  static ClassesTable* _preloaded_classes;
  static ClassesTable* _platform_initiated_classes; // classes initiated but not loaded by platform loader
  static ClassesTable* _app_initiated_classes;      // classes initiated but not loaded by app loader

  static bool _record_javabase_only;
  static bool _preload_javabase_only;
  struct PreloadedKlasses {
    Array<InstanceKlass*>* _boot;  // only java.base classes
    Array<InstanceKlass*>* _boot2; // boot classes in other modules
    Array<InstanceKlass*>* _platform;
    Array<InstanceKlass*>* _platform_initiated;
    Array<InstanceKlass*>* _app;
    Array<InstanceKlass*>* _app_initiated;
    PreloadedKlasses() : _boot(nullptr), _boot2(nullptr), _platform(nullptr), _app(nullptr) {}
  };

  static PreloadedKlasses _static_preloaded_classes;
  static PreloadedKlasses _dynamic_preloaded_classes;
  static Array<InstanceKlass*>* _unregistered_classes_from_preimage;

  static void add_one_vm_class(InstanceKlass* ik);
  static void add_preloaded_classes(Array<InstanceKlass*>* klasses);
  static void add_unrecorded_initiated_classes(ClassesTable* table, Array<InstanceKlass*>* klasses);
  static void add_extra_initiated_classes(PreloadedKlasses* table);
  static void add_initiated_classes_for_loader(ClassLoaderData* loader_data, const char* loader_name, ClassesTable* table);
  static void add_initiated_class(ClassesTable* initiated_classes, const char* loader_name, InstanceKlass* target);
  static Array<InstanceKlass*>* record_preloaded_classes(int loader_type);
  static Array<InstanceKlass*>* record_initiated_classes(ClassesTable* table);
  static void runtime_preload(PreloadedKlasses* table, Handle loader, TRAPS);
  static void runtime_preload_class_quick(InstanceKlass* ik, ClassLoaderData* loader_data, Handle domain, TRAPS);
  static void preload_archived_hidden_class(Handle class_loader, InstanceKlass* ik,
                                            const char* loader_name, TRAPS);
  static void jvmti_agent_error(InstanceKlass* expected, InstanceKlass* actual, const char* type);

  static bool is_in_javabase(InstanceKlass* ik);
  class RecordInitiatedClassesClosure;

  static void replay_training_at_init(Array<InstanceKlass*>* preloaded_classes, TRAPS) NOT_CDS_RETURN;

public:
  static void initialize();
  static void dispose();

  static void record_preloaded_classes(bool is_static_archive);
  static void record_initiated_classes(bool is_static_archive);
  static void record_unregistered_classes();
  static void serialize(SerializeClosure* soc, bool is_static_archive);

  // Is this class resolved as part of vmClasses::resolve_all()?
  static bool is_vm_class(InstanceKlass* ik);

  // When CDS is enabled, is ik guatanteed to be loaded at deployment time (and
  // cannot be replaced by JVMTI)?
  // This is a necessary (not but sufficient) condition for keeping a direct pointer
  // to ik in precomputed data (such as ConstantPool entries in archived classes,
  // or in AOT-compiled code).
  static bool is_preloaded_class(InstanceKlass* ik);

  static void add_preloaded_class(InstanceKlass* ik);
  static void add_initiated_class(InstanceKlass* ik, InstanceKlass* target);

  static void runtime_preload(JavaThread* current, Handle loader) NOT_CDS_RETURN;
  static void init_javabase_preloaded_classes(TRAPS) NOT_CDS_RETURN;
  static void replay_training_at_init_for_preloaded_classes(TRAPS) NOT_CDS_RETURN;
  static bool class_preloading_finished();

  static int  num_platform_initiated_classes();
  static int  num_app_initiated_classes();
  static void print_counters() NOT_CDS_RETURN;
};

#endif // SHARE_CDS_CLASSPRELOADER_HPP
