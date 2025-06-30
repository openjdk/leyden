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
# Leyden-premain + Helidon helidon-quickstart-se demo
# (https://helidon.io/docs/v4/se/guides/quickstart)
#
# See ../lib/DemoSupport.gmk for the "make" targets that build the demo, run the
# demo, and gather benchmarking data.
#===============================================================================

# The following needs to be defined before including ../lib/DemoSupport.gmk
DEMO_NAME       = helidon-quickstart-se
DEMO_SRC        = helidon-quickstart-se/src/main/java/io/helidon/examples/quickstart/se/Main.java
DEMO_JAR        = helidon-quickstart-se/target/helidon-quickstart-se.jar
DEMO_CMDLINE    = -DautoQuit=true -Dserver.port=0 -jar ${DEMO_JAR}
# This is for uploading into Artifactory, usually to a path like
#    https://xxxx.com/artifactory/yyyy/io/helidon/helidon-quickstart-se/4.0.7/helidon-quickstart-se-4.0.7.zip
DEMO_ARTIFACT   = helidon-quickstart-se-4.0.7.zip

include ../lib/DemoSupport.gmk

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


${DEMO_ARTIFACT}: ${DEMO_JAR}
	${JAR} cvfM ${DEMO_ARTIFACT} \
		-C helidon-quickstart-se target/helidon-quickstart-se.jar \
		-C helidon-quickstart-se target/libs

${DEMO_JAR}: ${DEMO_SRC}
	cd helidon-quickstart-se && JAVA_HOME=${BLDJDK_HOME} mvn package

clean: clean0
	rm -rf helidon-quickstart-se ${DEMO_ARTIFACT}

clean0:
	rm -f *.cds *.cds.code *.classlist *.jsa *~ *.log *.log.* hs_err_* *.aot *.aotconf

