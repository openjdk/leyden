/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/cdsAccess.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/filemap.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/stringTable.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "memory/virtualspace.hpp"
#include "oops/instanceKlass.hpp"

size_t _cached_code_size = 0;

bool CDSAccess::can_generate_cached_code(address addr) {
  if (CDSConfig::is_dumping_final_static_archive()) {
    return ArchiveBuilder::is_active() && ArchiveBuilder::current()->has_been_archived(addr);
  } else {
    // Old CDS+AOT workflow.
    return MetaspaceShared::is_in_shared_metaspace(addr);
  }
}

bool CDSAccess::can_generate_cached_code(InstanceKlass* ik) {
  if (CDSConfig::is_dumping_final_static_archive()) {
    if (!ArchiveBuilder::is_active()) {
      return false;
    }
    ArchiveBuilder* builder = ArchiveBuilder::current();
    if (!builder->has_been_archived((address)ik)) {
      return false;
    }
    InstanceKlass* buffered_ik = builder->get_buffered_addr(ik);
    if (ik->is_shared_unregistered_class()) {
      return false;
    }
    return true;
  } else {
    // Old CDS+AOT workflow.
    return ik->is_shared() && !ik->is_shared_unregistered_class();
  }
}

uint CDSAccess::delta_from_shared_address_base(address addr) {
  if (CDSConfig::is_dumping_final_static_archive()) {
    assert(ArchiveBuilder::is_active(), "must be");
    ArchiveBuilder* builder = ArchiveBuilder::current();
    address requested_addr = builder->to_requested(builder->get_buffered_addr(addr));
    return (uint)pointer_delta(requested_addr, (address)SharedBaseAddress, 1);
  } else {
    // Old CDS+AOT workflow.
    return (uint)pointer_delta(addr, (address)SharedBaseAddress, 1);
  }
}

Method* CDSAccess::method_in_cached_code(Method* m) {
  if (CDSConfig::is_dumping_final_static_archive()) {
    assert(ArchiveBuilder::is_active(), "must be");
    ArchiveBuilder* builder = ArchiveBuilder::current();
    return builder->to_requested(builder->get_buffered_addr(m));
  } else {
    // Old CDS+AOT workflow.
    return m;
  }
}

#ifdef INCLUDE_CDS_JAVA_HEAP
int CDSAccess::get_archived_object_permanent_index(oop obj) {
  return HeapShared::get_archived_object_permanent_index(obj);
}

oop CDSAccess::get_archived_object(int permanent_index) {
  return HeapShared::get_archived_object(permanent_index);
}

static void test_cds_heap_access_api_for_object(oop obj) {
  LogStreamHandle(Info, cds, jit) log;

  obj->print_on(&log);
  log.cr();

  int n = CDSAccess::get_archived_object_permanent_index(obj); // call this when -XX:+StoreCachedCode
  if (n < 0) {
    log.print_cr("*** This object is not in CDS archive");
  } else {
    log.print_cr("CDSAccess::get_archived_object_permanent_index(s) = %d", n);
    oop archived_obj = CDSAccess::get_archived_object(n); // call this when -XX:+LoadCachedCode
    if (archived_obj == obj || archived_obj == HeapShared::orig_to_scratch_object(obj)) {
      log.print_cr("CDSAccess::get_archived_object(%d) returns the same object, as expected", n);
    } else {
      log.print_cr("Error!!! CDSAccess::get_archived_object(%d) returns an unexpected object", n);
      if (archived_obj == nullptr) {
        log.print_cr("--> null");
      } else {
        archived_obj->print_on(&log);
        log.cr();
      }
    }
  }
}

// TEMP: examples for using the CDSAccess::get_archived_object_permanent_index() and CDSAccess::get_archived_object()
// APIs for the AOT compiler.

void CDSAccess::test_heap_access_api() {
  ResourceMark rm;
  const char* tests[] = {
    "",
    "null",
    "NARROW",
    "not in cds",
    nullptr,
  };

  LogStreamHandle(Info, cds, jit) log;

  int i;
  for (i = 0; tests[i] != nullptr; i++) {
    EXCEPTION_MARK;
    log.print_cr("Test %d ======================================== \"%s\"", i, tests[i]);
    oop s = StringTable::intern(tests[i], CHECK);
    test_cds_heap_access_api_for_object(s);
  }

  log.print_cr("Test %d ======================================== Universe::null_ptr_exception_instance()", i);
  test_cds_heap_access_api_for_object(Universe::null_ptr_exception_instance());
}

// new workflow only
void* CDSAccess::allocate_from_code_cache(size_t size) {
  assert(CDSConfig::is_dumping_final_static_archive(), "must be");
  return (void*)ArchiveBuilder::cc_region_alloc(size);
}

size_t CDSAccess::get_cached_code_size() {
  return _cached_code_size;
}

void CDSAccess::set_cached_code_size(size_t sz) {
  _cached_code_size = sz;
}

void CDSAccess::set_pointer(address* ptr, address value) {
  ArchiveBuilder* builder = ArchiveBuilder::current();
  if (value != nullptr && !builder->is_in_buffer_space(value)) {
    value = builder->get_buffered_addr(value);
  }
  *ptr = value;
  ArchivePtrMarker::mark_pointer(ptr);
}

bool CDSAccess::map_cached_code(ReservedSpace rs) {
  FileMapInfo* static_mapinfo = FileMapInfo::current_info();
  assert(UseSharedSpaces && static_mapinfo != nullptr, "must be");
  return static_mapinfo->map_cached_code_region(rs);
}

#endif // INCLUDE_CDS_JAVA_HEAP
