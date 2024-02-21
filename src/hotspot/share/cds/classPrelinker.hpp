/*
 * Copyright (c) 2022, 2024, Oracle and/or its affiliates. All rights reserved.
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
class SerializeClosure;

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

  static PreloadedKlasses _static_preloaded_klasses;
  static PreloadedKlasses _dynamic_preloaded_klasses;
  static Array<InstanceKlass*>* _unregistered_klasses_from_preimage;

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
  static Klass* find_loaded_class(Thread* current, oop class_loader, Symbol* name);
  static Klass* find_loaded_class(Thread* current, ConstantPool* cp, int class_cp_index);
  static void add_preloaded_klasses(Array<InstanceKlass*>* klasses);
  static void add_unrecorded_initiated_klasses(ClassesTable* table, Array<InstanceKlass*>* klasses);
  static void add_extra_initiated_klasses(PreloadedKlasses* table);
  static void add_initiated_klasses_for_loader(ClassLoaderData* loader_data, const char* loader_name, ClassesTable* table);
  static void add_initiated_klass(InstanceKlass* ik, InstanceKlass* target);
  static void add_initiated_klass(ClassesTable* initiated_classes, const char* loader_name, InstanceKlass* target);
  static Array<InstanceKlass*>* record_preloaded_klasses(int loader_type);
  static Array<InstanceKlass*>* record_initiated_klasses(ClassesTable* table);
  static void runtime_preload(PreloadedKlasses* table, Handle loader, TRAPS);
  static void preload_archived_hidden_class(Handle class_loader, InstanceKlass* ik,
                                            const char* loader_name, TRAPS);
  static void jvmti_agent_error(InstanceKlass* expected, InstanceKlass* actual, const char* type);

  // fmi = FieldRef/MethodRef/InterfaceMethodRef
  static Klass* get_fmi_ref_resolved_archivable_klass(ConstantPool* cp, int cp_index);
  static void maybe_resolve_fmi_ref(InstanceKlass* ik, Method* m, Bytecodes::Code bc, int raw_index,
                                    GrowableArray<bool>* resolve_fmi_list, TRAPS);
  static bool is_in_javabase(InstanceKlass* ik);
  class RecordResolveIndysCLDClosure;
  class RecordInitiatedClassesClosure;

  // helper
  static Klass* resolve_boot_klass_or_fail(const char* class_name, TRAPS);

  // java/lang/reflect/Proxy caching
  static void init_dynamic_proxy_cache(TRAPS);

  // Preinitialize classes during dump time
  static bool has_non_default_static_fields(InstanceKlass* ik);
  static bool is_forced_preinit_class(InstanceKlass* ik);

public:
  static void initialize();
  static void dispose();

  // Preinitialize classes during dump time
  static bool check_can_be_preinited(InstanceKlass* ik);
  static void maybe_preinit_class(InstanceKlass* ik, TRAPS);

  static void preresolve_class_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list);
  static void preresolve_field_and_method_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list);
  static void preresolve_indy_cp_entries(JavaThread* current, InstanceKlass* ik, GrowableArray<bool>* preresolve_list);
  static void preresolve_invoker_class(JavaThread* current, InstanceKlass* ik);
  static void apply_final_image_eager_linkage(TRAPS);

  static bool is_indy_archivable(ConstantPool* cp, int cp_index);

  // java/lang/Class$ReflectionData caching
  static void record_reflection_data_flags_for_preimage(InstanceKlass* ik, TRAPS);
  static int class_reflection_data_flags(InstanceKlass* ik, TRAPS);
  static void generate_reflection_data(JavaThread* current, InstanceKlass* ik, int rd_flags);

  // java/lang/reflect/Proxy caching
  static void trace_dynamic_proxy_class(oop loader, const char* proxy_name, objArrayOop interfaces, int access_flags);
  static void define_dynamic_proxy_class(Handle loader, Handle proxy_name, Handle interfaces, int access_flags, TRAPS);

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

  static bool can_archive_preinitialized_mirror(InstanceKlass* src_ik);

  static void record_preloaded_klasses(bool is_static_archive);
  static void record_initiated_klasses(bool is_static_archive);
  static void record_unregistered_klasses();
  static void record_final_image_eager_linkage();
  static void serialize(SerializeClosure* soc, bool is_static_archive);

  static void runtime_preload(JavaThread* current, Handle loader) NOT_CDS_RETURN;
  static void init_javabase_preloaded_classes(TRAPS) NOT_CDS_RETURN;
  static void replay_training_at_init_for_javabase_preloaded_classes(TRAPS) NOT_CDS_RETURN;
  static bool class_preloading_finished();

  static int  num_platform_initiated_classes();
  static int  num_app_initiated_classes();
  static void print_counters() NOT_CDS_RETURN;
};

#endif // SHARE_CDS_CLASSPRELINKER_HPP
