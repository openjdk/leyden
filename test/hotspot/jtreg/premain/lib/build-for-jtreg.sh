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



# build-for-jtreg.sh --
#
# This script builds the binaries needed by the tests under ../../runtime/cds/appcds/applications/
# It also prints out the arguments to be added to the jtreg command-line

# Edit the following according to your local setting, or pass them in like this
#
# env BLDJDK_HOME=/my/java/home ROOT=/my/repo MVN=${HOME}/bin/maven/apache-maven-3.8.8/bin/mvn bash build-for-jtreg.sh
#
if test "$BLDJDK_HOME" = ""; then
    # you need JDK 21
    BLDJDK_HOME=/jdk3/official/jdk21
fi

if test "$REPO" = ""; then
    # This is your copy of https://github.com/openjdk/leyden/tree/premain
    ROOT=/jdk3/le4/open
fi

# MVN should point to the mvn executable. You need at least 3.8.2
# If MVN is not specified, we will use the mvn installed in your $PATH
if test "$MVN" != ""; then
    MVNARG="MVN=$MVN"
else
    MVNARG=
fi

cd $ROOT/test/hotspot/jtreg/premain
for i in helidon-quickstart-se micronaut-first-app quarkus-getting-started spring-petclinic; do
    (cd $i; make app BLDJDK_HOME=${BLDJDK_HOME} ${MVNARG}) || exit 1
done

PM=$ROOT/test/hotspot/jtreg/premain
echo Add the following to your jtreg command-line
echo "  -vmoption:-Djdk.test.lib.artifacts.helidon-quickstart-se=$PM/helidon-quickstart-se/helidon-quickstart-se \\"
echo "  -vmoption:-Djdk.test.lib.artifacts.micronaut-first-app=$PM/micronaut-first-app/download/target \\" 
echo "  -vmoption:-Djdk.test.lib.artifacts.quarkus-getting-started=$PM/quarkus-getting-started/getting-started/target \\"
echo "  -vmoption:-Djdk.test.lib.artifacts.spring-petclinic=$PM/spring-petclinic/petclinic-snapshot/target/spring-petclinic-3.2.0.zip \\"

exit

===============================================================================
This is how Ioi executes the tests:

env JAVA_HOME=/jdk3/official/jdk22 \
    /jdk3/official/jtreg7.3.1/bin/jtreg \
    -J-Djavatest.maxOutputSize=10000000 \
    -conc:1 \
    -testjdk:/jdk3/bld/le4-fastdebug/images/jdk \
    -compilejdk:/jdk3/bld/le4/images/jdk \
    -verbose:2 \
    -timeout:4.0 \
    -agentvm \
    -vmoptions:-XX:MaxRAMPercentage=6 \
    -noreport \
    -vmoption:-Djdk.test.lib.artifacts.helidon-quickstart-se=/jdk3/le4/open/test/hotspot/jtreg/premain/helidon-quickstart-se/helidon-quickstart-se \
    -vmoption:-Djdk.test.lib.artifacts.micronaut-first-app=/jdk3/le4/open/test/hotspot/jtreg/premain/micronaut-first-app/download/target \
    -vmoption:-Djdk.test.lib.artifacts.quarkus-getting-started=/jdk3/le4/open/test/hotspot/jtreg/premain/quarkus-getting-started/getting-started/target \
    -vmoption:-Djdk.test.lib.artifacts.spring-petclinic=/jdk3/le4/open/test/hotspot/jtreg/premain/spring-petclinic/petclinic-snapshot/target/spring-petclinic-3.2.0.zip \
    -w /home/iklam/tmp/jtreg/work \
    HelidonQuickStartSE.java#static \
    MicronautFirstApp.java#static \
    QuarkusGettingStarted.java#static \
    spring-petclinic/SpringPetClinicStatic.java
