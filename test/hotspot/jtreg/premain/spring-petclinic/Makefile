# Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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
# Leyden-premain + spring-petclinic demo (https://github.com/spring-projects/spring-petclinic)
#
# Build spring-petclinic:
#    make app
#
# Build all leyden optimization artifacts (this is using the older "4 step training run" which will be simplified):
#    make aot
#
# Run with Leyden optimizations
#    make run
#
# Run without Leyden optimizations
#    make run0

# Set the following to point to JDK 17 and your Leyden JDK build.
#
#  *** NOTE: JDK 17 (or 21??) is needed to build spring-petclinic-3.2.0-SNAPSHOT.jar
JDK17_HOME   = /jdk3/official/jdk17
PREMAIN_HOME = /jdk3/bld/le4/images/jdk
#===============================================================================

# Usually there's no need to change the following
JAR_CMD          = ${JDK17_HOME}/bin/jar
JDK17_JAVA       = ${JDK17_HOME}/bin/java
PREMAIN_JAVA     = ${PREMAIN_HOME}/bin/java
PC_REPO          = petclinic-snapshot
PC_JAVA_SOURCES = ${PC_REPO}/src/main/java/org/springframework/samples/petclinic/PetClinicApplication.java
PC_APP_JAR       = ${PC_REPO}/target/spring-petclinic-3.2.0-SNAPSHOT.jar
PC_APP_UNPACKED  = ${PC_REPO}/target/unpacked

# This is for uploading to artifactory, to be tested with
# ../../runtime/cds/appcds/leyden/SpringPetClinic.java
PC_APP_UNPACKED_ZIP = ${PC_REPO}/target/spring-petclinic-3.2.0.zip

PC_MAIN_CLASS    = org.springframework.samples.petclinic.PetClinicApplication
PC_CLASSLIST     = spring-petclinic.classlist
PC_STATIC_JSA    = spring-petclinic.static.jsa
PC_DYNAMIC_JSA   = spring-petclinic.dynamic.jsa
PC_CACHED_CODE   = spring-petclinic.code.jsa

# new workflow
PC_CDS_PREIMAGE  = spring-petclinic.cds.preimage
PC_CDS           = spring-petclinic.cds

# TODO: should we add -Dspring.context.exit=onRefresh to command line??
# This will make the JVM quit after printing this line:
#
#    4:21.639 ... o.s.b.a.e.web.EndpointLinksResolver      : Exposing 13 endpoint(s) beneath base path '/actuator'
#
# Whereas -DautoQuit=true will make it exit after printing the following (a little bit of application code is executed)
#
#    4:21.665 ... o.s.b.w.embedded.tomcat.TomcatWebServer  : Tomcat started on port 8080 (http) with context path ''
#    4:21.666 ... o.s.s.petclinic.PetClinicApplication     : Started PetClinicApplication in 1.358 seconds (process running for 1.584)
#    #### Booted and returned in 1453ms
#    4:21.692 ... j.LocalContainerEntityManagerFactoryBean : Closing JPA EntityManagerFactory for persistence unit 'default'
#    4:21.693 ... com.zaxxer.hikari.HikariDataSource       : HikariPool-1 - Shutdown initiated...
#    4:21.694 ... com.zaxxer.hikari.HikariDataSource       : HikariPool-1 - Shutdown completed.
HEAP_SIZE        = -Xmx2g
UNPACKED_CMDLINE = $(HEAP_SIZE) -cp @${PC_APP_UNPACKED}/classpath -DautoQuit=true -Dspring.aot.enabled=true -Dspring.output.ansi.enabled=NEVER ${PC_MAIN_CLASS}

all: check git app aot

