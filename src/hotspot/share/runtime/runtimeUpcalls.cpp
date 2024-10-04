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
#include "compiler/compilerOracle.hpp"
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
#include "runtime/perfData.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stackWatermarkSet.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.inline.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/runtimeUpcalls.hpp"
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


MethodDetails* MethodDetails::Create(const methodHandle& method) {
  return new MethodDetails(method->method_holder()->name(), method->name(), method->signature());
}

MethodDetails* MethodDetails::Create(const ciMethod* method) {
  return new MethodDetails(method->holder()->name()->get_symbol(), method->name()->get_symbol(), method->signature()->as_symbol()->get_symbol());
}

MethodDetails* MethodDetails::Create(const Method* method) {
  return new MethodDetails(method->method_holder()->name(), method->name(), method->signature());
}

//--------------------------- initialization -------------------------------------
GrowableArray<RuntimeUpcallInfo*>* RuntimeUpcalls::_upcalls[RuntimeUpcallType::numTypes];
RuntimeUpcalls::State RuntimeUpcalls::_state = RuntimeUpcalls::Uninitialized;

bool runtimeUpcalls_open_registration() {
  return RuntimeUpcalls::open_upcall_registration();
}

bool RuntimeUpcalls::open_upcall_registration() {
  assert(_state == Uninitialized, "upcalls are already open");
  for (int i = 0; i < RuntimeUpcallType::numTypes; i++) {
    RuntimeUpcalls::_upcalls[i] = nullptr;
  }
  _state = Open;
  return true;
}

void runtimeUpcalls_close_registration() {
  RuntimeUpcalls::close_upcall_registration();
}

void RuntimeUpcalls::close_upcall_registration() {
  assert(_state == Open, "upcalls are not open");
  _state = Closed;
}

//--------------------------- private -------------------------------------
void RuntimeUpcalls::mark_for_upcalls(RuntimeUpcallType upcallType, const methodHandle& method) {
  if (_upcalls[upcallType] != nullptr) {
    MethodDetails* methodDetails = MethodDetails::Create(method());
    // see if method matches the pattern for any of the upcalls
    for(RuntimeUpcallInfo* info : *(_upcalls[upcallType])) {
      if(CompilerOracle::should_trigger_end_of_training_at(method)) {
        switch(upcallType) {
          case onMethodEntry: method->set_has_upcall_on_method_entry(true); break;
          case onMethodExit:  method->set_has_upcall_on_method_exit(true);  break;
          default:            ShouldNotReachHere();
        }
        break;
      }
    }
  }
}

bool RuntimeUpcalls::register_upcall(RuntimeUpcallType upcallType, RuntimeUpcallInfo* info) {
  assert(info != nullptr, "upcall info is null");
  if (_upcalls[upcallType] == nullptr) {
    _upcalls[upcallType] = new (mtServiceability) GrowableArray<RuntimeUpcallInfo*>(1, mtServiceability);
  }
  _upcalls[upcallType]->append(info);
  return true;
}

void RuntimeUpcalls::upcall_redirect(RuntimeUpcallType upcallType, JavaThread* current)//, Method* method)
{
    // call all upcalls for method
    MethodDetails* method = nullptr;
    RuntimeUpcallInfo* upcall = get_first_upcall(upcallType, method);
    while (upcall != nullptr) {
      upcall->upcall()(current);
      upcall = get_next_upcall(upcallType, method, upcall);
    }
}

// used by the interpreter which can only install 1 hook for now
void RuntimeUpcalls::on_method_entry_upcall_redirect(TRAPS)//JavaThread* current)//, Method* method)
{
  upcall_redirect(onMethodEntry, CHECK);//current, method);
}

// used by the interpreter which can only install 1 hook for now
void RuntimeUpcalls::on_method_exit_upcall_redirect(TRAPS)//JavaThread* current)//, Method* method)
{
  upcall_redirect(onMethodExit, CHECK);//current, method);
}

//-------------------------------RuntimeUpcalls---------------------------------------

void RuntimeUpcalls::install_upcalls(const methodHandle& method)
{
  for (int i = 0; i < RuntimeUpcallType::numTypes; i++) {
      mark_for_upcalls(static_cast<RuntimeUpcallType>(i), method);
  }
}

