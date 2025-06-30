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

#======================================================================
# Overview
#
# This file provides the framework for running start-up benchmarks that have very
# short elapsed time.
#
# The goal is to be able measure very minor improvements (under 1%) even when there
# are system noises.
#
# - Measure the entire elapsed time with 'perf stat -r 16 bin/java .....'
# - Interleave the execution of the test JVMs. This way, the effect of system noises 
#   (CPU overheating, throttling, background tasks) is likely evenly distributed across
#   the test JVMs.
# - Save the results in a CSV file so you can load it in a spreadsheet and manually
#   check for noisy samples.
#
# NOTE: you can see the effect of CPU throttling in the sample data below. See the
# 2_xon column, which goes from 123ms when the CPU is cold to 128ms when the CPU is hot.

#======================================================================
# Config
#
# See ../javac_helloworld/run.sh for an example of using this file
#
# Before sourcing this file
# The caller script should set up the following variables

# required
#     CMDLINE
#     APP
# optional
#     HEAP_SIZE
#     RUNS
#     REPEAT

# The number of the outer loop
if test "$RUNS" = ""; then
    RUNS=10
fi

# The number passed to "perf stat -r"
if test "$REPEAT" = ""; then
    REPEAT=16
fi

#======================================================================
# Example
#
# FYI: Results I got on 2023/12/21, Intel(R) Core(TM) i7-10700 CPU @ 2.90GHz, 32MB RAM
# With JDK mainline repo and leyden-premain branch that are pretty up to date.
#
# $ cd test/hotspot/jtreg/premain/javac_helloworld
# $ bash run.sh /bld/mainline/images/jdk/bin/java /bld/leyden/images/jdk/bin/java
# 
# ===report.csv================================================
# Run,1_xoff,1_xon,2_xon,2_td,2_aot
# 1,313.86000,164.25000,123.031000,96.849000,92.38000
# 2,324.59000,163.745000,122.550000,97.730000,92.45000
# 3,313.61000,164.222000,122.91000,100.59000,91.18000
# 4,316.58000,163.779000,124.673000,101.085000,95.10000
# 5,313.002000,162.854000,126.240000,102.423000,91.81000
# 6,314.97000,166.020000,126.148000,101.159000,91.43000
# 7,313.30000,168.23000,128.19000,101.956000,88.13000
# 8,314.67000,169.04000,126.168000,102.220000,91.90000
# 9,313.26000,167.864000,125.856000,102.57000,90.75000
# 10,318.14000,169.075000,128.055000,103.523000,93.61000
# ==============================jvm1 /bld/mainline/images/jdk/bin/java
# [1_xoff] Mainline JDK (CDS disabled)                   315.58 ms
# [1_xon ] Mainline JDK (CDS enabled)                    165.89 ms
# ==============================jvm2 /bld/leyden/images/jdk/bin/java
# [2_xon ] Premain Prototype (CDS )                      125.37 ms
# [2_td  ] Premain Prototype (CDS + Training Data)       100.99 ms
# [2_aot ] Premain Prototype (CDS + Training Data + AOT)  91.86 ms
# 
#======================================================================


# "$@" is the command-line specified by the user. Each element specifies a 
# JVM (which optionally can contain VM parameters).
#
# We automatically detect whether the JVM is from the mainline, or from the leyden
# repo.
function main () {
    do_dump "$@"
    do_exec "$@"
    do_summary "$@"
}

function do_dump () {
    if test "$NODUMP" != ""; then
        return
    fi

    local n=0
    for i in "$@"; do
        n=$(expr $n + 1)
        JAVA="$i"
        if test "$HEAP_SIZE" != ""; then
            JAVA="$JAVA $HEAP_SIZE"
        fi
        dump_one_jvm $n
    done
}

