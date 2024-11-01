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
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/methodDetails.hpp"
#include "runtime/runtimeUpcalls.hpp"

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
    MethodDetails md(method);
    for(RuntimeUpcallInfo* info : *(_upcalls[upcallType])) {
      if(info->includes(md)) {
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
  info->set_index(_upcalls[upcallType]->length());
  _upcalls[upcallType]->append(info);
  return true;
}

int RuntimeUpcalls::get_num_upcalls(RuntimeUpcallType upcallType) {
  return (_upcalls[upcallType] == nullptr) ? 0 : _upcalls[upcallType]->length();
}

void RuntimeUpcalls::upcall_redirect(RuntimeUpcallType upcallType, JavaThread* current, Method* method) {
  MethodDetails md(method);

  // one possible optimization here is to use the method flags bits to indicate which upcalls to call
  // currently we've used only two bits to support entry/exit, but we could use more bits to support
  // more upcalls. This would require a change in the MethodFlags class to add more
  // flags and the corresponding getters/setters. The flags could be set in the mark_for_upcalls
  // method and checked here to determine which upcalls to call.  Allowing us to skip pattern matching
  // the method flags curently use 18 out of 32 bits, so there are still 14 bits available for use.
  // we could set a limit of say 4-8 entry/exit upcalls combined, leaving 10-6 bits for other uses
  // this still requires a redirect heree, unless we encode bitwise tests in the method prologue/epilogue
  // when we generate the prologs we already know how many upcalls there are so it would be optimial

  //  status(has_upcall_on_method_entry  , 1 << 17) \
  // status(has_upcall_on_method_exit   , 1 << 18) \

  RuntimeUpcallInfo* upcall = get_first_upcall(upcallType, md);
  while (upcall != nullptr) {
    upcall->upcall()(current);
    upcall = get_next_upcall(upcallType, md, upcall);
  }
}

JRT_BLOCK_ENTRY(void, RuntimeUpcalls::on_method_entry_upcall_redirect(JavaThread* current, Method* method)) {
    RuntimeUpcalls::upcall_redirect(onMethodEntry, current, method);
}
JRT_END

JRT_BLOCK_ENTRY(void, RuntimeUpcalls::on_method_exit_upcall_redirect(JavaThread* current, Method* method)) {
    RuntimeUpcalls::upcall_redirect(onMethodExit, current, method);
}
JRT_END

//-------------------------------RuntimeUpcalls---------------------------------------

void RuntimeUpcalls::install_upcalls(const methodHandle& method) {
  for (int i = 0; i < RuntimeUpcallType::numTypes; i++) {
      mark_for_upcalls(static_cast<RuntimeUpcallType>(i), method);
  }
}

bool RuntimeUpcalls::register_upcall(RuntimeUpcallType upcallType, const char* upcallName, RuntimeUpcall upcall, RuntimeUpcallMethodFilterCallback methodFilterCallback)
{
  assert(upcallType < numTypes, "invalid upcall type");
  assert(_state == Open, "upcalls are not open for registration");
  if (_state != Open) return false;
  return register_upcall(upcallType, RuntimeUpcallInfo::Create(upcallName, upcall, methodFilterCallback));
}

RuntimeUpcallInfo* RuntimeUpcalls::get_next_upcall(RuntimeUpcallType upcallType, MethodDetails& methodDetails, RuntimeUpcallInfo* prevUpcallInfo) {
  assert(upcallType < numTypes, "invalid upcall type");
  if (_upcalls[upcallType] != nullptr) {
    // simple case where there's only one upcall
    if (_upcalls[upcallType]->length() == 1) {
      if (prevUpcallInfo != nullptr) {
        return nullptr;
      }
      RuntimeUpcallInfo* upcall = _upcalls[upcallType]->at(0);
      return upcall->includes(methodDetails) ? upcall : nullptr;
    }

    // resume from where we left off, unless we are the last entry
    assert(prevUpcallInfo == nullptr || (prevUpcallInfo->get_index() >= 0 && prevUpcallInfo->get_index() < _upcalls[upcallType]->length()), "invalid upcall index");
    int index = (prevUpcallInfo != nullptr) ? prevUpcallInfo->get_index() + 1 : 0;
    for (int i = index; i < _upcalls[upcallType]->length(); i++) {
      RuntimeUpcallInfo* upcall = _upcalls[upcallType]->at(i);
      if (upcall->includes(methodDetails)) {
        return upcall;
      }
    }
  }

  return nullptr;
}

RuntimeUpcallInfo* RuntimeUpcalls::get_first_upcall(RuntimeUpcallType upcallType, MethodDetails& methodDetails) {
  return get_next_upcall(upcallType, methodDetails, nullptr);
}

bool RuntimeUpcalls::does_upcall_need_method_parameter(address upcall_address)
{
  // redirect needs the method parameter for filtering
  if((upcall_address == CAST_FROM_FN_PTR(address, RuntimeUpcalls::on_method_entry_upcall_redirect)) ||
     (upcall_address == CAST_FROM_FN_PTR(address, RuntimeUpcalls::on_method_exit_upcall_redirect))) {
    return true;
  }

  return false;
}

address RuntimeUpcalls::on_method_entry_upcall_address()
{
  // optimised case when there's only one upcall (no need to redirect)
  if(_upcalls[onMethodEntry] != nullptr && _upcalls[onMethodEntry]->length() == 1) {
    return _upcalls[onMethodEntry]->at(0)->upcall_address();
  }

  return CAST_FROM_FN_PTR(address, RuntimeUpcalls::on_method_entry_upcall_redirect);
}

address RuntimeUpcalls::on_method_exit_upcall_address()
{
  // optimised case when there's only one upcall (no need to redirect)
  if(_upcalls[onMethodExit] != nullptr && _upcalls[onMethodExit]->length() == 1) {
    return _upcalls[onMethodExit]->at(0)->upcall_address();
  }

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