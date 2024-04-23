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

# Set the following to point to JDK 17, your JDK mainline build, and your Leyden JDK build.
#
#  *** NOTE: JDK 17 (or 21??) is needed to build spring-petclinic-3.2.0-SNAPSHOT.jar
JDK17_HOME   = /jdk3/official/jdk17

# Points to your build with https://github.com/openjdk/jdk
# For comparison purposes, this build should be the latest version of the mainline that
# has been merged into https://github.com/openjdk/leyden/tree/premain
MAINLINE_HOME = /jdk3/bld/rum/images/jdk

# Points to your build with https://github.com/openjdk/leyden/tree/premain
PREMAIN_HOME = /jdk3/bld/le4/images/jdk
#===============================================================================

LOG_INIT = -Xlog:init

# Options passed to PREMAIN_JAVA. See compare_premain_builds below
PM_OPTS =

# Usually there's no need to change the following
JAR_CMD          = ${JDK17_HOME}/bin/jar
JAVAC_CMD        = ${JDK17_HOME}/bin/javac
JDK17_JAVA       = ${JDK17_HOME}/bin/java
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

# old workflow
PC_CLASSLIST     = spring-petclinic$(PM_VER).classlist
PC_STATIC_JSA    = spring-petclinic$(PM_VER).static.jsa
PC_DYNAMIC_JSA   = spring-petclinic$(PM_VER).dynamic.jsa
PC_CACHED_CODE   = spring-petclinic$(PM_VER).code.jsa

# new workflow
PC_CDS_PREIMAGE  = spring-petclinic$(PM_VER).cds.preimage
PC_CDS           = spring-petclinic$(PM_VER).cds

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
	echo $$(cat ${PC_APP_UNPACKED}/classpath):Dummy.jar > ${PC_APP_UNPACKED}/classpath.dummy
	${JAR_CMD} cf0 ${PC_APP_UNPACKED_ZIP} ${PC_APP_UNPACKED}
	$(MAKE) run17

#uncomment this to see the generated classes for dynamic proxies
#SAVE_GEN_FILES=-Djdk.proxy.ProxyGenerator.saveGeneratedFiles=true

${PC_CLASSLIST}: ${PC_APP_UNPACKED}
	${PREMAIN_JAVA} -Xshare:off -XX:DumpLoadedClassList=${PC_CLASSLIST} \
	    -Xlog:class+load=debug:file=${PC_CLASSLIST}.classload.log ${SAVE_GEN_FILES} \
	    ${UNPACKED_CMDLINE}
	wc -lc ${PC_CLASSLIST}

${PC_STATIC_JSA}: ${PC_CLASSLIST}
	rm -f ${PC_STATIC_JSA}.log
	${PREMAIN_JAVA} -Xshare:dump -XX:SharedClassListFile=${PC_CLASSLIST} $(HEAP_SIZE) -cp @${PC_APP_UNPACKED}/classpath \
	    -XX:SharedArchiveFile=${PC_STATIC_JSA} \
	    -XX:+PreloadSharedClasses -XX:+ArchiveInvokeDynamic -XX:+ArchiveDynamicProxies -XX:+ArchiveReflectionData \
	    -XX:+ArchiveLoaderLookupCache -Dleyden.debug.archived.packages=1 \
	    -Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=${PC_STATIC_JSA}.log
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
	${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_DYNAMIC_JSA} -XX:+ReplayTraining -XX:+LoadCachedCode \
		-XX:CachedCodeFile=${PC_CACHED_CODE} ${LOG_INIT} -Xlog:scc=error ${UNPACKED_CMDLINE}

# run with just static CDS archive
runs: ${PC_STATIC_JSA}
	${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA} ${LOG_INIT} ${UNPACKED_CMDLINE}

# run with just static CDS archive, but instead of the main class, give a fake class name
# Because -XX:+LoadSharedClasses is specified, this command will basically test the speed of loading all
# the classes and entering the main() method.
dummy: ${PC_STATIC_JSA} Dummy.jar
	${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA} ${LOG_INIT} ${DUMMY_CMDLINE}

dummy-50: ${PC_STATIC_JSA} Dummy.jar
	bash -c "time for i in {1..50}; do ${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA} ${DUMMY_CMDLINE}; done"

dummy-50-perf: ${PC_STATIC_JSA} Dummy.jar
	perf stat -r 50 ${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA} ${DUMMY_CMDLINE}

Dummy.jar: Dummy.java
	${JAVAC_CMD} Dummy.java
	${JAR_CMD} cf Dummy.jar Dummy*.class

# run with just dynamic CDS archive
rund: ${PC_DYNAMIC_JSA}
	${PREMAIN_JAVA} -XX:SharedArchiveFile=${PC_DYNAMIC_JSA} \
		${LOG_INIT} ${UNPACKED_CMDLINE}

