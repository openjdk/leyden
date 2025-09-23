# Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
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
# Run with mainline, using only default CDS archive
#    make run0.ml
#
# Run with mainline, using custom static CDS archive
#    make runs.ml
#
# Run with mainline, using AOT cache (JEP 483)
#    make runa.ml
#
# Run with leyden, using AOT cache (JEP 483 + all new Leyden optimizations)
#    make runa

# Set the following to point to youd "Build JDK", your JDK mainline build, and your Leyden JDK build.
#
#  *** NOTE: JDK 21 (or 17) is needed to build spring-petclinic-3.2.0-SNAPSHOT.jar
BLDJDK_HOME   = /work/official/jdk21

# Points to your build with https://github.com/openjdk/jdk
# For comparison purposes, this build should be the latest version of the mainline that
# has been merged into https://github.com/openjdk/leyden/tree/premain
MAINLINE_HOME = /work/official/jdk24

# Points to your build with https://github.com/openjdk/leyden/tree/premain
PREMAIN_HOME = /work/bld/le4/images/jdk
#===============================================================================

LOG_INIT = -Xlog:init

# Options passed to PREMAIN_JAVA. See compare_premain_builds below
PM_OPTS =

# Usually there's no need to change the following
JAR_CMD          = ${BLDJDK_HOME}/bin/jar
JAVAC_CMD        = ${BLDJDK_HOME}/bin/javac
BLDJDK_JAVA      = ${BLDJDK_HOME}/bin/java
MAINLINE_JAVA    = ${MAINLINE_HOME}/bin/java
PREMAIN_JAVA     = ${PREMAIN_HOME}/bin/java ${PM_OPTS}
PC_REPO          = petclinic-snapshot
PC_JAVA_SOURCES = ${PC_REPO}/src/main/java/org/springframework/samples/petclinic/PetClinicApplication.java
PC_APP_JAR       = ${PC_REPO}/target/spring-petclinic-3.2.0-SNAPSHOT.jar
PC_APP_UNPACKED  = ${PC_REPO}/target/unpacked

# This is for uploading to artifactory, to be tested with
# ../../runtime/cds/appcds/leyden/SpringPetClinic.java
PC_APP_UNPACKED_ZIP = ${PC_REPO}/target/spring-petclinic-3.2.0.zip

PC_MAIN_CLASS    = org.springframework.samples.petclinic.PetClinicApplication

# Note: you can add specify PM_VER (PreMain Version) for comparing the performance of different builds of premain.
# See the compare_premain_builds target below

PC_ML_AOTCONF    = spring-petclinic$(PM_VER).ml.aotconf
PC_ML_AOT        = spring-petclinic$(PM_VER).ml.aot

PC_AOTCONF       = spring-petclinic$(PM_VER).aotconf
PC_AOT           = spring-petclinic$(PM_VER).aot

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
DUMMY_CMDLINE    = $(HEAP_SIZE) -cp @${PC_APP_UNPACKED}/classpath.dummy Dummy

all: check git app aot

check:
	@if ${PREMAIN_JAVA} -XX:+PrintFlagsFinal --version | grep -q CDSManualFinalImage; then \
	    true; \
	else \
	    echo PREMAIN_HOME should point to your build of https://github.com/openjdk/leyden/tree/premain; \
	    exit 1; \
	fi

# A bunch of quick targets so you can type "make list" to create the classlist, etc
git: ${PC_JAVA_SOURCES}
app: ${PC_APP_JAR}
unpack: ${PC_APP_UNPACKED}


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
	cd ${PC_REPO}; JAVA_HOME=${BLDJDK_HOME} mvn -Dmaven.test.skip=true package
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
	echo $$(cat ${PC_APP_UNPACKED}/classpath):Dummy.jar > ${PC_APP_UNPACKED}/classpath.dummy
	${JAR_CMD} cf0 ${PC_APP_UNPACKED_ZIP} ${PC_APP_UNPACKED}
	$(MAKE) runb



# run mainline, default CDS archive only
run0.ml: ${PC_APP_UNPACKED}
	${MAINLINE_JAVA} ${LOG_INIT} ${UNPACKED_CMDLINE}

# Run using "build JDK"
runb: ${PC_APP_UNPACKED}
	${BLDJDK_JAVA} ${UNPACKED_CMDLINE}

# Running with mainline
PC_CLASSLIST_MAINLINE   = spring-petclinic.ml.classlist
PC_STATIC_JSA_MAINLINE  = spring-petclinic.static.ml.jsa

list.ml:   ${PC_CLASSLIST_MAINLINE}

static.ml: ${PC_STATIC_JSA_MAINLINE}

${PC_CLASSLIST_MAINLINE}: ${PC_APP_UNPACKED}
	${MAINLINE_JAVA} -Xshare:off -XX:DumpLoadedClassList=${PC_CLASSLIST_MAINLINE} \
	    -Xlog:class+load=debug:file=spring-petclinic.classload.ml.log \
	    ${UNPACKED_CMDLINE}
	wc -lc ${PC_CLASSLIST_MAINLINE}

