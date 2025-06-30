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

# Compare the performance of petclinic with two JDK builds
# - Do the training run 10 times.
# - After each training run, run petclinic 10 times
#
# To compare the results, run result.sh

#------------------------------ configuration --------------------
OLD_DIR=/jdk3/le4/open/test/hotspot/jtreg/premain/spring-petclinic
NEW_DIR=/jdk3/le3/open/test/hotspot/jtreg/premain/spring-petclinic

OLD_HOME=/jdk3/bld/le4/images/jdk
NEW_HOME=/jdk3/bld/le3/images/jdk

OLD_JVM=$OLD_HOME/bin/java
NEW_JVM=$NEW_HOME/bin/java
#------------------------------ configuration end-----------------

cd ~/tmp/
mkdir -p pet-clinic-bench
cd pet-clinic-bench
mkdir -p data

CMDLINE="-XX:SharedArchiveFile=spring-petclinic.dynamic.jsa -XX:+UnlockDiagnosticVMOptions -XX:+AOTReplayTraining -XX:+LoadCachedCode"
CMDLINE="$CMDLINE -XX:CachedCodeFile=spring-petclinic.code.jsa -Xlog:init -Xlog:aot+codecache=error -Xmx2g"
CMDLINE="$CMDLINE -cp @petclinic-snapshot/target/unpacked/classpath -DautoQuit=true"
CMDLINE="$CMDLINE -Dspring.aot.enabled=true org.springframework.samples.petclinic.PetClinicApplication"

(
set -x

for i in {1..10}; do
    echo "make old $i"
    (cd $OLD_DIR; make clean0; make PREMAIN_HOME=$OLD_HOME run 2>&1) > data/make.old.$i
    grep "Booted and returned" data/make.old.$i

    echo "make new $i"
    (cd $NEW_DIR; make clean0; make PREMAIN_HOME=$NEW_HOME run 2>&1) > data/make.new.$i
    grep "Booted and returned" data/make.new.$i

    rm -f data/time.old.$i
    rm -f data/time.new.$i

    for j in {1..10}; do
        sync; echo 'after 100' | tclsh
        (cd $OLD_DIR; $OLD_JVM $CMDLINE 2>&1) > tmp.txt
        cat tmp.txt >> data/time.old.$i
        echo $i $j old $(grep "Booted and returned" tmp.txt)

        sync; echo 'after 100' | tclsh
        (cd $NEW_DIR; $NEW_JVM $CMDLINE 2>&1) > tmp.txt
        cat tmp.txt >> data/time.new.$i
        echo $i $j new $(grep "Booted and returned" tmp.txt)
    done
done
) 2>&1 | tee log.txt
