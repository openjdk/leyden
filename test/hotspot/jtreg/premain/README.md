# Demos and Documentation for Leyden-premain Protype


## Demos

- [simple](simple) script to demonstrate the "5 step" process of training/production run with leyden-premain prototype

- [jmh](jmh) run JMH + specjbb2005-jmh-1.25.jar

- [javac](javac) using javac as a regression test, to compile up to 10000 Java source files

- [javac_helloworld](javac_helloworld) measures the total elapsed time of `javac HelloWorld.java` using
the premain branch vs the JDK mainline.
It's a good demonstration of how we can improve start-up time of a complex application.

- [javac_new_workflow](javac_new_workflow) Example of the new "one step training" workflow
(still under development) where you can generate all the Leyden artifacts with a single
JVM invocation.

## Docs

- [InvokeDynamic.md](InvokeDynamic.md) CDS optimizations for invokedynamic

## Benchmarking Against JDK Mainline

We have included some demo application for some popular Java application
frameworks, so you can compare the performance of Leyden vs the mainline JDK.

- [helidon-quickstart-se](helidon-quickstart-se): from https://helidon.io/docs/v4/se/guides/quickstart
- [micronaut-first-app](micronaut-first-app): from https://guides.micronaut.io/latest/creating-your-first-micronaut-app-maven-java.html
- [quarkus-getting-started](quarkus-getting-started): from https://quarkus.io/guides/getting-started
- [spring-petclinic](spring-petclinic): from https://github.com/spring-projects/spring-petclinic

You need:

- An official build of JDK 21
- An up-to-date build of the JDK mainline
- The latest Leyden build
- Maven (ideally 3.8 or later, as required by some of the demos). Note: if you are behind
  a firewall, you may need to [set up proxies for Maven](https://maven.apache.org/guides/mini/guide-proxies.html)

The steps for benchmarking is the same for the above four demos. For example:

```
$ cd helidon-quickstart-se
$ make PREMAIN_HOME=/repos/leyden/build/linux-x64/images/jdk \
       MAINLINE_HOME=/repos/jdk/build/linux-x64/images/jdk \
       BLDJDK_HOME=/usr/local/jdk21 \
       bench
run,mainline default,mainline custom static CDS,premain custom static CDS only,premain CDS + AOT
1,398,244,144,107
2,387,247,142,108
3,428,238,143,107
4,391,252,142,111
5,417,247,141,107
6,390,239,139,127
7,387,247,145,111
8,387,240,147,110
9,388,242,147,108
10,400,242,167,108
Geomean,397.08,243.76,145.52,110.26
Stdev,13.55,4.19,7.50,5.73
Markdown snippets in mainline_vs_premain.md
```

The above command runs each configuration 10 times, in an interleaving order. This way
the noise of the system (background processes, thermo throttling, etc) is more likely to
be spread across the different runs.

As typical for start-up benchmarking, the numbers are not very steady. You should plot
the results (saved in the file mainline_vs_premain.csv) in a spreadsheet to check for noise, etc.

The "make bench" target also generates GitHub markdown snippets (in the file mainline_vs_premain.md) for creating the
graphs below.

## Benchmarking Between Two Leyden Builds

This is useful for Leyden developers to measure the benefits of a particular optimization.
The steps are similar to above, but we use the "make compare_premain_builds" target:

```
$ cd helidon-quickstart-se
$ make PM_OLD=/repos/leyden_old/build/linux-x64/images/jdk \
       PM_NEW=/repos/leyden_new/build/linux-x64/images/jdk \
       BLDJDK_HOME=/usr/local/jdk21 \
       compare_premain_builds
Old build = /repos/leyden_old/build/linux-x64/images/jdk with options
New build = /repos/leyden_new/build/linux-x64/images/jdk with options
Run,Old CDS + AOT,New CDS + AOT
1,110,109
2,131,111
3,118,115
4,110,108
5,117,110
6,114,109
7,110,109
8,118,110
9,110,110
10,113,114
Geomean,114.94,110.48
Stdev,6.19,2.16
Markdown snippets in compare_premain_builds.md
```

Please see [lib/Bench.gmk](lib/Bench.gmk) for more details.

Note: due to the variability of start-up time, the benefit of minor improvements may
be difficult to measure.

## Preliminary Benchmark Results

The following charts show the relative start-up performance of the Leyden/Premain branch vs
the JDK mainline.

For example, a number of "premain CDS + AOT : 291" indicates that if the application takes
1000 ms to start-up with the JDK mainline, it takes only 291 ms to start up when all the
current set of Leyden optimizations for CDS and AOT are enabled.

The benchmark results are collected with `make bench`in the following directories:

- helidon-quickstart-se
- micronaut-first-app
- quarkus-getting-started
- spring-petclinic

The version used in the comparisons:

- JDK Mainline: https://github.com/openjdk/jdk/commit/70944ca54ad0090c734bb5b3082beb33450c4877
- Leyden: https://github.com/openjdk/leyden/commit/9fa972214934d30f67db5fd4d1b8007636ac1428

The benchmarks were executed on an 8-core Intel i7-10700 CPU @ 2.90GHz with 32GB RAM running Ubuntu 22.04.3 LTS.

### Helidon Quick Start (SE) Demo

```mermaid
gantt
    title Elapsed time (normalized, smaller is better)
    todayMarker off
    dateFormat  X
    axisFormat %s

    mainline default   : 0, 1000
    mainline custom static CDS   : 0, 632
    premain custom static CDS only   : 0, 376
    premain CDS + AOT   : 0, 291
```

### Micronaut First App Demo

```mermaid
gantt
    title Elapsed time (normalized, smaller is better)
    todayMarker off
    dateFormat  X
    axisFormat %s

    mainline default   : 0, 1000
    mainline custom static CDS   : 0, 558
    premain custom static CDS only   : 0, 410
    premain CDS + AOT   : 0, 353
```


### Quarkus Getting Started Demo

```mermaid
gantt
    title Elapsed time (normalized, smaller is better)
    todayMarker off
    dateFormat  X
    axisFormat %s

    mainline default   : 0, 1000
    mainline custom static CDS   : 0, 568
    premain custom static CDS only   : 0, 395
    premain CDS + AOT   : 0, 317
```

### Spring PetClinic Demo

```mermaid
gantt
    title Elapsed time (normalized, smaller is better)
    todayMarker off
    dateFormat  X
    axisFormat %s

    mainline default   : 0, 1000
    mainline custom static CDS   : 0, 695
    premain custom static CDS only   : 0, 563
    premain CDS + AOT   : 0, 368
```

## Regression Testing

Leyden-specific tests have been added to the following directories in the repo:

- [test/hotspot/jtreg/runtime/cds/appcds/applications](../runtime/cds/appcds/applications)
- [test/hotspot/jtreg/runtime/cds/appcds/indy](../runtime/cds/appcds/indy)
- [test/hotspot/jtreg/runtime/cds/appcds/leyden](../runtime/cds/appcds/leyden)
- [test/hotspot/jtreg/runtime/cds/appcds/preloadedClasses](../runtime/cds/appcds/preloadedClasses)

These test cases can be executed using jtreg. Some of the tests (in the applications directories)
require binaries to be built separately. Please refer to the script [lib/build-for-jtreg.sh](lib/build-for-jtreg.sh)
