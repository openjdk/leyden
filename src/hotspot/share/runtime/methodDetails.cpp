/*
 * Copyright (c) 1997, 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/metaspaceShared.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/compiledIC.hpp"
#include "code/nmethod.inline.hpp"
#include "code/scopeDesc.hpp"
#include "code/vtableStubs.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/gcLocker.inline.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "jvm.h"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "metaprogramming/primitiveConversions.hpp"
#include "oops/klass.hpp"
#include "oops/method.inline.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/forte.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/jvmtiThreadState.hpp"
#include "prims/methodHandles.hpp"
#include "prims/nativeLookup.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/basicLock.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/init.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/methodDetails.hpp"
#include "runtime/perfData.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stackWatermarkSet.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.inline.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vframeArray.hpp"
#include "runtime/vm_version.hpp"
#include "services/management.hpp"
#include "utilities/copy.hpp"
#include "utilities/dtrace.hpp"
#include "utilities/events.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/resourceHash.hpp"
#include "utilities/macros.hpp"
#include "utilities/xmlstream.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#endif
#if INCLUDE_JFR
#include "jfr/jfr.hpp"
#endif

Symbol* MethodDetails::class_name() {
  if (_class_name == nullptr) {
    if (_methodHandle != nullptr) {
      _class_name = (*_methodHandle)->method_holder()->name();
    } else if (_ciMethod != nullptr) {
      _class_name = _ciMethod->holder()->name()->get_symbol();
    } else if (_method != nullptr) {
      _class_name = _method->method_holder()->name();
    }
  }
  return _class_name;
}

Symbol* MethodDetails::method_name() {
  if (_method_name == nullptr) {
    if (_methodHandle != nullptr) {
      _method_name = (*_methodHandle)->name();
    } else if (_ciMethod != nullptr) {
      _method_name = _ciMethod->name()->get_symbol();
    } else if (_method != nullptr) {
      _method_name = _method->name();
    }
  }
  return _method_name;
}

Symbol* MethodDetails::signature() {
  if (_signature == nullptr) {
    if (_methodHandle != nullptr) {
      _signature = (*_methodHandle)->signature();
    } else if (_ciMethod != nullptr) {
      _signature = _ciMethod->signature()->as_symbol()->get_symbol();
    } else if (_method != nullptr) {
      _signature = _method->signature();
    }
  }
  return _signature;
}
