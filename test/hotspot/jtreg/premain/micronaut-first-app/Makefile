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
# Leyden-premain + Micronaut first app
#  - https://guides.micronaut.io/latest/creating-your-first-micronaut-app-maven-java.html
#  - https://guides.micronaut.io/latest/creating-your-first-micronaut-app-maven-java.zip
#
# See ../lib/DemoSupport.gmk for the "make" targets that build the demo, run the
# demo, and gather benchmarking data.
#===============================================================================

# The following needs to be defined before including ../lib/DemoSupport.gmk
DEMO_NAME       = micronaut-first-app
DEMO_SRC        = $(wildcard ./download/src/main/java/example/micronaut/*.java)
DEMO_JAR        = download/target/default-0.1.jar
DEMO_CMDLINE    = -DautoQuit=true -Dmicronaut.server.port=0 -jar ${DEMO_JAR}
# This is for uploading into Artifactory, usually to a path like
#    https://xxxx.com/artifactory/yyyy/io/micronaut/micronaut-first-app/1.0.0/micronaut-first-app-1.0.0.zip
DEMO_ARTIFACT   = micronaut-first-app-1.0.0.zip

include ../lib/DemoSupport.gmk

${DEMO_JAR}: ${DEMO_SRC}
	cd download && JAVA_HOME=${BLDJDK_HOME} ${MVN} package

${DEMO_ARTIFACT}: ${DEMO_JAR}
	${JAR} cvfM ${DEMO_ARTIFACT} \
		-C download/target default-0.1.jar

clean: clean0
	rm -rf getting-started ${DEMO_ARTIFACT} download/target

clean0:
	rm -f *.cds *.cds.code *.classlist *.jsa *~ *.log *.log.* hs_err_* *.aot *.aotconf


