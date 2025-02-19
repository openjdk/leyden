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

#include "compiler/abstractCompiler.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/methodDetails.hpp"

Symbol* MethodDetails::class_name() {
  if (_class_name == nullptr) {
    if (_method_handle != nullptr) {
      _class_name = (*_method_handle)->method_holder()->name();
    } else if (_ci_method != nullptr) {
      _class_name = _ci_method->holder()->name()->get_symbol();
    } else if (_method != nullptr) {
      _class_name = _method->method_holder()->name();
    }
  }
  return _class_name;
}

Symbol* MethodDetails::method_name() {
  if (_method_name == nullptr) {
    if (_method_handle != nullptr) {
      _method_name = (*_method_handle)->name();
    } else if (_ci_method != nullptr) {
      _method_name = _ci_method->name()->get_symbol();
    } else if (_method != nullptr) {
      _method_name = _method->name();
    }
  }
  return _method_name;
}

Symbol* MethodDetails::signature() {
  if (_signature == nullptr) {
    if (_method_handle != nullptr) {
      _signature = (*_method_handle)->signature();
    } else if (_ci_method != nullptr) {
      _signature = _ci_method->signature()->as_symbol()->get_symbol();
    } else if (_method != nullptr) {
      _signature = _method->signature();
    }
  }
  return _signature;
}
