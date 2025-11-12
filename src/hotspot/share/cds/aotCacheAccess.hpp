/*
 * Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_CDS_AOTCACHEACCESS_HPP
#define SHARE_CDS_AOTCACHEACCESS_HPP

#include "cds/archiveBuilder.hpp"
#include "cds/archiveUtils.hpp"
#include "memory/allStatic.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/globalDefinitions.hpp"

class InstanceKlass;
class Klass;
class Method;
class ReservedSpace;

class AOTCacheAccess : AllStatic {
private:
  static bool can_generate_aot_code(address addr) NOT_CDS_RETURN_(false);
public:
  static bool can_generate_aot_code(Method* m) {
    return can_generate_aot_code((address)m);
  }
  static bool can_generate_aot_code(Klass* k) {
    assert(!k->is_instance_klass(), "other method should be called");
    return can_generate_aot_code((address)k);
  }
  static bool can_generate_aot_code_for(InstanceKlass* ik) NOT_CDS_RETURN_(false);

  /*
   * Used during an assembly run to compute the offset of the metadata object in the AOT Cache.
   * The input argument is the "source" address of a metadata object (Method/Klass) loaded by the assembly JVM.
   * Computation of the offset requires mapping the supplied metadata object to its "requested" address
   * and subtracting that address from the requested base address.
   * See ArchiveBuilder.hpp for definition of "source" and "requested" address.
   */
  static uint delta_from_base_address(address addr);

  /*
   * Used during a production run to materialize a pointer to a Klass located in a loaded AOT Cache.
   * The offset argument identifies a delta from the AOT Cache's currently mapped base address to the start of the Klass object.
   * The offset is normally obtained by reading a value embedded in some other AOT-ed entry, like an AOT compiled code.
   */
  static Klass* convert_offset_to_klass(uint offset_from_base_addr) {
    Metadata* metadata = (Metadata*)((address)SharedBaseAddress + offset_from_base_addr);
    assert(metadata->is_klass(), "sanity check");
    return (Klass*)metadata;
  }

  /*
   * Used during a production run to materialize a pointer to a Method located in a loaded AOT Cache.
   * The offset argument identifies a delta from the AOT Cache's currently mapped base address to the start of the Method object.
   * The offset is normally obtained by reading a value embedded in some other AOT-ed entry, like an AOT compiled code.
   */
  static Method* convert_offset_to_method(uint offset_from_base_addr) {
    Metadata* metadata = (Metadata*)((address)SharedBaseAddress + offset_from_base_addr);
    assert(metadata->is_method(), "sanity check");
    return (Method*)metadata;
  }

  // Used during production run to convert a Method in AOTCache to offset from SharedBaseAddress
  static uint convert_method_to_offset(Method* method);

  static int get_archived_object_permanent_index(oop obj) NOT_CDS_JAVA_HEAP_RETURN_(-1);
  static oop get_archived_object(int permanent_index) NOT_CDS_JAVA_HEAP_RETURN_(nullptr);

  static void* allocate_aot_code_region(size_t size) NOT_CDS_RETURN_(nullptr);

  static size_t get_aot_code_region_size() NOT_CDS_RETURN_(0);
  static void set_aot_code_region_size(size_t sz) NOT_CDS_RETURN;

  static bool map_aot_code_region(ReservedSpace rs) NOT_CDS_RETURN_(false);

  static bool is_aot_code_region_empty() NOT_CDS_RETURN_(true);

  template <typename T>
  static void set_pointer(T** ptr, T* value) {
    set_pointer((address*)ptr, (address)value);
  }
  static void set_pointer(address* ptr, address value);
};

#endif // SHARE_CDS_AOTCACHEACCESS_HPP
