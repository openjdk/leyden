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

#ifndef SHARE_COMPILER_RECOMPILATIONPOLICY_HPP
#define SHARE_COMPILER_RECOMPILATIONPOLICY_HPP

#include "compiler/compileBroker.hpp"
#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

namespace CompilationPolicyUtils {
template<int SAMPLE_COUNT = 256>
class WeightedMovingAverage {
  int _current;
  int _samples[SAMPLE_COUNT];
  int64_t _timestamps[SAMPLE_COUNT];

  void sample(int s, int64_t t) {
    assert(s >= 0, "Negative sample values are not supported");
    _samples[_current] = s;
    _timestamps[_current] = t;
    if (++_current >= SAMPLE_COUNT) {
      _current = 0;
    }
  }

  // Since sampling happens at irregular invervals the solution is to
  // discount the older samples proportionally to the time between
  // the now and the time of the sample.
  double value(int64_t t) const {
    double decay_speed = 1;
    double weighted_sum = 0;
    int count = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
      if (_samples[i] >= 0) {
        count++;
        double delta_t = (t - _timestamps[i]) / 1000.0; // in seconds
        if (delta_t < 1) delta_t = 1;
        weighted_sum += (double) _samples[i] / (delta_t * decay_speed);
      }
    }
    if (count > 0) {
      return weighted_sum / count;
    } else {
      return 0;
    }
  }
  static int64_t time() {
    return nanos_to_millis(os::javaTimeNanos());
  }
public:
  WeightedMovingAverage() : _current(0) {
    for (int i = 0; i < SAMPLE_COUNT; i++) {
      _samples[i] = -1;
    }
  }
  void sample(int s) { sample(s, time()); }
  double value() const { return value(time()); }
};
} // namespace CompilationPolicyUtils


class RecompilationPolicy : AllStatic {
  typedef CompilationPolicyUtils::WeightedMovingAverage<> LoadAverage;
  static LoadAverage _load_average;
  static volatile bool _recompilation_done;
public:
  static void sample_load_average();
  static void print_load_average(outputStream* st);
  static bool have_recompilation_work();
  static bool recompilation_step(int step, TRAPS);
};

#endif // SHARE_COMPILER_RECOMPILATIONPOLICY_HPP