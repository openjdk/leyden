/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/aotClassLocation.hpp"
#include "cds/customLoaderSupport.hpp"
#include "cds/urlClassLoaderSupport.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.hpp"
#include "oops/oopCast.inline.hpp"
#include "oops/oopHandle.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "utilities/growableArray.hpp"

static InstanceKlass* _urlClassLoader_klass = nullptr;
static GrowableArray<OopHandle>* _urlclassloader_instance_list = nullptr;

static Handle to_file_URL(const char* path, TRAPS) {
  JavaValue result(T_OBJECT);
  Handle path_string = java_lang_String::create_from_str(path, CHECK_NH);
  JavaCalls::call_static(&result,
                         vmClasses::jdk_internal_loader_ClassLoaders_klass(),
                         vmSymbols::toFileURL_name(),
                         vmSymbols::toFileURL_signature(),
                         path_string, CHECK_NH);
  return Handle(THREAD, result.get_oop());
}

void URLClassLoaderSupport::initialize(TRAPS) {
  Symbol* klass_name = SymbolTable::new_symbol("java/net/URLClassLoader");
  Klass* k = SystemDictionary::resolve_or_fail(klass_name, true, CHECK);
  _urlClassLoader_klass = InstanceKlass::cast(k);
  _urlclassloader_instance_list = new (mtClassShared)GrowableArray<OopHandle>(10, mtClassShared);
}

Handle URLClassLoaderSupport::create_urlclassloader(Symbol* parent_id, Array<AOTClassLocation*>* cp_locations, TRAPS) {
  refArrayOop url_array = oopFactory::new_refArray(vmClasses::URL_klass(), cp_locations->length(), CHECK_NH);
  Handle urls(THREAD, url_array);
  for (int i = 0; i < cp_locations->length(); i++) {
    const char* path = cp_locations->at(i)->path();
    Handle url = to_file_URL(path, CHECK_NH);
    oop_cast<refArrayOop>(urls())->obj_at_put(i, url(), CHECK_NH);
  }

  //get parent loader obj using the parent_id
  ClassLoaderData* parent_cld = ClassLoaderAotIdTable::get_cld(parent_id);
  assert(parent_cld != nullptr, "parent class loader data cannot be null");
  Handle parent(THREAD, parent_cld->class_loader());

  Symbol* methodSignature = SymbolTable::new_symbol("([Ljava/net/URL;Ljava/lang/ClassLoader;)V");
  // URLClassLoader classloder = new URLClassLoader(urls, parent);
  Handle classloader = JavaCalls::construct_new_instance(_urlClassLoader_klass,
                                                         methodSignature, urls, parent, CHECK_NH);

  OopHandle cl_handle = OopHandle(Universe::vm_global(), classloader());
  _urlclassloader_instance_list->append(cl_handle);
  return Handle(THREAD, cl_handle.resolve());
}