# run WITHOUT premain optimizatipn
run0: ${PC_APP_UNPACKED}
	${PREMAIN_JAVA} ${LOG_INIT} ${UNPACKED_CMDLINE}

run17: ${PC_APP_UNPACKED}
	${JDK17_JAVA} ${UNPACKED_CMDLINE}

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

run0.ml: ${PC_STATIC_JSA_MAINLINE}
	${MAINLINE_JAVA} \
		${UNPACKED_CMDLINE}

runs.ml: ${PC_STATIC_JSA_MAINLINE}
	${MAINLINE_JAVA} -XX:SharedArchiveFile=${PC_STATIC_JSA_MAINLINE} \
		${UNPACKED_CMDLINE}

clean.ml:
	rm -fv ${PC_CLASSLIST_MAINLINE}* ${PC_STATIC_JSA_MAINLINE}*

#======================================== new workflow
pre: ${PC_CDS_PREIMAGE}
cds: ${PC_CDS}

${PC_CDS_PREIMAGE}: ${PC_APP_UNPACKED}
	rm -f ${PC_CDS_PREIMAGE} ${PC_CDS}
	${PREMAIN_JAVA} -XX:+PreloadSharedClasses \
	    -XX:+ArchiveInvokeDynamic -XX:+ArchiveReflectionData \
	    -XX:+ArchiveDynamicProxies -XX:+ArchiveLoaderLookupCache -XX:CachedCodeMaxSize=512M \
	    -XX:+UnlockDiagnosticVMOptions -XX:+CDSManualFinalImage -XX:CacheDataStore=${PC_CDS} \
	    -Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=${PC_CDS_PREIMAGE}.log \
	    ${UNPACKED_CMDLINE}
	ls -l ${PC_CDS_PREIMAGE}

${PC_CDS}: ${PC_CDS_PREIMAGE}
	rm -f ${PC_CDS}
	${PREMAIN_JAVA} -XX:+PreloadSharedClasses -XX:+ArchiveInvokeDynamic -XX:CachedCodeMaxSize=512M \
	    -XX:+UnlockDiagnosticVMOptions -XX:CDSPreimage=${PC_CDS_PREIMAGE} -XX:CacheDataStore=${PC_CDS} \
	    -Xlog:scc,cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=${PC_CDS}.log \
	    -Dleyden.debug.archived.packages=1 \
	    ${UNPACKED_CMDLINE}
	ls -l ${PC_CDS}
	ls -l ${PC_CDS}.code

# run with NEW workflow
runn: ${PC_CDS}
	${PREMAIN_JAVA} -XX:CacheDataStore=${PC_CDS} ${LOG_INIT} -Xlog:scc=error ${UNPACKED_CMDLINE}

# see dummy above
dummyn: ${PC_CDS} Dummy.jar
	${PREMAIN_JAVA} -XX:CacheDataStore=${PC_CDS} ${LOG_INIT} -Xlog:scc=error ${DUMMY_CMDLINE}

dummyn-50: ${PC_CDS} Dummy.jar
	bash -c "time for i in {1..50}; do ${PREMAIN_JAVA} -XX:CacheDataStore=${PC_CDS} \
		-XX:-ReplayTraining -XX:-LoadCachedCode -Xlog:scc=error ${DUMMY_CMDLINE}; done"

dummyn-50-perf: ${PC_CDS} Dummy.jar
	perf stat -r 50 ${PREMAIN_JAVA} -XX:CacheDataStore=${PC_CDS} -XX:-ReplayTraining -XX:-LoadCachedCode -Xlog:scc=error ${DUMMY_CMDLINE}

clean: clean0
	rm -rf *~ ${PC_REPO} Dummy*.class Dummy.jar *.old.txt *.new.txt tmp.txt hs_err*

# clean the the leyden artifacts
clean0:
	rm -fv spring-petclinic.*

# clean the the leyden artifacts for the new workflow only
cleann:
	rm -fv spring-petclinic.cds.*

# You can choose a different number of loops like this:
#     make compare_premain_builds LOOPS="0 1 2" PM_OLD=.....
LOOPS = 0 1 2 3 4 5 6 7 8 9 10

SLEEP = sleep 1;

bench: mainline_vs_premain

mainline_vs_premain: clean0 _mainline_vs_premain

_mainline_vs_premain:
	rm -f mainline_vs_premain.csv
	@echo "run,mainline default,mainline custom static CDS,premain custom static CDS only,premain CDS + AOT" | tee -a mainline_vs_premain.csv
	@first=1; for i in ${LOOPS}; do \
		rm -f tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= run0.ml | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= runs.ml | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= runs    | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= runn    | grep Booted >> tmp.txt; \
		if test "$$first" != "1"; then \
		    echo $$i$$(cat tmp.txt | sed -e 's/.*in /,/g' -e 's/ms//g') | sed -e 's/ //g' | tee -a mainline_vs_premain.csv; \
	        fi; \
		first=0; \
	done
	echo ========================================
	${MAINLINE_JAVA} ../lib/GithubMDChart.java < mainline_vs_premain.csv

