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

#======================================================================
# Regression test to run javac with premain prototype, to test the CDS and AOT changes.
#
# QUICK START:
#  bash javac-test.sh -n $JAVA
#
#    This shows you all the 5 steps for using the premain prototype:
#
#       (STEP 1 of 5) Dump javac.classlist (skipped)
#       (STEP 2 of 5) Create Static javac-static.jsa (skipped)
#       (STEP 3 of 4) Run with javac-static.jsa and dump profile in javac-dynamic.jsa (With Training Data Replay) (skipped)
#       (STEP 4 of 4) Run with javac-dynamic.jsa and generate AOT code (skipped)
#       (STEP 5 of 5) Final production run: with javac-dynamic.jsa and load AOT code (skipped)
#
#  bash javac-test.sh -n $JAVA
#    This runs all the above 5 steps
#======================================================================

# Usage:   bash javac-test.sh $OPTS $JAVA
#          bash javac-test.sh $OPTS $JAVA $JVMARGS
#
# $JAVA must be from inside your JDK build dir. E.g., /your/build/dir/images/jdk/bin/java
#
# $OPTS is optional. Examples:
#
# - Don't run any tests, just print the command-lines
#     bash cds-test.sh -n $JAVA
#
# - Just run test #4
#     bash cds-test.sh -r 4 $JAVA
#
# - Run test 5, enable the JIT compile log, and loop for 10000 iterations. Also pass -Xmx16g to the JVM.
#     bash cds-test.sh -r 5 -lcomp -l 10000 $JAVA -Xmx16g
#
# See the "while" loop below for all available options

MYDIR=$(cd $(dirname $0); pwd)
echo script is installed at $MYDIR

LIMIT=
QUIET=
LOOPS=100
EXTRA_ARGS=
EXIT_WHEN_FAIL=0

while true; do
    if [[ $1 = "-lcomp" ]]; then
        # Log compiler
        EXTRA_ARGS="$EXTRA_ARGS -XX:+PrintCompilation"
        shift
    elif [[ $1 = "-lsc" ]]; then
        # Log shared code
        EXTRA_ARGS="$EXTRA_ARGS -Xlog:sca -Xlog:sca,nmethod"
        shift
    elif [[ $1 = "-r" ]]; then
        # Limit the run to a single test case
        LIMIT=$2
        shift
        shift
    elif [[ $1 = "-l" ]]; then
        # Number of loops for JavacBench
        LOOPS=$2
        shift
        shift
    elif [[ $1 = "-n" ]]; then
        # Don't run any test cases, just print their help message and command line
        LIMIT=0
        shift
    elif [[ $1 = "-f" ]]; then
        # Exit the script with exit code 1 as soon as a test fails
        EXIT_WHEN_FAIL=1
        shift
    elif [[ $1 = "-q" ]]; then
        # Don't print the help messages and command line of test cases that are skipped
        QUIET=1
        shift
    elif [[ $1 = "-all" ]]; then
        # Run all test cases (that are not included in the 5 steps)"
        # If this flag is NOT specified, only the tests with STEP in their description will be executed.
        DOALL=1
        shift
    else
        break
    fi
done

JVM=$1

echo JVM=$JVM

JSA_SUFFIX=-$JVM

# Ioi-specific
if test "$TESTBED" != ""; then
    IOI_PREFIX=$(cd $TESTBED/../..; pwd)
fi

if test "$JVM" = "p"; then
    # Ioi-specific
    PRODUCT_BLD=${IOI_PREFIX}
    IOI_TEST_VM=${IOI_PREFIX}-product/images/jdk
elif test "$JVM" = "d"; then
    PRODUCT_BLD=${IOI_PREFIX}
    IOI_TEST_VM=${IOI_PREFIX}-debug/images/jdk
elif test "$JVM" = "fd"; then
    PRODUCT_BLD=${IOI_PREFIX}
    IOI_TEST_VM=${IOI_PREFIX}-fastdebug/images/jdk
