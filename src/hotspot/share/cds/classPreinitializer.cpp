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
#include "cds/heapShared.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "memory/resourceArea.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/instanceKlass.hpp"

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
    nullptr
  };

  for (int i = 0; ; i++) {
    const char* class_name = forced_preinit_classes[i];
    if (class_name == nullptr) {
      return false;
    }
    if (ik->name()->equals(class_name)) {
      if (log_is_enabled(Info, cds, init)) {
        ResourceMark rm;
        log_info(cds, init)("Force initialization %s", ik->external_name());
      }
      return true;
    }
  }
}

bool ClassPreinitializer::check_can_be_preinited(InstanceKlass* ik) {
  ResourceMark rm;

  if (!SystemDictionaryShared::is_builtin(ik)) {
    log_info(cds, init)("cannot initialize %s (not built-in loader)", ik->external_name());
    return false;
  }

  InstanceKlass* super = ik->java_super();
  if (super != nullptr && !SystemDictionaryShared::can_be_preinited_locked(super)) {
    log_info(cds, init)("cannot initialize %s (super %s not initable)", ik->external_name(), super->external_name());
    return false;
  }

  Array<InstanceKlass*>* interfaces = ik->local_interfaces();
  for (int i = 0; i < interfaces->length(); i++) {
    if (!SystemDictionaryShared::can_be_preinited_locked(interfaces->at(i))) {
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

// Initialize a class at dump time, if possible.
void ClassPreinitializer::maybe_preinit_class(InstanceKlass* ik, TRAPS) {
  if (!ik->is_initialized() && SystemDictionaryShared::can_be_preinited(ik)) {
    if (log_is_enabled(Info, cds, init)) {
      ResourceMark rm;
      log_info(cds, init)("preinitializing %s", ik->external_name());
    }
    ik->initialize(CHECK);
  }
}

bool ClassPreinitializer::can_archive_preinitialized_mirror(InstanceKlass* ik) {
  assert(!ArchiveBuilder::current()->is_in_buffer_space(ik), "must be source klass");
  if (!CDSConfig::is_initing_classes_at_dump_time()) {
    return false;
  }

  if (ik->is_hidden()) {
    return HeapShared::is_archivable_hidden_klass(ik);
  } else {
    return SystemDictionaryShared::can_be_preinited_locked(ik);
  }
}
