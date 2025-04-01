#/bin/bash

J=build/linux-x86_64-server-release/images/jdk/bin/java

OPTS="-Xms64m -Xmx8g -XX:+UseSerialGC -cp JavacBenchApp.jar"

rm -f hs_err*

TRAINING_CONF="50"
PROD_CONF="50"

TRAINING_OPTS="$OPTS"
PROD_OPTS="$OPTS"

echo "Building"
rm JavacBenchApp.jar
${J}c ./test/hotspot/jtreg/runtime/cds/appcds/applications/JavacBenchApp.java -d .
jar cvf JavacBenchApp.jar JavacBenchApp*.class

echo "Training"

rm -f *.aot *.aotconf
time $J -XX:AOTMode=record -XX:AOTConfiguration=app.aotconf $TRAINING_OPTS JavacBenchApp $TRAINING_CONF
time $J -XX:AOTMode=create -XX:AOTConfiguration=app.aotconf $TRAINING_OPTS -XX:AOTCache=app.aot -Xlog:deoptimization=debug JavacBenchApp $TRAINING_CONF 2>&1 | tee print-assembly.log

$J $PROD_OPTS -XX:AOTCache=app.aot -XX:+PrintCompilation -Xlog:scc+deoptimization=debug -Xlog:deoptimization=debug -XX:+UnlockDiagnosticVMOptions JavacBenchApp $PROD_CONF 2>&1 | tee print-compilation.log

echo -------------------------------
grep com.sun.tools.javac.util.StringNameTable::fromString print-assembly.log
echo -------------------------------
grep com.sun.tools.javac.util.StringNameTable::fromString print-compilation.log