elif [[ ${JVM}XX =~ /images/jdk/bin/javaXX ]]; then
    PRODUCT_BLD=$(cd $(dirname $JVM)/../../..; pwd);
    JSA_SUFFIX=
else
    echo "Please specify the location of the JVM (must be in your /build/dir/images/jdk/bin)"
    exit 1
fi

JVM_AND_ARGS=$@
TEST_NUM=0

function test-info () {
    if test "$DOALL" != "1"; then
        if [[ "$@" =~ STEP ]] ; then
            true
        else
            return 1
        fi
    fi

    TEST_NUM=$(expr $TEST_NUM + 1)
    declare -gA infos
    declare -gA cmds
    cmds[$TEST_NUM]=$CMD
    infos[$TEST_NUM]=$*
    local SKIPPED=""
    if test "$LIMIT" != "" && test "$LIMIT" != "$TEST_NUM"; then
        SKIPPED=" (skipped)"
    fi
    if test "$QUIET" = ""; then
        echo ======================================================================
        echo "Test $TEST_NUM $*$SKIPPED"
        echo $CMD
    fi
    if test "$SKIPPED" != ""; then
        return 1
    else
        return 0
    fi
}

function record_success () {
    somepassed=1
    declare -gA status
    status[$TEST_NUM]=OK
    echo "RESULT: $TEST_NUM = OK"
}

function record_failure () {
    somefailed=1
    declare -gA status
    status[$TEST_NUM]=FAILED
    echo "RESULT: $TEST_NUM = FAILED"
    if test "$EXIT_WHEN_FAIL" = "1"; then
        touch fail-stop.txt
        exit 1
    fi
}

cd $MYDIR

if test ! -f JavacBench.jar; then
    cat > JavacBench.java <<EOF
//this file is auto-generated. Do not add to source control and do not edit.
//import org.openjdk.jmh.annotations.*;

import javax.tools.*;
import java.io.*;
import java.net.URI;
import java.util.*;

//@State(Scope.Benchmark)
public class JavacBench {
    static class ClassFile extends SimpleJavaFileObject {
        private final ByteArrayOutputStream baos = new ByteArrayOutputStream();
        protected ClassFile(String name) {
            super(URI.create("memo:///" + name.replace('.', '/') + Kind.CLASS.extension), Kind.CLASS);
        }
        @Override
        public ByteArrayOutputStream openOutputStream() { return this.baos; }
        byte[] toByteArray() { return baos.toByteArray(); }
    }
    static class FileManager extends ForwardingJavaFileManager<JavaFileManager> {
        private Map<String, ClassFile> classesMap = new HashMap<String, ClassFile>();
        protected FileManager(JavaFileManager fileManager) {
            super(fileManager);
        }
        @Override
        public ClassFile getJavaFileForOutput(Location location, String name, JavaFileObject.Kind kind, FileObject source) {
            ClassFile classFile = new ClassFile(name);
            classesMap.put(name, classFile);
            return classFile;
        }
        public Map<String, byte[]> getByteCode() {
            Map<String, byte[]> result = new HashMap<>();
            for (Map.Entry<String, ClassFile> entry : classesMap.entrySet()) {
                result.put(entry.getKey(), entry.getValue().toByteArray());
            }
            return result;
        }
    }
    static class SourceFile extends SimpleJavaFileObject {
        private CharSequence sourceCode;
        public SourceFile(String name, CharSequence sourceCode) {
            super(URI.create("memo:///" + name.replace('.', '/') + Kind.SOURCE.extension), Kind.SOURCE);
            this.sourceCode = sourceCode;
        }
        @Override
        public CharSequence getCharContent(boolean ignore) {
            return this.sourceCode;
        }
    }

    public Object compile(int count) {
        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        DiagnosticCollector<JavaFileObject> ds = new DiagnosticCollector<>();
        Collection<SourceFile> sourceFiles = sources10k.subList(0, count);

        try (FileManager fileManager = new FileManager(compiler.getStandardFileManager(ds, null, null))) {
            JavaCompiler.CompilationTask task = compiler.getTask(null, fileManager, null, null, null, sourceFiles);
            if (task.call()) {
                return fileManager.getByteCode();
            } else {
                for (Diagnostic<? extends JavaFileObject> d : ds.getDiagnostics()) {
                  System.out.format("Line: %d, %s in %s", d.getLineNumber(), d.getMessage(null), d.getSource().getName());
                  }
                throw new InternalError("compilation failure");
            }
        } catch (IOException e) {
            throw new InternalError(e);
        }
    }

