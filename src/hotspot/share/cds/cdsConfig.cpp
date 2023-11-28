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
#include "cds/archiveHeapLoader.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cds_globals.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classLoaderDataShared.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "logging/log.hpp"
#include "prims/jvmtiExport.hpp"

bool CDSConfig::_has_preloaded_classes;
bool CDSConfig::_is_dumping_dynamic_archive = false;

// The ability to dump the FMG depends on many factors checked by
// is_dumping_full_module_graph(), but can be unconditionally disabled by
// _dumping_full_module_graph_disabled. (Ditto for loading the FMG).
bool CDSConfig::_dumping_full_module_graph_disabled = false;
bool CDSConfig::_loading_full_module_graph_disabled = false;

bool CDSConfig::is_dumping_static_archive() {
  return DumpSharedSpaces || is_dumping_final_static_archive();
}

bool CDSConfig::is_dumping_preimage_static_archive() {
  if (CacheDataStore != nullptr && CDSPreimage == nullptr && DumpSharedSpaces) {
    return true;
  } else {
    return false;
  }
}

bool CDSConfig::is_dumping_final_static_archive() {
  if (CacheDataStore != nullptr && CDSPreimage != nullptr) {
    // [Temp: refactoring in progress] DumpSharedSpaces is sometimes used in contexts
    // that are not compatible with is_dumping_final_static_archive().
    assert(!DumpSharedSpaces, "this flag must not be set");
    return true;
  } else {
    return false;
  }
}

bool CDSConfig::is_dumping_regenerated_lambdaform_invokers() {
  if (is_dumping_final_static_archive()) {
    // Not yet supported in new workflow -- the training data may point
    // to a method in a lambdaform holder class that was not regenerated
    // due to JDK-8318064.
    return false;
  } else {
    return is_dumping_archive();
  }
}

// Preserve all states that were examined used during dumptime verification, such
// that the verification result (pass or fail) cannot be changed at runtime.
//
// For example, if the verification of ik requires that class A must be a subtype of B,
// then this relationship between A and B cannot be changed at runtime. I.e., the app
// cannot load alternative versions of A and B such that A is not a subtype of B.
bool CDSConfig::preserve_all_dumptime_verification_states(const InstanceKlass* ik) {
  return PreloadSharedClasses && SystemDictionaryShared::is_builtin(ik);
}

#if INCLUDE_CDS_JAVA_HEAP
bool CDSConfig::is_dumping_heap() {
  return is_dumping_static_archive() && !is_dumping_preimage_static_archive()
    && HeapShared::can_write();
}

bool CDSConfig::is_loading_heap() {
  return ArchiveHeapLoader::is_in_use();
}

bool CDSConfig::is_dumping_full_module_graph() {
  if (!_dumping_full_module_graph_disabled &&
      is_dumping_heap() &&
      MetaspaceShared::use_optimized_module_handling()) {
    return true;
  } else {
    return false;
  }
}

bool CDSConfig::is_loading_full_module_graph() {
  if (ClassLoaderDataShared::is_full_module_graph_loaded()) {
    return true;
  }

  if (!_loading_full_module_graph_disabled &&
      UseSharedSpaces &&
      ArchiveHeapLoader::can_use() &&
      MetaspaceShared::use_optimized_module_handling()) {
    // Classes used by the archived full module graph are loaded in JVMTI early phase.
    assert(!(JvmtiExport::should_post_class_file_load_hook() && JvmtiExport::has_early_class_hook_env()),
           "CDS should be disabled if early class hooks are enabled");
    return true;
  } else {
    return false;
  }
}

void CDSConfig::disable_dumping_full_module_graph(const char* reason) {
  if (!_dumping_full_module_graph_disabled) {
    _dumping_full_module_graph_disabled = true;
    if (reason != nullptr) {
      log_info(cds)("full module graph cannot be dumped: %s", reason);
    }
  }
}

void CDSConfig::disable_loading_full_module_graph(const char* reason) {
  assert(!ClassLoaderDataShared::is_full_module_graph_loaded(), "you call this function too late!");
  if (!_loading_full_module_graph_disabled) {
    _loading_full_module_graph_disabled = true;
    if (reason != nullptr) {
      log_info(cds)("full module graph cannot be loaded: %s", reason);
    }
  }
}
#endif // INCLUDE_CDS_JAVA_HEAP

// This is allowed by default. We disable it only in the final image dump before the
// metadata and heap are dumped.
static bool _is_dumping_cached_code = true;

bool CDSConfig::is_dumping_cached_code() {
  return _is_dumping_cached_code;
}

void CDSConfig::disable_dumping_cached_code() {
  _is_dumping_cached_code = false;
}

void CDSConfig::enable_dumping_cached_code() {
  _is_dumping_cached_code = true;
}

bool CDSConfig::is_initing_classes_at_dump_time() {
  return is_dumping_heap() && PreloadSharedClasses;
}

bool CDSConfig::is_dumping_invokedynamic() {
  return ArchiveInvokeDynamic && is_dumping_heap();
}