${PC_STATIC_JSA_MAINLINE}: ${PC_CLASSLIST_MAINLINE}
	rm -f ${PC_STATIC_JSA_MAINLINE}.log
	${MAINLINE_JAVA} -Xshare:dump -XX:SharedClassListFile=${PC_CLASSLIST_MAINLINE} $(HEAP_SIZE) -cp @${PC_APP_UNPACKED}/classpath \
	    -XX:SharedArchiveFile=${PC_STATIC_JSA_MAINLINE} \
	    -Xlog:cds=debug,cds+class=debug,cds+heap=warning:file=${PC_STATIC_JSA_MAINLINE}.log
	ls -l ${PC_STATIC_JSA_MAINLINE}

runs.ml: ${PC_STATIC_JSA_MAINLINE}
	${MAINLINE_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA_MAINLINE} \
		${UNPACKED_CMDLINE}

clean.ml:
	rm -fv ${PC_CLASSLIST_MAINLINE}* ${PC_STATIC_JSA_MAINLINE}*

#======================================== JDK 483 (mainline)

aotconf.ml:   ${PC_ML_AOTCONF}
aot.ml:       ${PC_ML_AOT}

${PC_ML_AOTCONF}: ${PC_APP_UNPACKED}
	rm -f ${PC_ML_AOTCONF}.log
	${MAINLINE_JAVA} -XX:AOTMode=record -XX:AOTConfiguration=${PC_ML_AOTCONF} \
	    -Xlog:class+load=debug:file=${PC_ML_AOTCONF}.log \
	    ${UNPACKED_CMDLINE}
	ls -l ${PC_ML_AOTCONF}

${PC_ML_AOT}: ${PC_ML_AOTCONF}
	rm -f ${PC_ML_AOT}.log
	${MAINLINE_JAVA} -XX:AOTMode=create -XX:AOTConfiguration=${PC_ML_AOTCONF} $(HEAP_SIZE) -cp @${PC_APP_UNPACKED}/classpath \
	    -XX:AOTCache=${PC_ML_AOT} \
	    -Xlog:cds=debug,cds+class=debug,cds+heap=warning:file=${PC_ML_AOT}.log
	ls -l ${PC_ML_AOT}

runa.ml: ${PC_ML_AOT}
	${MAINLINE_JAVA} -XX:AOTMode=on -XX:AOTCache=${PC_ML_AOT} \
		${UNPACKED_CMDLINE}

#======================================== JDK 483 (Leyden -- includes AOT profile/compiled code)
aotconf:   ${PC_AOTCONF}
aot:       ${PC_AOT}

${PC_AOTCONF}: ${PC_APP_UNPACKED}
	rm -f ${PC_AOTCONF}.log
	${PREMAIN_JAVA} -XX:AOTMode=record -XX:AOTConfiguration=${PC_AOTCONF} \
	    -Xlog:class+load=debug:file=${PC_AOTCONF}.log \
	    ${UNPACKED_CMDLINE}
	ls -l ${PC_AOTCONF}

${PC_AOT}: ${PC_AOTCONF}
	rm -f ${PC_AOT}.log
	${PREMAIN_JAVA} -XX:AOTMode=create -XX:AOTConfiguration=${PC_AOTCONF} $(HEAP_SIZE) -cp @${PC_APP_UNPACKED}/classpath \
	    -XX:AOTCache=${PC_AOT} \
	    -Xlog:cds=debug,cds+class=debug,cds+heap=warning:file=${PC_AOT}.log
	ls -l ${PC_AOT}

runa: ${PC_AOT}
	${PREMAIN_JAVA} -XX:AOTMode=on -XX:AOTCache=${PC_AOT} \
		${UNPACKED_CMDLINE}

#========================================

clean: clean0
	rm -rf *~ ${PC_REPO} Dummy*.class Dummy.jar *.old.txt *.new.txt tmp.txt hs_err*

# clean the the leyden artifacts
clean0:
	rm -f spring-petclinic.* *.aot *.aotconf

# clean the the leyden artifacts for the new workflow only
cleann:
	rm -f spring-petclinic.cds.* *.aot *.aotconf

include ../lib/Bench.gmk

# Clean up the logs generated by compare_premain_builds, so you can see if one build includes
# more classes than another.
diff_training_logs:
	@for i in old new; do \
	    grep cds,class spring-petclinic.$$i.static.jsa.log | \
		sed -e 's/.*0x[0-9a-f]* ..... //g' \
		    -e 's/Lambda\/0x[0-9a-f]*/Lambda\/nnn/g' \
		    -e 's/MH\/0x[0-9a-f]*/MH\/nnn/g' | \
		sort > old-workflow-static-classes.$$i.txt; \
	    grep cds,class spring-petclinic.$$i.dynamic.jsa.log | \
		sed -e 's/.*0x[0-9a-f]* ..... //g' \
		    -e 's/Lambda\/0x[0-9a-f]*/Lambda\/nnn/g' \
		    -e 's/MH\/0x[0-9a-f]*/MH\/nnn/g' | \
		sort > old-workflow-dynamic-classes.$$i.txt; \
	    grep cds,class spring-petclinic.$$i.cds.log | \
		sed -e 's/.*0x[0-9a-f]* ..... //g' \
		    -e 's/Lambda\/0x[0-9a-f]*/Lambda\/nnn/g' \
		    -e 's/MH\/0x[0-9a-f]*/MH\/nnn/g' | \
		sort > new-workflow-classes.$$i.txt; \
	done
	@ls -l *.old.txt *.new.txt
	@echo '***'
	@echo "You can diff the above files to diagnose speed differences between two builds"


