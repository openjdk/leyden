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

cd ~/tmp/
mkdir -p pet-clinic-bench
cd pet-clinic-bench

function geomean () {
    (for i in $*; do echo $i; done) | \
        awk 'BEGIN{E = exp(1);} $1>0{tot+=log($1); c++} END{m=tot/c; printf "%.2f\n", E^m}' -
}

function stdev () {
    (for i in $*; do echo $i; done) | \
        awk '{for(i=1;i<=NF;i++) {sum[i] += $i; sumsq[i] += ($i)^2}}
          END {for (i=1;i<=NF;i++) {
          printf "%.2f\n", sqrt((sumsq[i]-sum[i]^2/NR)/NR)}
         }' -
}

function count () {
    echo $* | wc -w
}

# This tests overall improvements
echo "Final run: {static + dynamic} archive, +AOTReplayTraining and +LoadCachedCode"
for i in old new; do
    nums=$(grep Booted data/time.$i.* | sed -e 's/.*in //g' -e 's/ms//g')
    nums="$nums $(for j in data/make.$i.*; do grep Booted $j | tail -1 | sed -e 's/.*in //g' -e 's/ms//g'; done)"
    printf "%s %8.2f ms %8.2f ms (%d samples)\n" $i $(geomean $nums) $(stdev $nums) $(count $nums)
done

# This tests improvements in CDS only.
echo ""
echo "Static archive only"
for i in old new; do
    nums=$(for j in data/make.$i.*; do grep Booted $j | head -2 | tail -1 | sed -e 's/.*in //g' -e 's/ms//g'; done)
    printf "%s %8.2f ms %8.2f ms (%d samples)\n" $i $(geomean $nums) $(stdev $nums) $(count $nums)
done

