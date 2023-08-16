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

# javac_helloworld.sh
#
# Usage:
#
# bash javac_helloworld.sh $MAINLINE_JAVA $PREMAIN_JAVA
#
# $MAINLINE_JAVA - the java executable of a product build from the JDK mainline repo
# $PREMAIN_JAVA  - the java executable of a product build from the leyden-premain branch

MAINLINE_JAVA=$1; shift 
PREMAIN_JAVA=$1; shift 

APP=Javac

if test "$SKIP_SETUP" = "" || test ! -f Javac-mainline.jsa; then
    echo "Set up: create JSA and AOT caches ..."

    #----------------------------------------------------------------------
    # MAINLINE_JAVA
    # Trial run and create CDS archive

    $MAINLINE_JAVA -Xshare:off -XX:DumpLoadedClassList=$APP-mainline.classlist com.sun.tools.javac.Main HelloWorld.java
    $MAINLINE_JAVA -Xshare:dump -XX:SharedArchiveFile=$APP-mainline.jsa -XX:SharedClassListFile=$APP-mainline.classlist \
                   -Xlog:cds=debug,cds+class=debug:file=$APP-mainline-static.dump.log::filesize=0

    #----------------------------------------------------------------------
    # PREMAIN_JAVA
    # Run the "5 steps" to generate CDS archive, training data and AOT code cache
    #
    # FIXME: for AOT, we should do a longer and compile more source files, so we can have more optimized AOT code

    JAVA=$PREMAIN_JAVA
    DUMP_EXTRA_ARGS=-XX:+ArchiveInvokeDynamic 
    CMDLINE="-XX:+ArchiveInvokeDynamic com.sun.tools.javac.Main HelloWorld.java"

    source ../lib/premain-run.sh
fi


# The number of the outer loop
if test "$RUNS" = ""; then
    RUNS=10
fi

# The number passed to "perf stat -r"
if test "$REPEAT" = ""; then
    REPEAT=16
fi

function get_elapsed () {
    elapsed=$(bc <<< "scale=3; $(cat $1 | grep elapsed | sed -e 's/[+].*//') * 1000")
    echo $elapsed >> $1.elapsed
    echo $elapsed
}

rm -rf logs
mkdir -p logs
rm -f report.csv

function report () {
    echo $@
    echo $@ >> report.csv
}

function geomean () {
    printf "%6.2f ms" $(awk 'BEGIN{E = exp(1);} $1>0{tot+=log($1); c++} END{m=tot/c; printf "%.2f\n", E^m}' $1)
}

report "mainline_xoff,mainline_xon,premain_xon,premain_aot"

for i in $(seq 1 $RUNS); do
    echo RUN $i

    (set -x;
     perf stat -r $REPEAT $MAINLINE_JAVA -Xshare:off com.sun.tools.javac.Main HelloWorld.java 2> logs/mainline_xoff.$i
    )
    mainline_xoff=$(get_elapsed logs/mainline_xoff.$i)

    (set -x;
     perf stat -r $REPEAT $MAINLINE_JAVA -XX:SharedArchiveFile=$APP-mainline.jsa com.sun.tools.javac.Main HelloWorld.java 2> logs/mainline_xon.$i
    )
    mainline_xon=$(get_elapsed logs/mainline_xon.$i)

    (set -x;
     perf stat -r $REPEAT $PREMAIN_JAVA -XX:SharedArchiveFile=$APP-static.jsa com.sun.tools.javac.Main HelloWorld.java 2> logs/premain_xon.$i
    )
    premain_xon=$(get_elapsed logs/premain_xon.$i)

    (set -x;
     perf stat -r $REPEAT $PREMAIN_JAVA -XX:SharedArchiveFile=$APP-dynamic.jsa -XX:+ReplayTraining -XX:+LoadSharedCode \
        -XX:SharedCodeArchive=$APP-dynamic.jsa-sc -XX:ReservedSharedCodeSize=1000M -Xlog:sca=error \
        com.sun.tools.javac.Main HelloWorld.java 2> logs/premain_aot.$i
    )
    premain_aot=$(get_elapsed logs/premain_aot.$i)
    
    report $mainline_xoff,$mainline_xon,$premain_xon,$premain_aot
done

echo ===report.csv================================================
cat report.csv
echo ===geomean===================================================
echo "Wall clock time - geomean over $RUNS runs of 'perf stat -r $REPEAT javac HelloWorld.java'"
echo "Mainline JDK (CDS disabled)     $(geomean logs/mainline_xoff.*.elapsed)"
echo "Mainline JDK (CDS enabled)      $(geomean logs/mainline_xon.*.elapsed)"
echo "Premain Prototype (CDS only)    $(geomean logs/premain_xon.*.elapsed)"
echo "Premain Prototype (CDS + AOT)   $(geomean logs/premain_aot.*.elapsed)"
echo =============================================================

exit

FYI: Results I got on 2023/08/15, Intel(R) Core(TM) i7-10700 CPU @ 2.90GHz, 32MB RAM
With JDK mainline repo and leyden-premain branch that are pretty up to date


===report.csv================================================
mainline_xoff,mainline_xon,premain_xon,premain_aot
306.33000,164.83000,135.191000,99.70000
305.81000,163.295000,139.99000,99.57000
308.45000,166.204000,139.664000,100.46000
309.64000,167.040000,137.19000,98.88000
309.79000,169.989000,139.449000,98.32000
310.65000,173.60000,139.232000,100.34000
309.01000,168.14000,137.82000,98.51000
308.73000,168.168000,139.110000,97.74000
310.29000,169.21000,139.68000,97.56000
311.09000,169.80000,140.121000,99.44000
===geomean===================================================
Wall clock time - geomean over 10 runs of 'perf stat -r 20 javac HelloWorld.java'
Mainline JDK (CDS disabled)     311.09 ms
Mainline JDK (CDS enabled)      169.80 ms
Premain Prototype (CDS only)    140.12 ms
Premain Prototype (CDS + AOT)    99.44 ms
=============================================================
