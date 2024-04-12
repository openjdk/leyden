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

#===============================================================================
# Leyden-premain + Helidon helidon-quickstart-se demo (https://helidon.io/docs/v4/se/guides/quickstart)
#
# Build the app from source code
#    make app
#
# Run with all Leyden optimizations (new workflow)
#    make run
#    make runn
#
# Run with all CDS static archive only (with Leyden repo)
#    make runs
#
# Run without Leyden optimizations (with Leyden repo)
#    make run0

# Set the following to point to JDK 21, your JDK mainline build, and your Leyden JDK build.
#
#  *** NOTE: JDK 21 is needed to build
BLDJDK_HOME   = /jdk3/official/jdk21

# Points to your build with https://github.com/openjdk/jdk
# For comparison purposes, this build should be the latest version of the mainline that
# has been merged into https://github.com/openjdk/leyden/tree/premain
MAINLINE_HOME = /jdk3/bld/rum/images/jdk

# Points to your build with https://github.com/openjdk/leyden/tree/premain
PREMAIN_HOME = /jdk3/bld/le4/images/jdk
#===============================================================================

# Options passed to PREMAIN_JAVA. See compare_premain_builds below
PM_OPTS =

JAR             = ${BLDJDK_HOME}/bin/jar
BLDJDK_JAVA     = ${BLDJDK_HOME}/bin/java
MAINLINE_JAVA   = ${MAINLINE_HOME}/bin/java
PREMAIN_JAVA    = ${PREMAIN_HOME}/bin/java ${PM_OPTS}
DEMO_SRC        = helidon-quickstart-se/src/main/java/io/helidon/examples/quickstart/se/Main.java
DEMO_JAR        = helidon-quickstart-se/target/helidon-quickstart-se.jar
DEMO_CMDLINE    = -DautoQuit=true -Dserver.port=0 -jar ${DEMO_JAR}

# This is for uploading into Artifactory, usually to a path like
#    https://xxxx.com/artifactory/yyyy/io/helidon/helidon-quickstart-se/4.0.7/helidon-quickstart-se-4.0.7.zip
DEMO_ARTIFACT   = helidon-quickstart-se-4.0.7.zip

# Static CDS only
DEMO_CLASSLIST  = helidon-quickstart-se$(PM_VER).classlist
DEMO_JSA        = helidon-quickstart-se$(PM_VER).jsa

# Premain New workflow
DEMO_CDS        = helidon-quickstart-se$(PM_VER).cds

src: ${DEMO_SRC}

# Create the project using instructions from https://helidon.io/docs/v4/se/guides/quickstart, then
# patch it to add some start-up timing data.
${DEMO_SRC}: Main.java.diff
	JAVA_HOME=${BLDJDK_HOME} mvn -U archetype:generate -DinteractiveMode=false \
	    -DarchetypeGroupId=io.helidon.archetypes \
	    -DarchetypeArtifactId=helidon-quickstart-se \
	    -DarchetypeVersion=4.0.7 \
	    -DgroupId=io.helidon.examples \
	    -DartifactId=helidon-quickstart-se \
	    -Dpackage=io.helidon.examples.quickstart.se
	patch ${DEMO_SRC} Main.java.diff

artifact: ${DEMO_ARTIFACT}

${DEMO_ARTIFACT}: ${DEMO_JAR}
	${JAR} cvfM ${DEMO_ARTIFACT} \
		-C helidon-quickstart-se target/helidon-quickstart-se.jar \
		-C helidon-quickstart-se target/libs

#======================================================================
# TODO: Hopefully the rest of this file can be factored out, so it can
#       be included by various test cases.
#======================================================================
app: ${DEMO_JAR}

${DEMO_JAR}: ${DEMO_SRC}
	cd helidon-quickstart-se && JAVA_HOME=${BLDJDK_HOME} mvn package

# Run with the build JDK------------------------
runb: ${DEMO_JAR}
	${BLDJDK_JAVA} ${DEMO_CMDLINE}

# No optimizations (Leyden repo)----------------
run0: ${DEMO_JAR}
	${PREMAIN_JAVA} ${DEMO_CMDLINE}

# Static CDS only (Leyden repo)-----------------

list:  ${DEMO_CLASSLIST}
jsa:   ${DEMO_JSA}

${DEMO_CLASSLIST}: ${DEMO_JAR}
	${PREMAIN_JAVA} -Xshare:off -XX:DumpLoadedClassList=${DEMO_CLASSLIST} ${DEMO_CMDLINE}

${DEMO_JSA}: ${DEMO_CLASSLIST}
	${PREMAIN_JAVA} -Xshare:dump -XX:SharedClassListFile=${DEMO_CLASSLIST} -XX:SharedArchiveFile=${DEMO_JSA} \
	    ${DEMO_CMDLINE}

runs:  ${DEMO_JSA}
	${PREMAIN_JAVA} -XX:SharedArchiveFile=${DEMO_CDS} ${DEMO_CMDLINE}

# Leyden new workflow --------------------------

cds: ${DEMO_CDS}

${DEMO_CDS}: ${DEMO_JAR}
	rm -f ${DEMO_CDS} ${DEMO_CDS}.log
	${PREMAIN_JAVA} \
	    -XX:CacheDataStore=${DEMO_CDS} -XX:CachedCodeMaxSize=512M \
	    -Xlog:scc,cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=${DEMO_CDS}.log::filesize=0 \
	     ${DEMO_CMDLINE}
	ls -l ${DEMO_CDS}
	ls -l ${DEMO_CDS}.code

run: runn

runn: ${DEMO_CDS}
	${PREMAIN_JAVA} \
	    -XX:CacheDataStore=${DEMO_CDS} \
	    -Xlog:scc*=error \
	     ${DEMO_CMDLINE}

clean: clean0
	rm -rf helidon-quickstart-se ${DEMO_ARTIFACT}

clean0:
	rm -f *.cds *.cds.code *.classlist *.jsa *~ *.log *.log.* hs_err_*