check:
	@if ${JDK17_JAVA} --version | grep -q '17.*LTS'; then \
	    true; \
	else \
	    echo JDK17_HOME should point to JDK 17; \
	    exit 1; \
	fi
	@if ${PREMAIN_JAVA} -XX:+PrintFlagsFinal --version | grep -q ArchiveInvokeDynamic; then \
	    true; \
	else \
	    echo PREMAIN_HOME should point to your build of https://github.com/openjdk/leyden/tree/premain; \
	    exit 1; \
	fi

# A bunch of quick targets so you can type "make list" to create the classlist, etc
git: ${PC_JAVA_SOURCES}
app: ${PC_APP_JAR}
unpack: ${PC_APP_UNPACKED}

# The "4 step training run" workflow -- this will soon be replaced with the new workflow in ../javac_new_workflow/
list: ${PC_CLASSLIST}
static: ${PC_STATIC_JSA}
dynamic: ${PC_DYNAMIC_JSA}
aot: ${PC_CACHED_CODE}

# Check out this specific version of spring-petclinic that we have been testing with.
# https://github.com/spring-projects/spring-petclinic/commit/80fd11067c4662486e4c635deceba927375b621c
#    Author: Dave Syer <dsyer@vmware.com>
#    Date:   Wed Jan 10 05:01:00 2024
#        Upgrade to Boot 3.2.1
${PC_JAVA_SOURCES}:
	rm -rf ${PC_REPO}
	git clone https://github.com/spring-projects/spring-petclinic ${PC_REPO}
	cd ${PC_REPO}; git reset --hard 80fd11067c4662486e4c635deceba927375b621c
	cd ${PC_REPO}; git apply ../premain-patch.diff

# Need to touch ${PC_APP_JAR} as mvn wants to set it the release date of 3.2.0-SNAPSHOT
${PC_APP_JAR}: ${PC_JAVA_SOURCES}
	cd ${PC_REPO}; JAVA_HOME=${JDK17_HOME} mvn -Dmaven.test.skip=true package
	if test -f ${PC_APP_JAR}; then \
	    touch ${PC_APP_JAR}; \
	fi

${PC_APP_UNPACKED}: ${PC_APP_JAR}
	rm -rf ${PC_APP_UNPACKED}
	mkdir -p ${PC_APP_UNPACKED}
	cd ${PC_APP_UNPACKED} && \
	    ${JAR_CMD} xf ../spring-petclinic-3.2.0-SNAPSHOT.jar && \
	    ${JAR_CMD} cf classes.jar META-INF org && \
            cd BOOT-INF/classes/ && \
	    ${JAR_CMD} cf classes.jar *
	echo ${PC_APP_UNPACKED}/classes.jar $$(find ${PC_APP_UNPACKED}/BOOT-INF -name \*.jar | sort) \
	    | sed -e 's/ /:/g' > ${PC_APP_UNPACKED}/classpath
	${JAR_CMD} cf0 ${PC_APP_UNPACKED_ZIP} ${PC_APP_UNPACKED}
	$(MAKE) run17

#uncomment this to see the generated classes for dynamic proxies
#SAVE_GEN_FILES=-Djdk.proxy.ProxyGenerator.saveGeneratedFiles=true

${PC_CLASSLIST}: ${PC_APP_UNPACKED}
	${PREMAIN_JAVA} -Xshare:off -XX:DumpLoadedClassList=${PC_CLASSLIST} \
	    -Xlog:class+load=debug:file=spring-petclinic.classload.log ${SAVE_GEN_FILES} \
	    ${UNPACKED_CMDLINE}
	wc -lc ${PC_CLASSLIST}

${PC_STATIC_JSA}: ${PC_CLASSLIST}
	rm -f ${PC_STATIC_JSA}.log
	${PREMAIN_JAVA} -Xshare:dump -XX:SharedClassListFile=${PC_CLASSLIST} $(HEAP_SIZE) -cp @${PC_APP_UNPACKED}/classpath \
	    -XX:SharedArchiveFile=${PC_STATIC_JSA} -XX:+ArchiveInvokeDynamic -Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=${PC_STATIC_JSA}.log
	ls -l ${PC_STATIC_JSA}

