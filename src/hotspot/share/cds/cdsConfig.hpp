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

#ifndef SHARE_CDS_CDSCONFIG_HPP
#define SHARE_CDS_CDSCONFIG_HPP

#include "memory/allStatic.hpp"
#include "utilities/macros.hpp"

class InstanceKlass;

class CDSConfig : public AllStatic {
#if INCLUDE_CDS
  static bool _is_dumping_static_archive;
  static bool _is_dumping_dynamic_archive;
  static bool _dumping_full_module_graph_disabled;
  static bool _loading_full_module_graph_disabled;
  static bool _has_preloaded_classes;
#endif

public:
  // Basic CDS features
  static bool      is_dumping_archive()                      { return is_dumping_static_archive() || is_dumping_dynamic_archive(); }
  static bool      is_dumping_static_archive()               { return (CDS_ONLY(_is_dumping_static_archive) NOT_CDS(false))
                                                                    || is_dumping_final_static_archive(); }
  static void  enable_dumping_static_archive()               { CDS_ONLY(_is_dumping_static_archive = true); }
  static bool      is_dumping_preimage_static_archive()      NOT_CDS_RETURN_(false);
  static bool      is_dumping_final_static_archive()         NOT_CDS_RETURN_(false);
  static bool      is_dumping_dynamic_archive()              { return CDS_ONLY(_is_dumping_dynamic_archive) NOT_CDS(false); }
  static void  enable_dumping_dynamic_archive()              { CDS_ONLY(_is_dumping_dynamic_archive = true); }
  static void disable_dumping_dynamic_archive()              { CDS_ONLY(_is_dumping_dynamic_archive = false); }
  static bool      has_preloaded_classes()                   { CDS_ONLY(return _has_preloaded_classes); NOT_CDS(return false); }
  static void      set_has_preloaded_classes()               { CDS_ONLY(_has_preloaded_classes = true); }
  static bool      is_dumping_regenerated_lambdaform_invokers() NOT_CDS_RETURN_(false);

  // Misc CDS features
  static bool      preserve_all_dumptime_verification_states(const InstanceKlass* ik);

  // CDS archived heap
  static bool      is_dumping_heap()                         NOT_CDS_JAVA_HEAP_RETURN_(false);
  static bool      is_loading_heap()                         NOT_CDS_JAVA_HEAP_RETURN_(false);
  static void disable_dumping_full_module_graph(const char* reason = nullptr) NOT_CDS_JAVA_HEAP_RETURN;
  static bool      is_dumping_full_module_graph()            NOT_CDS_JAVA_HEAP_RETURN_(false);
  static void disable_loading_full_module_graph(const char* reason = nullptr) NOT_CDS_JAVA_HEAP_RETURN;
  static bool      is_loading_full_module_graph()            NOT_CDS_JAVA_HEAP_RETURN_(false);
  static bool      is_dumping_invokedynamic()                NOT_CDS_JAVA_HEAP_RETURN_(false);
  static bool      is_initing_classes_at_dump_time()         NOT_CDS_JAVA_HEAP_RETURN_(false);

  // AOT compiler
  static bool      is_dumping_cached_code()                  NOT_CDS_RETURN_(false);
  static void disable_dumping_cached_code()                  NOT_CDS_RETURN;
  static void  enable_dumping_cached_code()                  NOT_CDS_RETURN;
};

#endif // SHARE_CDS_CDSCONFIG_HPP