# Same as mainline_vs_premain, but also includes old workflow
mainline_vs_premain_all: clean0 _mainline_vs_premain_all

_mainline_vs_premain_all:
	@echo "run,mainline default,mainline custom static CDS,premain custom static CDS,premain custom static CDS+training+AOT,premain new workflow"
	@first=1; for i in ${LOOPS}; do \
		rm -f tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= run0.ml | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= runs.ml | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= runs    | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= run     | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= runn    | grep Booted >> tmp.txt; \
		if test "$$first" != "1"; then \
		    echo $$i$$(cat tmp.txt | sed -e 's/.*in /,/g' -e 's/ms//g') | sed -e 's/ //g'; \
	        fi; \
		first=0; \
	done

# Do something like this to compare two different builds:
#
# make compare_premain_builds \
#    PM_OLD=/repo/old/build/linux-x64/images/jdk \
#    PM_NEW=/repo/new/build/linux-x64/images/jdk \
#    JDK17_HOME=/my/jdk/17/home
#
# or do this to compare the same build with different options:
#
# make compare_premain_builds \
#    PM_OLD=/repo/leyden/build/linux-x64/images/jdk PM_OPTS_NEW=-XX:-ArchivePackages \
#    PM_NEW=/repo/leyden/build/linux-x64/images/jdk PM_OPTS_NEW=-XX:+ArchivePackages \
#    JDK17_HOME=/my/jdk/17/home

compare_premain_builds: clean0 _compare_premain_builds

_compare_premain_builds:
	@echo Old build = ${PM_OLD} with options ${PM_OPTS_OLD}
	@echo New build = ${PM_NEW} with options ${PM_OPTS_NEW}
	@echo "Run,Old static CDS only,New static CDS only,Old static CDS + training + AOT,New static CDS + training + AOT,Old 1-step training,New 1-step training"
	@first=1; for i in ${LOOPS}; do \
		rm -f tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_OLD} PM_VER=.old PM_OPTS=${PM_OPTS_OLD} runs | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_NEW} PM_VER=.new PM_OPTS=${PM_OPTS_NEW} runs | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_OLD} PM_VER=.old PM_OPTS=${PM_OPTS_OLD} run  | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_NEW} PM_VER=.new PM_OPTS=${PM_OPTS_NEW} run  | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_OLD} PM_VER=.old PM_OPTS=${PM_OPTS_OLD} runn | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_NEW} PM_VER=.new PM_OPTS=${PM_OPTS_NEW} runn | grep Booted >> tmp.txt; \
		if test "$$first" != "1"; then \
		    echo $$i$$(cat tmp.txt | sed -e 's/.*in /,/g' -e 's/ms//g') | sed -e 's/ //g'; \
	        fi; \
		first=0; \
	done

# same as above, but only compare the speed of "make runs"
compare_premain_builds_static: clean0 _compare_premain_builds_static

_compare_premain_builds_static:
	@echo Old build = ${PM_OLD} with options ${PM_OPTS_OLD}
	@echo New build = ${PM_NEW} with options ${PM_OPTS_NEW}
	@echo "Run,Old static CDS only,New static CDS only"
	@first=1; for i in ${LOOPS}; do \
		rm -f tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_OLD} PM_VER=.old PM_OPTS=${PM_OPTS_OLD} runs | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_NEW} PM_VER=.new PM_OPTS=${PM_OPTS_NEW} runs | grep Booted >> tmp.txt; \
		if test "$$first" != "1"; then \
		    echo $$i$$(cat tmp.txt | sed -e 's/.*in /,/g' -e 's/ms//g') | sed -e 's/ //g'; \
	        fi; \
		first=0; \
	done

# same as above, but only compare the speed of "make runn"
compare_premain_builds_1step: clean0 _compare_premain_builds_1step

_compare_premain_builds_1step:
	@echo Old build = ${PM_OLD} with options ${PM_OPTS_OLD}
	@echo New build = ${PM_NEW} with options ${PM_OPTS_NEW}
	@echo "Run,Old 1-step training,New 1-step training"
	@first=1; for i in ${LOOPS}; do \
		rm -f tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_OLD} PM_VER=.old PM_OPTS=${PM_OPTS_OLD} runn | grep Booted >> tmp.txt; \
		$(SLEEP) $(MAKE) LOG_INIT= PREMAIN_HOME=${PM_NEW} PM_VER=.new PM_OPTS=${PM_OPTS_NEW} runn | grep Booted >> tmp.txt; \
		if test "$$first" != "1"; then \
		    echo $$i$$(cat tmp.txt | sed -e 's/.*in /,/g' -e 's/ms//g') | sed -e 's/ //g'; \
	        fi; \
		first=0; \
	done

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