${PC_DYNAMIC_JSA}: ${PC_STATIC_JSA}
	rm -f ${PC_DYNAMIC_JSA} ${PC_DYNAMIC_JSA}.log
	${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA} -XX:ArchiveClassesAtExit=${PC_DYNAMIC_JSA} \
	    -Xlog:cds=debug,cds+class=debug,cds+resolve=debug:file=${PC_DYNAMIC_JSA}.log \
	    -XX:+RecordTraining ${UNPACKED_CMDLINE}
	ls -l ${PC_DYNAMIC_JSA}

${PC_CACHED_CODE}: ${PC_DYNAMIC_JSA}
	${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_DYNAMIC_JSA} -XX:+ReplayTraining -XX:+StoreCachedCode \
	     -XX:CachedCodeFile=${PC_CACHED_CODE} -XX:CachedCodeMaxSize=512M ${UNPACKED_CMDLINE}
	ls -l ${PC_CACHED_CODE}

# run with premain optimizatipn
run: ${PC_CACHED_CODE}
	time ${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_DYNAMIC_JSA} -XX:+ReplayTraining -XX:+LoadCachedCode \
		-XX:CachedCodeFile=${PC_CACHED_CODE} -Xlog:init -Xlog:scc=error ${UNPACKED_CMDLINE}

# run with just static CDS archive
runs: ${PC_STATIC_JSA}
	time ${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA} \
		-Xlog:init ${UNPACKED_CMDLINE}

# run with just dynamic CDS archive
rund: ${PC_DYNAMIC_JSA}
	time ${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_DYNAMIC_JSA} \
		-Xlog:init ${UNPACKED_CMDLINE}

# run WITHOUT premain optimizatipn
run0: ${PC_APP_UNPACKED}
	time ${PREMAIN_JAVA} -Xlog:init ${UNPACKED_CMDLINE}

run17: ${PC_APP_UNPACKED}
	time ${JDK17_JAVA} ${UNPACKED_CMDLINE}

#======================================== new workflow
pre: ${PC_CDS_PREIMAGE}
cds: ${PC_CDS}

${PC_CDS_PREIMAGE}: ${PC_APP_UNPACKED}
	rm -f ${PC_CDS_PREIMAGE} ${PC_CDS}
	${PREMAIN_JAVA} -XX:+ArchiveInvokeDynamic -XX:CachedCodeMaxSize=512M -XX:+UnlockDiagnosticVMOptions -XX:+CDSManualFinalImage -XX:CacheDataStore=${PC_CDS} \
	    -Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=${PC_CDS_PREIMAGE}.log \
	    ${UNPACKED_CMDLINE}
	ls -l ${PC_CDS_PREIMAGE}

${PC_CDS}: ${PC_CDS_PREIMAGE}
	rm -f ${PC_CDS}
	${PREMAIN_JAVA} -XX:+ArchiveInvokeDynamic -XX:CachedCodeMaxSize=512M -XX:+UnlockDiagnosticVMOptions -XX:CDSPreimage=${PC_CDS_PREIMAGE} -XX:CacheDataStore=${PC_CDS} \
	    -Xlog:scc,cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=${PC_CDS}.log \
	    ${UNPACKED_CMDLINE}
	ls -l ${PC_CDS}
	ls -l ${PC_CDS}.code

# run with NEW workflow
runn: ${PC_CDS}
	time ${PREMAIN_JAVA} -XX:CacheDataStore=${PC_CDS} -Xlog:init -Xlog:scc=error ${UNPACKED_CMDLINE}

clean: clean0
	rm -rf *~ ${PC_REPO}

# clean the the leyden artifacts
clean0:
	rm -fv spring-petclinic.*

# clean the the leyden artifacts for the new workflow only
cleann:
	rm -fv spring-petclinic.cds.*
