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

#ifndef SHARE_RUNTIME_RUNTIME_UPCALLS_HPP
#define SHARE_RUNTIME_RUNTIME_UPCALLS_HPP

#include "memory/allStatic.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/methodDetails.hpp"
#include "utilities/macros.hpp"

enum RuntimeUpcallType{
  onMethodEntry = 0,
  onMethodExit,
  numTypes
};

typedef void (*RuntimeUpcall)(JavaThread* current);
typedef bool (*RuntimeUpcallMethodFilterCallback)(MethodDetails& method);

class RuntimeUpcallInfo: public CHeapObj<mtInternal>{
  const char* _upcallName;
  const RuntimeUpcall _upcall;
  const RuntimeUpcallMethodFilterCallback _methodFilter;
  address _address;
  int _index;

  RuntimeUpcallInfo(const char* upcallName,
                    const RuntimeUpcall upcall,
                    const RuntimeUpcallMethodFilterCallback methodFilter)
  : _upcallName(upcallName),
    _upcall(upcall),
    _methodFilter(methodFilter),
    _index(-1) {
    _address = CAST_FROM_FN_PTR(address, upcall);
  }

private:
  friend class RuntimeUpcalls;
  void set_index(const int index) { _index = index; }
  int get_index() const { assert(_index >= 0, "invalid index"); return _index; }

public:
  static RuntimeUpcallInfo* Create(const char* upcallName, const RuntimeUpcall upcall, const RuntimeUpcallMethodFilterCallback methodFilter) {
    assert(upcallName != nullptr, "upcall name must be provided");
    assert(upcall != nullptr, "upcall must be provided");
    assert(methodFilter != nullptr, "method filter must be provided");
    return new RuntimeUpcallInfo(upcallName, upcall, methodFilter);
  }

  RuntimeUpcall upcall() const { return _upcall; }
  const char* upcall_name() const { return _upcallName; }
  address upcall_address() const { return _address; }

  bool includes(MethodDetails& methodDetails) const {
    return _methodFilter(methodDetails);
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
  static void upcall_redirect(RuntimeUpcallType upcallType, JavaThread* current, Method* method);

  static int  get_num_upcalls(RuntimeUpcallType upcallType);

  static void on_method_entry_upcall_redirect(JavaThread* current, Method* method);
  static void on_method_exit_upcall_redirect(JavaThread* current, Method* method);

public:

  static bool               open_upcall_registration();
  static bool               register_upcall(RuntimeUpcallType upcallType, const char* upcallName, RuntimeUpcall upcall, RuntimeUpcallMethodFilterCallback methodFilterCallback = nullptr);
  static void               close_upcall_registration();

  static void               install_upcalls(const methodHandle& method);

  static RuntimeUpcallInfo* get_first_upcall(RuntimeUpcallType upcallType, MethodDetails& methodDetails);
  static RuntimeUpcallInfo* get_next_upcall(RuntimeUpcallType upcallType, MethodDetails& methodDetails, RuntimeUpcallInfo* prevUpcallInfo = nullptr);

  static address            on_method_entry_upcall_address();
  static address            on_method_exit_upcall_address();

  static bool               does_upcall_need_method_parameter(address upcall_address);

  static const char*        get_name_for_upcall_address(address upcall_address);
};

#endif // SHARE_RUNTIME_RUNTIME_UPCALLS_HPP
