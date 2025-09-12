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
DEMO_NAME       = javac-bench
DEMO_SRC        = ../../../../setup_aot/JavacBenchApp.java
DEMO_JAR        = JavacBenchApp.jar
DEMO_CMDLINE    = -DJavacBenchApp.leyden.bench=1 -cp ${DEMO_JAR} JavacBenchApp 50

include ../lib/DemoSupport.gmk

${DEMO_ARTIFACT}: ${DEMO_JAR}

${DEMO_JAR}: ${DEMO_SRC}
	${JAVAC} -d . ${DEMO_SRC}
	${JAR} cvf JavacBenchApp.jar JavacBenchApp*.class

clean0:
	rm -f  *.class *.jar tmp.txt

clean: clean0
	rm -f *.cds *.cds.code *.classlist *.jsa *~ *.log *.log.* hs_err_* *.aot *.aotconf

