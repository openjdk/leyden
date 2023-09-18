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
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "oops/instanceKlass.hpp"

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

int CDSAccess::get_archived_object_permanent_index(oop obj) {
  if (CDSConfig::is_dumping_final_static_archive()) {
    // TODO: not implemented yet
    return -1;
  } else {
    return HeapShared::get_archived_object_permanent_index(obj);
  }
}
