# Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
# DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
#
# This code is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 only, as
# published by the Free Software Foundation.
#
# This code is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# version 2 for more details (a copy is included in the LICENSE file that
# accompanied this code).
#
# You should have received a copy of the GNU General Public License version
# 2 along with this work; if not, write to the Free Software Foundation,
# Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
# or visit www.oracle.com if you need additional information or have any
# questions.

# This script measure the performance of "javac HelloWorld.java"
#
# Specify one or more jvms:
#
#     bash run.sh $jvm1
#     bash run.sh $jvm1 $jvm2 ...
#
# It's possible to specify extra parameters for the jvms. E.g.
#
#     bash run.sh a/bin/java 'b/bin/java -Xint'  'c/bin/java -Xmixed'
#
# See ../lib/bench-lib.sh for sample timing data and more information.

APP=Javac
CMDLINE="com.sun.tools.javac.Main HelloWorld.java"

source ../lib/bench-lib-new-workflow.sh

# Example of comparing two leyden builds
#
# $ bash run_bench.sh /bld/leyden/new/bin/java /bld/leyden/old/bin/java
# ===report.csv================================================
# Run,1_xoff,1_xon,1_aot,2_xoff,2_xon,2_aot
# 1,251.04000,144.48000,93.34000,254.87000,150.73000,86.31000
# 2,241.86000,156.01000,94.01000,262.28000,147.82000,93.03000
# 3,248.77000,138.17000,84.51000,242.18000,132.86000,87.26000
# 4,249.83000,162.71000,94.93000,247.70000,152.60000,85.95000
# 5,249.90000,157.8000,88.98000,239.13000,152.79000,89.16000
# 6,250.33000,161.91000,84.07000,236.03000,156.12000,85.63000
# 7,244.62000,122.86000,75.37000,248.32000,142.14000,91.10000
# 8,255.41000,118.35000,83.61000,233.07000,130.12000,90.43000
# 9,244.33000,153.18000,81.47000,250.07000,150.15000,91.92000
# 10,247.32000,129.58000,79.58000,244.58000,154.66000,82.08000
# ==============================jvm1 /bld/leyden/new/bin/java
# [1_xoff] Premain JDK (CDS disabled)                   248.31 ms
# [1_xon ] Premain JDK (CDS enabled)                    143.63 ms
# [1_aot ] Premain Prototype (CDS + Training Data + AOT)  85.76 ms
# ==============================jvm2 /bld/leyden/old/bin/java
# [2_xoff] Premain JDK (CDS disabled)                   245.68 ms
# [2_xon ] Premain JDK (CDS enabled)                    146.74 ms
# [2_aot ] Premain Prototype (CDS + Training Data + AOT)  88.23 ms
