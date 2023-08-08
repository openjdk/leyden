/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_CDS_CLASSPRELINKER_HPP
#define SHARE_CDS_CLASSPRELINKER_HPP

#include "interpreter/bytecodes.hpp"
#include "oops/oopsHierarchy.hpp"
#include "memory/allStatic.hpp"
#include "memory/allocation.hpp"
#include "runtime/handles.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/macros.hpp"
#include "utilities/resourceHash.hpp"

class ConstantPool;
class constantPoolHandle;
class InstanceKlass;
class Klass;

template <typename T> class Array;
template <typename T> class GrowableArray;
// ClassPrelinker is used to perform ahead-of-time linking of ConstantPool entries
// for archived InstanceKlasses.
//
// At run time, Java classes are loaded dynamically and may be replaced with JVMTI.
// Therefore, we take care to prelink only the ConstantPool entries that are
// guatanteed to resolve to the same results at both dump time and run time.
//
// For example, a JVM_CONSTANT_Class reference to a supertype can be safely resolved
// at dump time, because at run time we will load a class from the CDS archive only
// if all of its supertypes are loaded from the CDS archive.
class ClassPrelinker :  AllStatic {
  class PreloadedKlassRecorder;
  using ClassesTable = ResourceHashtable<InstanceKlass*, bool, 15889, AnyObj::C_HEAP, mtClassShared> ;
  static ClassesTable* _processed_classes;
  // Classes loaded inside vmClasses::resolve_all()
  static ClassesTable* _vm_classes;
  // Classes that will be automatically loaded into system dictionary at
  // VM start-up (this is a superset of _vm_classes)
  static ClassesTable* _preloaded_classes;
  static ClassesTable* _platform_initiated_classes; // classes initiated but not loaded by platform loader
  static ClassesTable* _app_initiated_classes;      // classes initiated but not loaded by app loader
  static int _num_vm_klasses;
  static bool _record_java_base_only;
  static bool _preload_java_base_only;
  struct PreloadedKlasses {
    Array<InstanceKlass*>* _boot;  // only java.base classes
    Array<InstanceKlass*>* _boot2; // boot classes in other modules
    Array<InstanceKlass*>* _platform;
    Array<InstanceKlass*>* _platform_initiated;
    Array<InstanceKlass*>* _app;
    Array<InstanceKlass*>* _app_initiated;
    PreloadedKlasses() : _boot(nullptr), _boot2(nullptr), _platform(nullptr), _app(nullptr) {}
  };

  static PreloadedKlasses _static_preloaded_klasses;
  static PreloadedKlasses _dynamic_preloaded_klasses;

  static void add_one_vm_class(InstanceKlass* ik);

#ifdef ASSERT
  static bool is_in_archivebuilder_buffer(address p);
#endif

  template <typename T>
  static bool is_in_archivebuilder_buffer(T p) {
    return is_in_archivebuilder_buffer((address)(p));
  }
  static void resolve_string(constantPoolHandle cp, int cp_index, TRAPS) NOT_CDS_JAVA_HEAP_RETURN;
  static Klass* maybe_resolve_class(constantPoolHandle cp, int cp_index, TRAPS);
  static bool can_archive_resolved_klass(InstanceKlass* cp_holder, Klass* resolved_klass);
  static Klass* find_loaded_class(JavaThread* current, oop class_loader, Symbol* name);
  static Klass* find_loaded_class(JavaThread* current, ConstantPool* cp, int class_cp_index);
  static void add_preloaded_klasses(Array<InstanceKlass*>* klasses);
  static Array<InstanceKlass*>* archive_klass_array(GrowableArray<InstanceKlass*>* tmp_array);
  static Array<InstanceKlass*>* record_preloaded_klasses(int loader_type);
  static Array<InstanceKlass*>* record_initiated_klasses(ClassesTable* table);
  static void runtime_preload(PreloadedKlasses* table, Handle loader, TRAPS);
  static void jvmti_agent_error(InstanceKlass* expected, InstanceKlass* actual, const char* type);
  static Klass* get_fmi_ref_resolved_archivable_klass(ConstantPool* cp, int cp_index);

  static void maybe_resolve_fmi_ref(InstanceKlass* ik, Method* m, Bytecodes::Code bc, int raw_index,
                                    GrowableArray<bool>* resolve_fmi_list, TRAPS);
public:
  static void initialize();
  static void dispose();

  static void preresolve_class_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list);
  static void preresolve_field_and_method_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list);
  static void preresolve_indy_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list);
  static void preresolve_invoker_class(JavaThread* current, InstanceKlass* ik);

  static bool should_preresolve_invokedynamic(ConstantPool* cp, int cp_index);

  // Is this class resolved as part of vmClasses::resolve_all()? If so, these
  // classes are guatanteed to be loaded at runtime (and cannot be replaced by JVMTI)
  // when CDS is enabled. Therefore, we can safely keep a direct reference to these
  // classes.
  static bool is_vm_class(InstanceKlass* ik);
  static bool is_preloaded_class(InstanceKlass* ik);
  static int  num_vm_klasses() { return _num_vm_klasses; }
  // Resolve all constant pool entries that are safe to be stored in the
  // CDS archive.
  static void dumptime_resolve_constants(InstanceKlass* ik, TRAPS);

  // Can we resolve the klass entry at cp_index in this constant pool, and store
  // the result in the CDS archive? Returns true if cp_index is guaranteed to
  // resolve to the same InstanceKlass* at both dump time and run time.
  static bool can_archive_resolved_klass(ConstantPool* cp, int cp_index);

  // Similar to can_archive_resolved_klass() -- returns true if cp_index is
  // guaranteed to resolve to the same result both dump time and run time.
  static bool can_archive_resolved_field(ConstantPool* cp, int cp_index);
  static bool can_archive_resolved_method(ConstantPool* cp, int cp_index);

  static void record_preloaded_klasses(bool is_static_archive);
  static void record_initiated_klasses(bool is_static_archive);
  static void serialize(SerializeClosure* soc, bool is_static_archive);

  static void runtime_preload(JavaThread* current, Handle loader);
  static bool class_preloading_finished();
};

#endif // SHARE_CDS_CLASSPRELINKER_HPP
