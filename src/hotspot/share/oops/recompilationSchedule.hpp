/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_RECOMPILATION_SCHEDULE_HPP
#define SHARE_OOPS_RECOMPILATION_SCHEDULE_HPP

#include "cds/serializeClosure.hpp"
#include "memory/allStatic.hpp"
#include "memory/metaspaceClosure.hpp"
#include "oops/array.hpp"
#include "oops/trainingData.hpp"
#include "runtime/atomic.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

class RecompilationSchedule : public AllStatic {
  static Array<MethodTrainingData*>* _schedule;
  static Array<MethodTrainingData*>* _schedule_for_dumping;
  static volatile bool* _status;
public:
  static void initialize();
  static void prepare(TRAPS);
  static bool have_schedule() { return _schedule != nullptr; }
  static Array<MethodTrainingData*>* schedule() { return _schedule; }
  static int length() {
    return have_schedule() ? _schedule->length() : 0;
  }
  static MethodTrainingData* at(int i) {
    assert(i < length(), "");
    return schedule()->at(i);
  }
  static volatile bool* status() { return _status; }
  static volatile bool* status_adr_at(int i) {
    assert(i < length(), "");
    return &_status[i];
  }
  static bool status_at(int i) {
    return Atomic::load_acquire(status_adr_at(i));
  }
  static void set_status_at(int i, bool value) {
    Atomic::release_store(RecompilationSchedule::status_adr_at(i), value);
  }
  static bool claim_at(int i) {
    return Atomic::cmpxchg(RecompilationSchedule::status_adr_at(i), false, true) == false;
  }
#if INCLUDE_CDS
  static void iterate_roots(MetaspaceClosure* it);
  static void cleanup();
  static void serialize(SerializeClosure* soc);
  static void print_archived_training_data_on(outputStream* st);
#endif
};

#endif // SHARE_OOPS_RECOMPILATION_SCHEDULE_HPP