# Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

# Make a digest of bench.20250930.txt, etc, to be cut-and-pasted into ../../../../../README.md
#
# Example:
# tclsh digest.tcl bench.20250930.txt

set group {
    helidon-quickstart-se "Helidon Quick Start"
    javac_bench "JavacBenchApp 50 source files"
    micronaut-first-app "Micronaut First App Demo"
    quarkus-getting-started "Quarkus Getting Started Demo"
    spring-boot-getting-started "Spring-boot Getting Started Demo"
    spring-petclinic "Spring PetClinic Demo"
}

set file [lindex $argv 0]
set fd [open $file]
set data [read $fd]
seek $fd 0

if {![regexp {built from https://github.com/openjdk/leyden/commit/([0-9a-z]+)} $data dummy version]} {
    puts "Error: cannot find version"
    exit 1
}

puts "- Leyden: https://github.com/openjdk/leyden/tree/$version"
puts ""
puts "For details information about the hardware and raw numbers, see \[$file\](test/hotspot/jtreg/premain/bench_data/$file)"

set speed ""
set i 0
while {![eof $fd]} {
    set line [gets $fd]
    if {[regexp {([0-9]+.[0-9]+x) improvement} $line dummy x]} {
        set speed $x
    } elseif {[regexp {[-]Normalized-----} $line]} {
        set data ""

        while {![eof $fd]} {
            set line [gets $fd]
            if {[regexp {[-]--------------------} $line]} {
                break
            } else {
                append data $line\n
            }
        }        
        set name [lindex $group [expr $i * 2 + 1]]
        puts "\n### $name ($speed improvement)\n"
        puts [string trim $data]
        set speed ""
        incr i
    }
}
