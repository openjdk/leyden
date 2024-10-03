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

#ifndef SHARE_RUNTIME_UPCALLS_HPP
#define SHARE_RUNTIME_UPCALLS_HPP

#include "code/codeBlob.hpp"
#include "code/vmreg.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/allStatic.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/macros.hpp"

class ciMethod;

class MethodDetails: public CHeapObj<mtInternal> {
private:
  Symbol* _class_name;
  Symbol* _method_name;
  Symbol* _signature;

  MethodDetails(Symbol* class_name, Symbol* method_name, Symbol* signature) : _class_name(class_name), _method_name(method_name), _signature(signature) {};
public:
  static MethodDetails* Create(const methodHandle& method);
  static MethodDetails* Create(const ciMethod* method);
  static MethodDetails* Create(const Method* method);

  Symbol* class_name() const { return _class_name; }
  Symbol* method_name() const { return _method_name; }
  Symbol* signature() const { return _signature; }
};

class MethodPattern: public CHeapObj<mtInternal> {
private:
  MethodPattern(const char* methodPattern) {};
public:
  static MethodPattern* Create(const char* methodPattern) { return new MethodPattern(methodPattern); }

  bool matches(const MethodDetails* method) const {
    return false;
  }
};

enum RuntimeUpcallType{
  onMethodEntry = 0,
  onMethodExit,

  numTypes,
  begin = onMethodEntry,
  end = numTypes
};

typedef void (*RuntimeUpcall)(JavaThread* current);

class RuntimeUpcallInfo: public CHeapObj<mtInternal>{
  const MethodPattern* _methodPattern;
  const char* _upcallName;
  const RuntimeUpcall _upcall;
  address _address;

  RuntimeUpcallInfo(const MethodPattern* methodPattern, const char* upcallName, const RuntimeUpcall upcall)
  : _methodPattern(methodPattern), _upcallName(upcallName), _upcall(upcall) {
    _address = CAST_FROM_FN_PTR(address, upcall);
  }

public:
  static RuntimeUpcallInfo* Create(const MethodPattern* methodPattern, const char* upcallName, const RuntimeUpcall upcall) {
    return new RuntimeUpcallInfo(methodPattern, upcallName, upcall);
  }

  RuntimeUpcall upcall() const { return _upcall; }
  const char* upcall_name() const { return _upcallName; }
  address upcall_address() const { return _address; }

  bool includes(const MethodDetails* method) const {
    if (_methodPattern == nullptr) return false;
    return _methodPattern->matches(method);
  }
};

class RuntimeUpcalls: AllStatic {
private:

  enum State {
    Uninitialized,
    Open,
    Closed
  };

  static GrowableArray<RuntimeUpcallInfo*>* _upcalls[RuntimeUpcallType::numTypes];
  static State _state;

  static void mark_for_upcalls(RuntimeUpcallType upcallType, const methodHandle& method);
  static bool register_upcall(RuntimeUpcallType upcallType, RuntimeUpcallInfo* info);
  static void upcall_redirect(RuntimeUpcallType upcallType, JavaThread* current);//, Method* method);

  static RuntimeUpcallInfo* get_first_upcall(RuntimeUpcallType upcallType, MethodDetails* method);
  static RuntimeUpcallInfo* get_next_upcall(RuntimeUpcallType upcallType, MethodDetails* method, RuntimeUpcallInfo* prev);

public:

  static void on_method_entry_upcall_redirect(JavaThread* current);//, Method* method);
  static void on_method_exit_upcall_redirect(JavaThread* current);//, Method* method);

  static bool               open_upcall_registration();
  static void               close_upcall_registration();
  static void               install_upcalls(const methodHandle& method);
  static bool               register_upcall(RuntimeUpcallType upcallType, const char* methodPattern, const char* upcallName, RuntimeUpcall upcall);
  static bool               register_upcall(RuntimeUpcallType upcallType, const MethodPattern* methodPattern, const char* upcallName, RuntimeUpcall upcall);

  static RuntimeUpcallInfo* get_first_upcall(RuntimeUpcallType upcallType, ciMethod* method);
  static RuntimeUpcallInfo* get_next_upcall(RuntimeUpcallType upcallType, ciMethod* method, RuntimeUpcallInfo* prev);
  static RuntimeUpcallInfo* get_first_upcall(RuntimeUpcallType upcallType, Method* method);
  static RuntimeUpcallInfo* get_next_upcall(RuntimeUpcallType upcallType, Method* method, RuntimeUpcallInfo* prev);

  static address            on_method_entry_upcall_address();
  static address            on_method_exit_upcall_address();
};

#endif // SHARE_RUNTIME_RUNTIME_UPCALLS_HPP
