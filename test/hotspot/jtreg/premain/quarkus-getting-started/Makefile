# Copyright (c) 2024, 2025, Oracle and/or its affiliates. All rights reserved.
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
# Leyden-premain + Quarkus getting-started demo
#  - https://quarkus.io/guides/getting-started
#  - https://quarkus.io/guides/lifecycle
#
# See ../lib/DemoSupport.gmk for the "make" targets that build the demo, run the
# demo, and gather benchmarking data.
#===============================================================================

# The following needs to be defined before including ../lib/DemoSupport.gmk
DEMO_NAME       = quarkus-getting-started
DEMO_SRC        = getting-started/src/main/java/Main.java
DEMO_JAR        = getting-started/target/getting-started-1.0.0-SNAPSHOT-runner.jar
DEMO_CMDLINE    = -DautoQuit=true -Dquarkus.http.port=0 -jar ${DEMO_JAR}
# This is for uploading into Artifactory, usually to a path like
#    https://xxxx.com/artifactory/yyyy/io/quarkus/quarkus-getting-started/1.0.0b/quarkus-getting-started-1.0.0b.zip
DEMO_ARTIFACT   = quarkus-getting-started-1.0.0b.zip

include ../lib/DemoSupport.gmk

# Create the project using instructions from https://quarkus.io/guides/getting-started, then
# add the Main class which prints out the timing data and exits.
${DEMO_SRC}: Main.java
	rm -rf getting-started
	JAVA_HOME=${BLDJDK_HOME} ${MVN} \
	    io.quarkus.platform:quarkus-maven-plugin:3.9.3:create \
	    -DprojectGroupId=org.acme \
	    -DprojectArtifactId=getting-started \
	    -Dextensions='rest'
	cp Main.java ${DEMO_SRC}

# We package the app in the "legacy-jar" to be CDS friendly
# See https://quarkus.io/guides/class-loading-reference:
#  "everything is loaded in the system ClassLoader, so it is a completely flat classpath."
# See https://www.cheat-sheets.org/saved-copy/quarkus-cheat-sheet.pdf
${DEMO_JAR}: ${DEMO_SRC}
	cd getting-started && JAVA_HOME=${BLDJDK_HOME} ${MVN} -Dquarkus.package.type=legacy-jar install

${DEMO_ARTIFACT}: ${DEMO_JAR}
	${JAR} cvfM ${DEMO_ARTIFACT} \
		-C getting-started/target .

clean: clean0
	rm -rf getting-started ${DEMO_ARTIFACT}

clean0:
	rm -f *.cds *.cds.code *.classlist *.jsa *~ *.log *.log.* hs_err_* *.aot *.aotconf

