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

#include "precompiled.hpp"
#include "cds/archiveBuilder.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/classPreinitializer.hpp"
#include "dumpTimeClassInfo.inline.hpp"
#include "cds/heapShared.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "memory/resourceArea.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "runtime/mutexLocker.hpp"

// Warning -- this is fragile!!!
//
// This is a hard-coded list of classes that are safe to preinitialize at dump time. It needs
// to be updated if the Java source code changes.
bool ClassPreinitializer::is_forced_preinit_class(InstanceKlass* ik) {
  if (!CDSConfig::is_dumping_invokedynamic()) {
    return false;
  }

  static const char* forced_preinit_classes[] = {
    "java/util/HexFormat",
    "jdk/internal/util/ClassFileDumper",
    "java/lang/reflect/ClassFileFormatVersion",
    "java/lang/Character$CharacterCache",
    "java/lang/invoke/Invokers",
    "java/lang/invoke/Invokers$Holder",
    "java/lang/invoke/MethodHandle",
    "java/lang/invoke/MethodHandleStatics",
    "java/lang/invoke/DelegatingMethodHandle",
    "java/lang/invoke/DelegatingMethodHandle$Holder",
    "java/lang/invoke/LambdaForm",
    "java/lang/invoke/LambdaForm$NamedFunction",
    "java/lang/invoke/ClassSpecializer",
    "java/lang/invoke/DirectMethodHandle",
    "java/lang/invoke/DirectMethodHandle$Holder",
    "java/lang/invoke/BoundMethodHandle$Specializer",
    "java/lang/invoke/MethodHandles$Lookup",

  //TODO: these use java.lang.ClassValue$Entry which is a subtype of WeakReference
  //"java/lang/reflect/Proxy$ProxyBuilder",
  //"java/lang/reflect/Proxy",

  // TODO -- need to clear internTable, etc
  //"java/lang/invoke/MethodType",

  // TODO -- these need to link to native code
  //"java/lang/invoke/BoundMethodHandle",
  //"java/lang/invoke/BoundMethodHandle$Holder",
  //"java/lang/invoke/MemberName",
  //"java/lang/invoke/MethodHandleNatives",
  };

  for (const char* class_name : forced_preinit_classes) {
    if (ik->name()->equals(class_name)) {
      if (log_is_enabled(Info, cds, init)) {
        ResourceMark rm;
        log_info(cds, init)("Force initialization %s", ik->external_name());
      }
      return true;
    }
  }

  return false;
}

// check_can_be_preinited() is quite costly, so we cache the results inside
// DumpTimeClassInfo::_can_be_preinited. See also ClassPreinitializer::reset_preinit_check().
bool ClassPreinitializer::check_can_be_preinited(InstanceKlass* ik) {
  ResourceMark rm;

  if (!SystemDictionaryShared::is_builtin(ik)) {
    log_info(cds, init)("cannot initialize %s (not built-in loader)", ik->external_name());
    return false;
  }

  InstanceKlass* super = ik->java_super();
  if (super != nullptr && !can_be_preinited_locked(super)) {
    log_info(cds, init)("cannot initialize %s (super %s not initable)", ik->external_name(), super->external_name());
    return false;
  }

  Array<InstanceKlass*>* interfaces = ik->local_interfaces();
  for (int i = 0; i < interfaces->length(); i++) {
    if (!can_be_preinited_locked(interfaces->at(i))) {
      log_info(cds, init)("cannot initialize %s (interface %s not initable)",
                          ik->external_name(), interfaces->at(i)->external_name());
      return false;
    }
  }

  if (HeapShared::is_lambda_form_klass(ik) || is_forced_preinit_class(ik)) {
    // We allow only these to have <clinit> or non-default static fields
  } else {
    if (ik->class_initializer() != nullptr) {
      log_info(cds, init)("cannot initialize %s (has <clinit>)", ik->external_name());
      return false;
    }
    if (ik->is_initialized() && !has_non_default_static_fields(ik)) {
      return false;
    }
  }

  return true;
}

