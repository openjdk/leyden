# Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

source ../lib/bench-lib.sh

# Example of comparing two leyden builds
#
# $ bash run.sh /bld/leyden/new/bin/java /bld/leyden/old/bin/java
# ===report.csv================================================
# Run,1_xon,1_td,1_aot,2_xon,2_td,2_aot
# 1,118.68000,95.003000,91.050000,118.69000,99.08000,94.664000
# 2,117.96000,95.262000,91.223000,120.73000,96.863000,96.67000
# 3,118.43000,96.11000,90.455000,121.16000,98.138000,94.71000
# 4,115.742000,98.342000,91.075000,120.956000,99.306000,93.407000
# 5,118.60000,98.172000,90.635000,121.783000,100.952000,95.33000
# 6,119.958000,99.836000,90.052000,122.60000,101.492000,94.346000
# 7,118.391000,99.28000,91.158000,121.96000,101.476000,93.113000
# 8,121.36000,100.026000,90.705000,124.63000,101.697000,94.93000
# 9,119.84000,100.23000,91.469000,122.93000,102.776000,96.37000
# 10,121.15000,101.35000,92.78000,124.60000,103.405000,95.88000
# ==============================jvm1 /bld/leyden/new/bin/java
# [1_xon ] Premain Prototype (CDS )                      119.00 ms
# [1_td  ] Premain Prototype (CDS + Training Data)        98.34 ms
# [1_aot ] Premain Prototype (CDS + Training Data + AOT)  91.06 ms
# ==============================jvm2 /bld/leyden/old/bin/java
# [2_xon ] Premain Prototype (CDS )                      121.99 ms
# [2_td  ] Premain Prototype (CDS + Training Data)       100.50 ms
# [2_aot ] Premain Prototype (CDS + Training Data + AOT)  94.94 ms