    List<SourceFile> sources10k;

    List<SourceFile> generate(int count) {
        ArrayList<SourceFile> sources = new ArrayList<>(count);
        for (int i = 0; i < count; i++) {
            sources.add(new SourceFile("HelloWorld" + i,
                    "public class HelloWorld" + i + " {" +
                            "    public static void main(String[] args) {" +
                            "        System.out.println(\"Hellow World!\");" +
                            "    }" +
                            "}"));
        }
        return sources;
    }
    //@Setup
    public void setup() {
        sources10k = generate(10_000);
    }

  public static void main(String args[]) throws Throwable {
    JavacBench bench = new JavacBench();
    bench.setup();

    if (args.length >= 0) {
        int count = Integer.parseInt(args[0]);
        if (count > 0) {
            bench.compile(count);
        }
    }
  }
}
EOF
    (set -x
    rm -rf tmpclasses
    mkdir -p tmpclasses
    ${PRODUCT_BLD}/images/jdk/bin/javac -d tmpclasses JavacBench.java || exit 1
    ${PRODUCT_BLD}/images/jdk/bin/jar cf JavacBench.jar -C tmpclasses . || exit 1
    rm -rf tmpclasses
    ) || exit 1
fi

#----------------------------------------------------------------------
CMD="$JVM_AND_ARGS \
    -cp JavacBench.jar -XX:DumpLoadedClassList=javac.classlist \
    -Xlog:class+load=debug:file=javac.class+load.log \
    JavacBench 1000"
test-info "(STEP 1 of 5) Dump javac.classlist" &&
if $CMD; then
    ls -l javac.classlist
    wc -l javac.classlist
    record_success
    # remove old log
    rm -f javac.class+load.log.*
else
    record_failure
fi

JSA=javac-static${JSA_SUFFIX}.jsa

#----------------------------------------------------------------------
CMD="$JVM_AND_ARGS \
    -Xshare:dump -XX:SharedArchiveFile=$JSA \
    -XX:SharedClassListFile=javac.classlist \
    -XX:+PreloadSharedClasses \
    -cp JavacBench.jar \
    -Xlog:cds=debug,cds+class=debug,cds+resolve=debug:file=javac.staticdump.log::filesize=0"
test-info "(STEP 2 of 5) Create Static $JSA" &&
if $CMD; then
    ls -l $JSA
    record_success
    rm -f javac.staticdump.log.*
else
    record_failure
fi

#----------------------------------------------------------------------
CMD="$JVM_AND_ARGS $EXTRA_ARGS \
    -XX:SharedArchiveFile=$JSA \
    -cp JavacBench.jar JavacBench $LOOPS"
test-info "Run with $JSA" &&
if $CMD; then
    record_success
else
    record_failure
fi

#----------------------------------------------------------------------
CMD="$JVM_AND_ARGS $EXTRA_ARGS \
    -cp JavacBench.jar JavacBench $LOOPS"
test-info "Run with default archive" &&
if $CMD; then
    record_success
else
    record_failure
fi

#----------------------------------------------------------------------
CMD="$JVM_AND_ARGS $EXTRA_ARGS -Xshare:off -Xlog:cds=debug \
    -cp JavacBench.jar JavacBench $LOOPS"
test-info "Run without CDS" &&
if $CMD; then
    record_success
else
    record_failure
fi

TESTS="1 2"

