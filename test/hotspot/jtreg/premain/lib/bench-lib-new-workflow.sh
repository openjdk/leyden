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
# See ../javac_new_workflow/run_bench.sh for an example of using this file
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
#
#     LEYDEN_CALLER_OPTS
#       extra args to be added to Leyden JVM runs
#       e.g. LEYDEN_CALLER_OPTS='-Xlog:aot+codecache=error'
#     TASKSET
#       allows pinning of perf runs to specific processors
#       e.g. setting TASKSET='taskest 0xff0' will execute
#       benchmark runs as
#         "taskest 0xff0 perf stat -r $REPEAT $JAVA ... $APP $CMDLINE"

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
# FYI: Results I got on 2024/05/14, AArch64
# With JDK mainline repo and leyden-premain branch that are pretty up to date.
#
# $ cd test/hotspot/jtreg/premain/javac_helloworld
# $ bash run.sh .../mainline/images/jdk/bin/java .../leyden/images/jdk/bin/java
# 
# ===report.csv================================================
# Run,1_xoff,1_xon,2_xoff,2_xon,2_td,2_aot
# 1,245.95000,165.35000,253.3000,135.85000,114.08000,80.13000
# 2,255.98000,172.27000,249.61000,131.03000,111.25000,92.39000
# 3,244.63000,149.85000,247.29000,138.09000,113.11000,94.04000
# 4,252.84000,150.57000,247.35000,144.17000,110.42000,93.87000
# 5,247.72000,156.89000,249.38000,149.35000,111.93000,94.74000
# 6,259.24000,167.54000,239.00000,147.69000,109.98000,84.89000
# 7,241.83000,150.16000,248.35000,154.1000,110.58000,91.57000
# 8,247.79000,143.51000,248.72000,144.88000,108.20000,78.43000
# 9,254.68000,155.95000,253.50000,134.91000,114.17000,93.27000
# 10,250.66000,166.34000,260.68000,142.91000,117.24000,87.14000
# ==============================jvm1 /home/adinn/redhat/openjdk/jdkdev/image-jdk-master/jdk/bin/java
# [1_xoff] Mainline JDK (CDS disabled)                   250.08 ms
# [1_xon ] Mainline JDK (CDS enabled)                    157.58 ms
# ==============================jvm2 /home/adinn/redhat/openjdk/jdkdev/image-premain/jdk/bin/java
# [2_xoff] Premain JDK (CDS disabled)                    249.66 ms
# [2_xon ] Premain JDK (CDS enabled)                     142.13 ms
# [2_td  ] Premain Prototype (CDS + Training Data)        112.07 ms
# [2_aot ] Premain Prototype (CDS + Training Data + AOT)  88.86 ms
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
            REPORT_HEADER=${REPORT_HEADER},${n}_xoff
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
            local type_msg="(premain )"
        else
            local is_premain=0
            local type_msg="(mainline)"
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
        echo "(Premain) Dump classlist"
        (set -x; $JAVA -Xshare:off -XX:DumpLoadedClassList=$APPID-premain.classlist $CMDLINE) || exit 1

        echo "(Premain) Dump CDS archive"
        rm -f $APPID-premain-static.dump.log
        (set -x; $JAVA -Xshare:dump -XX:SharedArchiveFile=$APPID-premain.jsa -XX:SharedClassListFile=$APPID-premain.classlist \
                       -Xlog:cds=debug,cds+class=debug:file=$APPID-premain-static.dump.log::filesize=0 $CMDLINE) || exit 1


        LEYDEN_OPTS="$LEYDEN_CALLER_OPTS"

        echo "(Premain) Dump Leyden archive"
        (set -x; $JAVA -XX:CacheDataStore=$APPID.cds $LEYDEN_OPTS $CMDLINE) || exit 1

    fi
}

function exec_one_jvm () {
    local APPID=$APP-jvm$1

    if test -f $APPID-mainline.classlist; then
        (set -x;
         $TASKSET perf stat -r $REPEAT $JAVA -Xshare:off $CMDLINE 2> logs/${1}_xoff.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_xoff.$2)

        (set -x;
         $TASKSET perf stat -r $REPEAT $JAVA -XX:SharedArchiveFile=$APPID-mainline.jsa $CMDLINE 2> logs/${1}_xon.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_xon.$2)
    fi

    if test -f $APPID.cds; then
        LEYDEN_OPTS="$LEYDEN_CALLER_OPTS"
        (set -x;
         $TASKSET perf stat -r $REPEAT $JAVA -Xshare:off $CMDLINE 2> logs/${1}_xoff.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_xoff.$2)

        (set -x;
         $TASKSET perf stat -r $REPEAT $JAVA -XX:SharedArchiveFile=$APPID-premain.jsa $CMDLINE 2> logs/${1}_xon.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_xon.$2)
        (set -x;
         $TASKSET perf stat -r $REPEAT $JAVA -XX:CacheDataStore=$APPID.cds -XX:-LoadCachedCode $LEYDEN_OPTS  $CMDLINE 2> logs/${1}_td.$2
        )
        RUNLOG=$RUNLOG,$(get_elapsed logs/${1}_td.$2)
        (set -x;
         $TASKSET perf stat -r $REPEAT $JAVA -XX:CacheDataStore=$APPID.cds  -XX:+LoadCachedCode $LEYDEN_OPTS  $CMDLINE 2> logs/${1}_aot.$2
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
            report "[$(printf %-6s ${n}_xoff)] Premain JDK (CDS disabled)                    $(geomean logs/${n}_xoff.*.elapsed)"
            report "[$(printf %-6s ${n}_xon )] Premain JDK (CDS enabled)                     $(geomean logs/${n}_xon.*.elapsed)"
            report "[$(printf %-6s ${n}_td )] Premain Prototype (CDS + Training Data)        $(geomean logs/${n}_td.*.elapsed)"
            report "[$(printf %-6s ${n}_aot )] Premain Prototype (CDS + Training Data + AOT) $(geomean logs/${n}_aot.*.elapsed)"
        fi
    done
}

main "$@"