function do_exec () {
    if test "$NOEXEC" != ""; then
        return
    fi

    rm -rf logs
    mkdir -p logs
    rm -f report.csv

    REPORT_HEADER='Run'

    local n=0
    for i in "$@"; do
        n=$(expr $n + 1)
        JAVA="$i"
        if test "$HEAP_SIZE" != ""; then
            JAVA="$JAVA $HEAP_SIZE"
        fi
        local APPID=$APP-jvm$n
        if test -f $APPID-mainline.classlist; then
            REPORT_HEADER=${REPORT_HEADER},${n}_xoff
            REPORT_HEADER=${REPORT_HEADER},${n}_xon
        else
            REPORT_HEADER=${REPORT_HEADER},${n}_xon
            REPORT_HEADER=${REPORT_HEADER},${n}_td
            REPORT_HEADER=${REPORT_HEADER},${n}_aot
        fi
        #report jvm$n = $JAVA
    done

    report $REPORT_HEADER

    # The #0 run is to warm up the CPU and is not counted
    for r in $(seq 0 $RUNS); do
        if test $r = 0; then
            echo "RUN $r (warmup - discarded)"
        else
            echo "RUN $r of $RUNS"
        fi
        RUNLOG=$r
        local n=0
        for i in "$@"; do
            n=$(expr $n + 1)
            JAVA="$i"
            if test "$HEAP_SIZE" != ""; then
                JAVA="$JAVA $HEAP_SIZE"
            fi
            echo === jvm$n
            exec_one_jvm $n $r
        done

        if test $r -gt 0; then
            report $RUNLOG
        else
            echo $RUNLOG
        fi
    done
}

function dump_one_jvm () {
    local binjava=$(echo $JAVA | sed -e 's/ .*//g')
    local APPID=$APP-jvm$1

    if $JAVA --version > /dev/null; then
        if $JAVA -XX:+PrintFlagsFinal --version | grep CDSManualFinalImage > /dev/null; then
            local is_premain=1
            local type_msg="(premain 5 step)"
        else
            local is_premain=0
            local type_msg="(mainline      )"
        fi
    else
        echo "$JAVA doesn't seem to be a real JVM"
        exit 1
    fi

    echo ========================================
    echo dumping archive "$type_msg" for $JAVA
    echo ========================================

    if test $is_premain = 0; then
        echo "(Mainline) Dump classlist"
        (set -x; $JAVA -Xshare:off -XX:DumpLoadedClassList=$APPID-mainline.classlist $CMDLINE) || exit 1

        echo "(Mainline) Dump CDS archive"
        rm -f $APPID-mainline-static.dump.log
        (set -x; $JAVA -Xshare:dump -XX:SharedArchiveFile=$APPID-mainline.jsa -XX:SharedClassListFile=$APPID-mainline.classlist \
                       -Xlog:cds=debug,cds+class=debug:file=$APPID-mainline-static.dump.log::filesize=0 $CMDLINE) || exit 1

    else
        # FIXME -- add support for new workflow
        LEYDEN_OPTS="-XX:+AOTClassLinking"

        echo "(Premain STEP 1 of 5) Dump classlist"
        (set -x; $JAVA -Xshare:off -XX:DumpLoadedClassList=$APPID.classlist $CMDLINE) || exit 1

        echo "(Premain STEP 2 of 5) Create Static $APPID-static.jsa"
        rm -f $APPID-static.dump.log
        (set -x; $JAVA -Xshare:dump $LEYDEN_OPTS -XX:SharedArchiveFile=$APPID-static.jsa -XX:SharedClassListFile=$APPID.classlist \
                       -Xlog:cds=debug,cds+class=debug,cds+resolve=debug:file=$APPID-static.dump.log::filesize=0 $CMDLINE) || exit 1


        echo "(Premain STEP 3 of 5) Run with $APPID-static.jsa and dump profile in $APPID-dynamic.jsa (With Training Data Replay)"
        rm -f $APPID-dynamic.dump.log
        (set -x; $JAVA -XX:SharedArchiveFile=$APPID-static.jsa -XX:ArchiveClassesAtExit=$APPID-dynamic.jsa -XX:+UnlockDiagnosticVMOptions -XX:+AOTRecordTraining \
                       -Xlog:cds=debug,cds+class=debug:file=$APPID-dynamic.dump.log::filesize=0 $CMDLINE) || exit 1

        echo "(Premain  4 of 5) Run with $APPID-dynamic.jsa and generate AOT code"
        rm -f $APPID-store-sc.log
        (set -x; $JAVA -XX:SharedArchiveFile=$APPID-dynamic.jsa -XX:+UnlockDiagnosticVMOptions -XX:+AOTReplayTraining -XX:+StoreCachedCode \
                       -Xlog:aot+codecache*=warning:file=$APPID-store-sc.log::filesize=0 \
                       -XX:CachedCodeFile=$APPID-dynamic.jsa-sc -XX:CachedCodeMaxSize=100M $CMDLINE) || exit 1

    fi
}

