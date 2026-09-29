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
# Leyden-premain + Sprint-boot getting-started demo
#  - https://spring.io/guides/gs/spring-boot
#  - https://docs.spring.io/spring-boot/maven-plugin/aot.html
#  - https://bell-sw.com/blog/how-to-use-cds-with-spring-boot-applications/
#
# See ../lib/DemoSupport.gmk for the "make" targets that build the demo, run the
# demo, and gather benchmarking data.
#===============================================================================

# The following needs to be defined before including ../lib/DemoSupport.gmk
DEMO_NAME       = spring-boot-getting-started
DEMO_SRC        = gs-spring-boot/complete/src/main/java/com/example/springboot/Application.java
DEMO_JAR        = gs-spring-boot/complete/spring-boot-complete-0.0.1-SNAPSHOT/spring-boot-complete-0.0.1-SNAPSHOT.jar
DEMO_CMDLINE    = -Dserver.port=0 -Dspring.aot.enabled=true -Dspring.output.ansi.enabled=NEVER -Dspring.context.exit=onRefresh -jar ${DEMO_JAR}
# This is for uploading into Artifactory, usually to a path like
#    https://xxxx.com/artifactory/yyyy/io/spring/spring-boot-getting-started/3.3.0/spring-boot-getting-started-3.3.0.zip
DEMO_ARTIFACT   = spring-boot-getting-started-3.3.0.zip

include ../lib/DemoSupport.gmk

# Create the project using instructions from https://spring.io/guides/gs/spring-boot and
# patch the ${DEMO_SRC} to show start-up measurement used by ../lib/DemoSupport.gmk
# Note: we use GIT version 99085f078b01fed27667c0b8fbbb957758911f4a
# Note: pom.xml is patched to add <goal>process-aot</goal>; see https://docs.spring.io/spring-boot/maven-plugin/aot.html
${DEMO_SRC}: Application.java pom.xml
	rm -rf gs-spring-boot ${DEMO_SRC}
	git clone https://github.com/spring-guides/gs-spring-boot.git
	cd gs-spring-boot; git reset --hard 99085f078b01fed27667c0b8fbbb957758911f4a
	cp -v Application.java ${DEMO_SRC}
	cp -v pom.xml gs-spring-boot/complete

# Package the app in CDS friendly way
# See https://bell-sw.com/blog/how-to-use-cds-with-spring-boot-applications/
${DEMO_JAR}: ${DEMO_SRC}
	cd gs-spring-boot/complete && JAVA_HOME=${BLDJDK_HOME} ${MVN} -Dmaven.test.skip=true package
	cd gs-spring-boot/complete && ${BLDJDK_JAVA} -Djarmode=tools -jar target/spring-boot-complete-0.0.1-SNAPSHOT.jar extract

${DEMO_ARTIFACT}: ${DEMO_JAR}
	${JAR} cvfM ${DEMO_ARTIFACT} \
		-C gs-spring-boot/complete/spring-boot-complete-0.0.1-SNAPSHOT .

clean: clean0
	rm -rf gs-spring-boot ${DEMO_ARTIFACT} mainline_vs_premain.* compare_premain_builds.*

clean0:
	rm -f *.cds *.cds.code *.classlist *.jsa *~ *.log *.log.* hs_err_* tmp.txt *.aot *.aotconf

# Show the patch we made to gs-spring-boot
diff:
	cd gs-spring-boot; git diff -w