bool RuntimeUpcalls::register_upcall(RuntimeUpcallType upcallType, const MethodPattern* methodPattern, const char* upcallName, RuntimeUpcall upcall) {
  assert(upcallType < numTypes, "invalid upcall type");
  assert(_state == Open, "upcalls are not open for registration");
  if (_state != Open) return false;
  return register_upcall(upcallType, RuntimeUpcallInfo::Create(methodPattern, upcallName, upcall));
}

bool RuntimeUpcalls::register_upcall(RuntimeUpcallType upcallType, const char* methodPattern, const char* upcallName, RuntimeUpcall upcall) {
  assert(upcallType < numTypes, "invalid upcall type");
  assert(_state == Open, "upcalls are not open for registration");
  if (_state != Open) return false;
  return register_upcall(upcallType, RuntimeUpcallInfo::Create(MethodPattern::Create(methodPattern), upcallName, upcall));
}

RuntimeUpcallInfo* RuntimeUpcalls::get_next_upcall(RuntimeUpcallType upcallType, MethodDetails* method, RuntimeUpcallInfo* prev = nullptr) {
  assert(upcallType < numTypes, "invalid upcall type");
  if (_upcalls[upcallType] != nullptr) {
    // simple case where there's only one upcall
    if (_upcalls[upcallType]->length() == 1) {
      if (prev != nullptr) {
        return nullptr;
      }
      RuntimeUpcallInfo* upcall = _upcalls[upcallType]->at(0);
      return upcall;// MNCMNC upcall->includes(method) ? upcall : nullptr;
    }

    // resume from where we left off, unless we are the last entry
    int index = 0;
    if (prev != nullptr) {
      index = _upcalls[upcallType]->find(prev);
      if (index == _upcalls[upcallType]->length() - 1) {
        return nullptr;
      }
      index = index + 1;
    }
    for (int i = index; i < _upcalls[upcallType]->length(); i++) {
      RuntimeUpcallInfo* upcall = _upcalls[upcallType]->at(i);
      if (true) { //upcall->includes(method)) {
        return upcall;
      }
    }
  }

  return nullptr;
}

RuntimeUpcallInfo* RuntimeUpcalls::get_next_upcall(RuntimeUpcallType upcallType, ciMethod* method, RuntimeUpcallInfo* prev = nullptr) {
  return get_next_upcall(upcallType, MethodDetails::Create(method), prev);
}

RuntimeUpcallInfo* RuntimeUpcalls::get_next_upcall(RuntimeUpcallType upcallType, Method* method, RuntimeUpcallInfo* prev = nullptr) {
  return get_next_upcall(upcallType, MethodDetails::Create(method), prev);
}

RuntimeUpcallInfo* RuntimeUpcalls::get_first_upcall(RuntimeUpcallType upcallType, MethodDetails* method) {
  return get_next_upcall(upcallType, method, nullptr);
}

RuntimeUpcallInfo* RuntimeUpcalls::get_first_upcall(RuntimeUpcallType upcallType, ciMethod* method) {
  return get_next_upcall(upcallType, MethodDetails::Create(method), nullptr);
}

RuntimeUpcallInfo* RuntimeUpcalls::get_first_upcall(RuntimeUpcallType upcallType, Method* method) {
  return get_next_upcall(upcallType, MethodDetails::Create(method), nullptr);
}

address RuntimeUpcalls::on_method_entry_upcall_address()
{
  return CAST_FROM_FN_PTR(address, RuntimeUpcalls::on_method_entry_upcall_redirect);
}

address RuntimeUpcalls::on_method_exit_upcall_address()
{
  return CAST_FROM_FN_PTR(address, RuntimeUpcalls::on_method_exit_upcall_redirect);
}

const char* RuntimeUpcalls::get_name_for_upcall_address(address upcall_address)
{
  for(int i = 0; i < RuntimeUpcallType::numTypes; i++) {
    if (_upcalls[i] != nullptr) {
      for (int j = 0; j < _upcalls[i]->length(); j++) {
        RuntimeUpcallInfo* upcall = _upcalls[i]->at(j);
        if (upcall->upcall_address() == upcall_address) {
          return upcall->upcall_name();
        }
      }
    }
  }
  return nullptr;
}