function exec_one_jvm () {
    local APPID=$APP-jvm$1

    if test -f $APPID-mainline.classlist; then
        (set -x;
         perf stat -r $REPEAT $JAVA -Xshare:off $CMDLINE 2> logs/${1}_xoff.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_xoff.$2)

        (set -x;
         perf stat -r $REPEAT $JAVA -XX:SharedArchiveFile=$APPID-mainline.jsa $CMDLINE 2> logs/${1}_xon.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_xon.$2)
    fi

    if test -f $APPID.classlist; then
        (set -x;
         perf stat -r $REPEAT $JAVA -XX:SharedArchiveFile=$APPID-static.jsa $CMDLINE 2> logs/${1}_xon.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_xon.$2)
        (set -x;
         perf stat -r $REPEAT $JAVA -XX:SharedArchiveFile=$APPID-dynamic.jsa -XX:+UnlockDiagnosticVMOptions -XX:+AOTReplayTraining \
              $CMDLINE 2> logs/${1}_td.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_td.$2)
        (set -x;
         perf stat -r $REPEAT $JAVA -XX:SharedArchiveFile=$APPID-dynamic.jsa -XX:+UnlockDiagnosticVMOptions -XX:+AOTReplayTraining -XX:+LoadCachedCode \
              -XX:CachedCodeFile=$APPID-dynamic.jsa-sc -Xlog:aot+codecache=error \
              $CMDLINE 2> logs/${1}_aot.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_aot.$2)
    fi
}

function get_elapsed () {
    elapsed=$(bc <<< "scale=3; $(cat $1 | grep elapsed | sed -e 's/[+].*//') * 1000")
    echo $elapsed >> $1.elapsed
    echo $elapsed
}


function report () {
    echo "$@"
    echo "$@" >> report.csv
}

function geomean () {
    printf "%6.2f ms" $(awk 'BEGIN{E = exp(1);} $1>0{tot+=log($1); c++} END{m=tot/c; printf "%.2f\n", E^m}' $*)
}



function do_summary () {
    echo ===report.csv================================================
    cat report.csv

    mkdir -p logs/warmup
    mv logs/*.0.elapsed logs/*.0 logs/warmup
    local n=0
    for i in "$@"; do
        n=$(expr $n + 1)
        local APPID=$APP-jvm$n
        report ==============================jvm$n $i
        if test -f $APPID-mainline.classlist; then
            report "[$(printf %-6s ${n}_xoff)] Mainline JDK (CDS disabled)                   $(geomean logs/${n}_xoff.*.elapsed)"
            report "[$(printf %-6s ${n}_xon )] Mainline JDK (CDS enabled)                    $(geomean logs/${n}_xon.*.elapsed)"
        else
            report "[$(printf %-6s ${n}_xon )] Premain Prototype (CDS )                      $(geomean logs/${n}_xon.*.elapsed)"
            report "[$(printf %-6s ${n}_td  )] Premain Prototype (CDS + Training Data)       $(geomean logs/${n}_td.*.elapsed)"
            report "[$(printf %-6s ${n}_aot )] Premain Prototype (CDS + Training Data + AOT) $(geomean logs/${n}_aot.*.elapsed)"
        fi
    done
}

main "$@"
