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

#ifndef SHARED_CDS_METHODDATAINFO_HPP
#define SHARED_CDS_METHODDATAINFO_HPP

#include "cds/archiveUtils.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/compactHashtable.hpp"
#include "classfile/javaClasses.hpp"
#include "memory/metaspaceClosure.hpp"
#include "oops/methodData.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/resourceHash.hpp"

class InstanceKlass;
class Method;
class Symbol;

class MethodDataKey {
  Method* _holder;
public:
  MethodDataKey(Method* holder) : _holder(holder) {}

  void metaspace_pointers_do(MetaspaceClosure* it) {
    it->push(&_holder);
  }

  bool equals(MethodDataKey const& other) const {
    return _holder == other._holder;
  }

  void mark_pointers();
  unsigned int hash() const;

  static unsigned int dumptime_hash(Symbol* sym)  {
    if (sym == nullptr) {
      // _invoked_name maybe null
      return 0;
    }
    return java_lang_String::hash_code((const jbyte*)sym->bytes(), sym->utf8_length());
  }

  unsigned int dumptime_hash() const {
    return dumptime_hash(_holder->name()) +
           dumptime_hash(_holder->signature());
  }

  static inline unsigned int DUMPTIME_HASH(MethodDataKey const& key) {
    return (key.dumptime_hash());
  }

  static inline bool DUMPTIME_EQUALS(
      MethodDataKey const& k1, MethodDataKey const& k2) {
    return (k1.equals(k2));
  }

  void init_for_archive(MethodDataKey& dumptime_key);
  Method* method() const { return _holder; }
};

class DumpTimeMethodDataInfo {
  MethodData*     _method_data;
  MethodCounters* _method_counters;
public:
  DumpTimeMethodDataInfo(MethodData* method_data, MethodCounters* counters)
  : _method_data(method_data), _method_counters(counters) {}

  void metaspace_pointers_do(MetaspaceClosure* it) {
    it->push(&_method_data);
    it->push(&_method_counters);
  }

  MethodData* method_data() {
     return _method_data;
  }
  MethodCounters* method_counters() {
    return _method_counters;
  }
};

class RunTimeMethodDataInfo {
  MethodDataKey _key;
  MethodData* _method_data;
  MethodCounters* _method_counters;
public:
  RunTimeMethodDataInfo(MethodDataKey key, MethodData* method_data, MethodCounters* counters) :
      _key(key), _method_data(method_data), _method_counters(counters) {}

  // Used by MethodDataDictionary to implement OffsetCompactHashtable::EQUALS
  static inline bool EQUALS(
      const RunTimeMethodDataInfo* value, MethodDataKey* key, int len_unused) {
    return (value->_key.equals(*key));
  }
  void init(MethodDataKey& key, DumpTimeMethodDataInfo& info) {
    _key.init_for_archive(key);
    ArchiveBuilder::current()->write_pointer_in_buffer(&_method_data, info.method_data());
    ArchiveBuilder::current()->write_pointer_in_buffer(&_method_counters, info.method_counters());
  }

  unsigned int hash() const {
    return _key.hash();
  }
  MethodDataKey key() const {
    return _key;
  }

  Method*         method()          const { return _key.method();   }
  MethodData*     method_data()     const { return _method_data;     }
  MethodCounters* method_counters() const { return _method_counters; }
};

class DumpTimeMethodInfoDictionary
    : public ResourceHashtable<MethodDataKey,
        DumpTimeMethodDataInfo,
        137, // prime number
        AnyObj::C_HEAP,
        mtClassShared,
        MethodDataKey::DUMPTIME_HASH,
        MethodDataKey::DUMPTIME_EQUALS> {
public:
  DumpTimeMethodInfoDictionary() : _count(0) {}
  int _count;
};

class MethodDataInfoDictionary : public OffsetCompactHashtable<
    MethodDataKey*,
    const RunTimeMethodDataInfo*,
    RunTimeMethodDataInfo::EQUALS> {};

#endif // SHARED_CDS_METHODDATAINFO_HPP