for i in $TESTS; do
    if test $i = 2; then
        REPLAY=" (With Training Data Replay)"
        X1="-XX:+RecordTraining"
        X2="-XX:+ReplayTraining"
        STEP3="(STEP 3 of 5) "
        STEP4="(STEP 4 of 5) "
        STEP5="(STEP 5 of 5) "
   fi

    DYNJSA=javac-dynamic1${JSA_SUFFIX}-$i.jsa

    #----------------------------------------------------------------------
    CMD="$JVM_AND_ARGS $EXTRA_ARGS -Xlog:cds=debug \
            -cp JavacBench.jar -XX:ArchiveClassesAtExit=$DYNJSA $X1 JavacBench $LOOPS"
    test-info "Dump CDS dynamic archive$REPLAY" &&
    if $CMD; then
            record_success
        else
            record_failure
        fi


    #----------------------------------------------------------------------
    CMD="$JVM_AND_ARGS $EXTRA_ARGS -Xlog:cds \
        -cp JavacBench.jar -XX:SharedArchiveFile=$DYNJSA $X2 JavacBench $LOOPS"
    test-info "Use CDS dynamic archive$REPLAY" &&
    if $CMD; then
        record_success
    else
        record_failure
    fi

    JSA2=javac-dynamic2${JSA_SUFFIX}-$i.jsa
    if test "$JSA2" = "javac-dynamic2-2.jsa"; then
        JSA2=javac-dynamic.jsa
    fi

    #----------------------------------------------------------------------
    CMD="$JVM_AND_ARGS $EXTRA_ARGS -Xlog:cds=debug,cds+class=debug:file=javac.dynamicdump.log::filesize=0 \
        -XX:SharedArchiveFile=$JSA -XX:ArchiveClassesAtExit=$JSA2 $X1 \
        -cp JavacBench.jar JavacBench $LOOPS"
    test-info "${STEP3}Run with $JSA and dump profile in $JSA2${REPLAY}" &&
    if $CMD; then
        record_success
    else
        record_failure
    fi

    #----------------------------------------------------------------------
    CMD="$JVM_AND_ARGS $EXTRA_ARGS \
        -XX:SharedArchiveFile=$JSA2 $X2 \
        -cp JavacBench.jar JavacBench $LOOPS"
    test-info "Run with $JSA2 $REPLAY" &&
    if $CMD; then
        record_success
    else
        record_failure
    fi

    #----------------------------------------------------------------------
    CMD="$JVM_AND_ARGS $EXTRA_ARGS -Xlog:sca*=trace:file=javac.sca-store.log::filesize=0 \
        -XX:SharedArchiveFile=$JSA2 $X2 -XX:+StoreSharedCode -XX:SharedCodeArchive=${JSA2}-sc -XX:ReservedSharedCodeSize=100M \
        -cp JavacBench.jar JavacBench $LOOPS"
    test-info "${STEP4}Run with $JSA2 and generate AOT code" &&
    if $CMD; then
        record_success
    else
        record_failure
    fi

    #----------------------------------------------------------------------
    CMD="$JVM_AND_ARGS $EXTRA_ARGS -Xlog:sca*=trace:file=javac.sca-load.log::filesize=0 \
        -XX:SharedArchiveFile=$JSA2 $X2 -XX:+LoadSharedCode -XX:SharedCodeArchive=${JSA2}-sc -XX:ReservedSharedCodeSize=100M \
        -cp JavacBench.jar JavacBench $LOOPS"
    test-info "${STEP5}Final production run: with $JSA2 and load AOT code" &&
    if $CMD; then
        record_success
    else
        record_failure
    fi
done

echo ===SUMMARY===
for (( i=1; i<=$TEST_NUM; i++ )) ; do
    if test "${status[$i]}" != ""; then
        echo ......................
        echo $i ${infos[$i]} = ${status[$i]}
        echo "    $(echo ${cmds[$i]})"
    fi
done

if test "$somefailed" = "1"; then
    echo ""
    echo '***********************SOME TESTS FAILED ********************************************'
    echo ""
    for (( i=1; i<=$TEST_NUM; i++ )) ; do
        if test "${status[$i]}" = "FAILED"; then
            echo $i ${infos[$i]} = ${status[$i]}
        fi
    done
    echo ""
elif test "$somepassed" = "1"; then
    echo ""
    echo '*** all passed *****'
    echo ""
fi
