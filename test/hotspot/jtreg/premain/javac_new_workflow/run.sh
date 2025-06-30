# Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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

# This is a new propsed workflow for the "terminal stage" of the Leyden
# condenser pipeline (see https://openjdk.org/projects/leyden/notes/03-toward-condensers)
#
# The CDS and AOT artifacts are generated in a single step.
#
# Please run this script and look at the commands in the generated cmds.txt file.

# Usage:
#
# bash run.sh $JAVA
# bash run.sh $JAVA args ....
#
# e.g.
#
# bash.sh /jdk3/bld/le4/images/jdk/bin/java
# bash.sh /jdk3/bld/le4/images/jdk/bin/java -XX:+UnlockDiagnosticVMOptions -XX:-AOTInvokeDynamicLinking

# These options are enabled by default for training run. You can either disable them in the command-line
# as shown above, or edit the following line
#
# The training run uses two JVM processes. We add "pid" to the log to distinguish their output.
TRAINING_OPTS="${TRAINING_OPTS} -Xlog:aot+codecache -Xlog:cds=debug::uptime,tags,pid"
TRAINING_OPTS="${TRAINING_OPTS} -XX:+AOTClassLinking"
#TRAINING_OPTS="${TRAINING_OPTS} -XX:+UnlockDiagnosticVMOptions -XX:+AOTRecordTraining"

# These options are enabled by default for training run.
#PRODUCTION_OPTS="${PRODUCTION_OPTS} -XX:+UnlockDiagnosticVMOptions -XX:+AOTReplayTraining"


launcher=$1
shift

rm -f cmds.txt
#========================================
# One-step training run
#   Remove javac.cds, so that it will be regenerated
rm -f javac.cds*
cmd="$launcher -XX:CacheDataStore=javac.cds $TRAINING_OPTS "$@" com.sun.tools.javac.Main HelloWorld.java"

echo "# commands for training run" >> cmds.txt
echo "rm -f javac.cds*" >> cmds.txt
echo $cmd >> cmds.txt
echo >> cmds.txt
eval $cmd

#========================================
# Production run
#    TODO: add flags for AOT cache
cmd="$launcher $PRODUCTION_OPTS -Xlog:cds -Xlog:aot+codecache -XX:CacheDataStore=javac.cds $@ com.sun.tools.javac.Main HelloWorld.java"
perfcmd="perf stat -r 20 $(echo $cmd | sed -e 's/ [-]Xlog:[^ ]*//g')"
echo "Production run: "
echo "   $cmd"
echo "   "

rm -f HelloWorld.class
eval "$cmd"

echo "# command for production run" >> cmds.txt
echo $cmd >> cmds.txt
echo >> cmds.txt

echo "# commands for benchmarking" >> cmds.txt
echo perf stat -r 20 $launcher $@ com.sun.tools.javac.Main HelloWorld.java >> cmds.txt
echo $perfcmd >> cmds.txt
echo >> cmds.txt

echo "cat cmds.txt"
cat cmds.txt

echo ========================================
echo "Verifying"
if (set -x; $launcher -Xshare:off -cp . HelloWorld); then
    echo javac seems to be working correctly with $PRODUCTION_OPTS -XX:CacheDataStore=javac.cds
else
    echo "Failed?????????????????"
    exit 1
fi

