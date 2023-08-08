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

# run JMH with premain prototype
#
# run all tests
#      bash run.sh $JAVA
# run a single test
#      bash run.sh $JAVA 2
# clean up
#      make clean

JAVA=$1; shift 
JAR=specjbb2005-jmh-1.25.jar
CMDLINE="-jar $JAR Jbb -t 1 -f 0 -r 1 -i 2 -wi 0"
APP=specjbb2005-jmh

ONLYTEST=$1

TEST_NUM=0

function do_test () {
    TEST_NUM=$(expr $TEST_NUM + 1)

    echo ========================================
    echo Test $TEST_NUM $1

    shift
    if test "$ONLYTEST" != "" -a "$ONLYTEST" != "$TEST_NUM"; then
        echo "$@"
        echo "SKIPPED"
        return 0
    else
        local before_errfiles=$(ls hs_err*  2> /dev/null | sort)
        echo "$@"
        if eval "$@"; then
            true
        else
            echo "FAILED; command line was:"
            echo "$@";
            exit 1
        fi
        local after_errfiles=$(ls hs_err*  2> /dev/null | sort)
        if test "$before_errfiles" != "$after_errfiles"; then
            echo "HotSpot crashed (check hs_err errfiles); command line was:"
            echo "$@"
            exit 1
        fi
        echo "PASSED $TEST_NUM"
        if test "$ONLYTEST" = "$TEST_NUM"; then
            echo "Exiting after test $TEST_NUM has passed. Further tests may have been skipped"
            exit 0
        fi
    fi
}

do_test "(STEP 1 of 5) Dump classlist" \
        $JAVA -Xshare:off -XX:DumpLoadedClassList=$APP.classlist $CMDLINE

do_test "(STEP 2 of 5) Create Static $APP-static.jsa" \
    $JAVA -Xshare:dump -XX:SharedArchiveFile=$APP-static.jsa -XX:SharedClassListFile=$APP.classlist -cp $JAR \
    -Xlog:cds=debug,cds+class=debug,cds+resolve=debug:file=$APP-static.dump.log

if false; then
    do_test "Run with $APP-static.jsa" \
            $JAVA -XX:SharedArchiveFile=$APP-static.jsa $CMDLINE
fi

do_test "(STEP 3 of 5) Run with $APP-static.jsa and dump profile in $APP-dynamic.jsa (With Training Data Replay)" \
    $JAVA -XX:SharedArchiveFile=$APP-static.jsa -XX:ArchiveClassesAtExit=$APP-dynamic.jsa -XX:+RecordTraining \
        -Xlog:cds=debug,cds+class=debug:file=$APP-dynamic.dump.log \
        $CMDLINE

do_test "(STEP 4 of 5) Run with $APP-dynamic.jsa and generate AOT code" \
    $JAVA -XX:SharedArchiveFile=$APP-dynamic.jsa -XX:+ReplayTraining -XX:+StoreSharedCode \
        -Xlog:sca*=trace:file=$APP-store-sc.log \
        -XX:SharedCodeArchive=$APP-dynamic.jsa-sc -XX:ReservedSharedCodeSize=100M $CMDLINE

do_test "(STEP 5 of 5) Final production run: with $APP-dynamic.jsa and load AOT code" \
    $JAVA -XX:SharedArchiveFile=$APP-dynamic.jsa -XX:+ReplayTraining -XX:+LoadSharedCode \
        -Xlog:sca*=trace:file=$APP-load-sc.log \
        -XX:SharedCodeArchive=$APP-dynamic.jsa-sc -XX:ReservedSharedCodeSize=100M $CMDLINE