bool ClassPreinitializer::has_non_default_static_fields(InstanceKlass* ik) {
  oop mirror = ik->java_mirror();

  for (JavaFieldStream fs(ik); !fs.done(); fs.next()) {
    if (fs.access_flags().is_static()) {
      fieldDescriptor& fd = fs.field_descriptor();
      int offset = fd.offset();
      bool is_default = true;
      bool has_initval = fd.has_initial_value();
      switch (fd.field_type()) {
      case T_OBJECT:
      case T_ARRAY:
        is_default = mirror->obj_field(offset) == nullptr;
        break;
      case T_BOOLEAN:
        is_default = mirror->bool_field(offset) == (has_initval ? fd.int_initial_value() : 0);
        break;
      case T_BYTE:
        is_default = mirror->byte_field(offset) == (has_initval ? fd.int_initial_value() : 0);
        break;
      case T_SHORT:
        is_default = mirror->short_field(offset) == (has_initval ? fd.int_initial_value() : 0);
        break;
      case T_CHAR:
        is_default = mirror->char_field(offset) == (has_initval ? fd.int_initial_value() : 0);
        break;
      case T_INT:
        is_default = mirror->int_field(offset) == (has_initval ? fd.int_initial_value() : 0);
        break;
      case T_LONG:
        is_default = mirror->long_field(offset) == (has_initval ? fd.long_initial_value() : 0);
        break;
      case T_FLOAT:
        is_default = mirror->float_field(offset) == (has_initval ? fd.float_initial_value() : 0);
        break;
      case T_DOUBLE:
        is_default = mirror->double_field(offset) == (has_initval ? fd.double_initial_value() : 0);
        break;
      default:
        ShouldNotReachHere();
      }

      if (!is_default) {
        log_info(cds, init)("cannot initialize %s (static field %s has non-default value)",
                            ik->external_name(), fd.name()->as_C_string());
        return false;
      }
    }
  }

  return true;
}

bool ClassPreinitializer::can_be_preinited(InstanceKlass* ik) {
  MutexLocker ml(DumpTimeTable_lock, Mutex::_no_safepoint_check_flag);
  return can_be_preinited_locked(ik);
}

bool ClassPreinitializer::can_be_preinited_locked(InstanceKlass* ik) {
  if (!CDSConfig::is_initing_classes_at_dump_time()) {
    return false;
  }

  assert_lock_strong(DumpTimeTable_lock);
  DumpTimeClassInfo* info = SystemDictionaryShared::get_info_locked(ik);
  if (!info->has_done_preinit_check()) {
    info->set_can_be_preinited(ClassPreinitializer::check_can_be_preinited(ik));
  }
  return info->can_be_preinited();
}

// Initialize a class at dump time, if possible.
void ClassPreinitializer::maybe_preinit_class(InstanceKlass* ik, TRAPS) {
  if (!ik->is_initialized() && ClassPreinitializer::can_be_preinited(ik)) {
    if (log_is_enabled(Info, cds, init)) {
      ResourceMark rm;
      log_info(cds, init)("preinitializing %s", ik->external_name());
    }
    ik->initialize(CHECK);
  }
}

// ClassPreinitializer::can_be_preinited(ik) is called in two different phases:
//
// [1] Before the VM_PopulateDumpSharedSpace safepoint:
//     when MetaspaceShared::link_shared_classes calls ClassPreinitializer::maybe_preinit_class(ik)
// [2] Inside the VM_PopulateDumpSharedSpace safepoint
//     when HeapShared::archive_java_mirrors() calls ClassPreinitializer::can_archive_preinitialized_mirror(ik)
//
// Between the two phases, some Java code may have been executed to contaminate the
// some initialized mirrors. So we call reset_preinit_check() at the beginning of the
// [2] so that we will re-run has_non_default_static_fields() on all the classes.
// As a result, phase [2] may archive fewer mirrors that were initialized in phase [1].
void ClassPreinitializer::reset_preinit_check() {
  auto iterator = [&] (InstanceKlass* k, DumpTimeClassInfo& info) {
    if (info.can_be_preinited()) {
      info.reset_preinit_check();
    }
  };
  SystemDictionaryShared::dumptime_table()->iterate_all_live_classes(iterator);
}

bool ClassPreinitializer::can_archive_preinitialized_mirror(InstanceKlass* ik) {
  assert(!ArchiveBuilder::current()->is_in_buffer_space(ik), "must be source klass");
  if (!CDSConfig::is_initing_classes_at_dump_time()) {
    return false;
  }

  if (ik->is_hidden()) {
    return HeapShared::is_archivable_hidden_klass(ik);
  } else {
    return ClassPreinitializer::can_be_preinited_locked(ik);
  }
}
