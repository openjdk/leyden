We use a small set of benchmarks to demonstrate the performance of the optimizations in the Leyden repo.

| Benchmark  | Source |
| ------------- | ------------- |
|[helidon-quickstart-se](test/hotspot/jtreg/premain/helidon-quickstart-se) | https://helidon.io/docs/v4/se/guides/quickstart|
|[javac-bench](test/hotspot/jtreg/premain/javac_bench) | Using Javac to compile 50 source files |
|[micronaut-first-app](test/hotspot/jtreg/premain/micronaut-first-app) | https://guides.micronaut.io/latest/creating-your-first-micronaut-app-maven-java.html|
|[quarkus-getting-started](test/hotspot/jtreg/premain/quarkus-getting-started) | https://quarkus.io/guides/getting-started|
|[spring-boot-getting-started](test/hotspot/jtreg/premain/spring-boot-getting-started) | https://spring.io/guides/gs/spring-boot|
|[spring-petclinic](test/hotspot/jtreg/premain/spring-petclinic) | https://github.com/spring-projects/spring-petclinic|

### Benchmarking Against JDK Main-line

To can compare the performance of Leyden vs the main-line JDK, you need:

- An official build of JDK 21
- An up-to-date build of the JDK main-line
- The latest Leyden build
- Maven (ideally 3.8 or later, as required by some of the demos). Note: if you are behind
  a firewall, you may need to [set up proxies for Maven](https://maven.apache.org/guides/mini/guide-proxies.html)

The same steps are used for benchmarking all of the above demos. For example:

```
$ cd test/hotspot/jtreg/premain/helidon-quickstart-se
$ make PREMAIN_HOME=/repos/leyden/build/linux-x64/images/jdk \
       MAINLINE_HOME=/repos/jdk/build/linux-x64/images/jdk \
       BLDJDK_HOME=/usr/local/jdk21 \
       bench
run,mainline default,mainline custom static cds,mainline aot cache,premain aot cache
1,456,229,156,117
2,453,227,157,117
3,455,232,155,116
4,448,230,154,114
5,440,228,156,114
6,446,228,156,114
7,448,232,156,114
8,465,261,159,114
9,448,226,157,113
10,442,233,154,114
Geomean,450.05,232.41,155.99,114.69
Stdev,6.98,9.72,1.41,1.35
Markdown snippets in mainline_vs_premain.md
```

The above command runs each configuration 10 times, in an interleaving order. This way
the noise of the system (background processes, thermo throttling, etc) is more likely to
be spread across the different runs.

As is typical for benchmarking start-up performance, the numbers are not very steady.
It is best to plot
the results (as saved in the file `mainline_vs_premain.csv`) in a spreadsheet to check for
noise and other artifacts.

The "make bench" target also generates GitHub markdown snippets (in the file `mainline_vs_premain.md`) for creating the
graphs below.

### Benchmarking Between Two Leyden Builds

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

Please see [test/hotspot/jtreg/premain/lib/Bench.gmk](test/hotspot/jtreg/premain/lib/Bench.gmk) for more details.

Note: due to the variability of start-up time, the benefit of minor improvements may
be difficult to measure.

### Preliminary Benchmark Results

The following charts show the relative start-up performance of the Leyden/Premain branch vs
the JDK main-line.

For example, a number of "premain aot cache: 255" indicates that if the application takes
1000 ms to start-up with the JDK main-line, it takes only 255 ms to start up when all the
current set of Leyden optimizations are enabled.

The benchmark results are collected with `make bench` in the following directories under [test/hotspot/jtreg/premain](test/hotspot/jtreg/premain):

- `helidon-quickstart-se`
- `javac-bench`
- `micronaut-first-app`
- `quarkus-getting-started`
- `spring-boot-getting-started`
- `spring-petclinic`

The meaning of the four rows in the following charts:

| Row  | Meaning |
| ------------- | ------------- |
| **mainline default**            |Run benchmark with no optimizations|
| **mainline custom static cds**  |Run benchmark with a custom static CDS archive|
| **mainline aot cache**          |Run benchmark with a custom AOT cache (JDK mainline)|
| **premain aot cache**           |Run benchmark with a custom AOT cache (Leyden Premain Prototype)|

We have benchmark results from two types of configurations using the
script [test/hotspot/jtreg/premain/bench_data/do_bench.sh](test/hotspot/jtreg/premain/bench_data/do_bench.sh):

- Desktop/Server Class: these are the results when running on a modern desktop or server, using the
  command `bash bench_data/do_bench.sh`.
- 2 Cores Only: these are the results when running in a limited configuration where only two cores.
  are available, using the command `taskset -c 1,2 bash bench_data/do_bench.sh`

The 2 Cores Only setting is intended to emulate microservice configurations where a very small number
of cores are allocated for small Java programs. In this setting, the JIT compiler may compete for CPU
with the Java program, making start-up slower. The **premain aot cache** numbers usually are much better in
this setting because most of the start-up code has been AOT-compiled, so the app can spend most of the
available CPUs to execute application logic.

These JDK versions were used in the comparisons:

- JDK main-line: JDK 25, build 25+37-LTS-3491
- Leyden: https://github.com/openjdk/leyden/tree/ce150637130086ad2b47916d66148007f5331a28

For details information about the hardware and raw numbers, see [bench.20250930.txt](test/hotspot/jtreg/premain/bench_data/bench.20250930.txt)
 and [bench.20250930-2cpu.txt](test/hotspot/jtreg/premain/bench_data/bench.20250930-2cpu.txt)

#### Premain AOT Cache Summary

This is the speed up of **premain aot cache** vs **mainline default** in the two types of configurations

| Benchmark | Desktop/Server Class (28 Cores) | 2 Cores Only|
|:-------------|-------------:| -------------:|
| Helidon Quick Start | 3.59x | 4.11x |
| JavacBenchApp 50 source files | 2.21x | 3.17x |
| Micronaut First App Demo | 2.91x | 4.90x |
| Quarkus Getting Started Demo | 2.97x | 3.74x |
| Spring-boot Getting Started Demo | 4.13x | 4.70x |
| Spring PetClinic Demo | 3.33x | 3.03x |

### 5.1 Benchmark Results - Desktop/Server Class (28 Cores)

#### Helidon Quick Start (3.59x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 520, 350, 279]
```

#### JavacBenchApp 50 source files (2.21x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 785, 572, 452]
```

#### Micronaut First App Demo (2.91x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 482, 387, 344]
```

#### Quarkus Getting Started Demo (2.97x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 499, 417, 337]
```

#### Spring-boot Getting Started Demo (4.13x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 492, 332, 242]
```

#### Spring PetClinic Demo (3.33x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 619, 568, 301]
```
### 5.2 Benchmark Results - 2 Cores Only

#### Helidon Quick Start (4.11x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 585, 459, 244]
```

#### JavacBenchApp 50 source files (3.17x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 845, 674, 315]
```

#### Micronaut First App Demo (4.90x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 439, 355, 204]
```

#### Quarkus Getting Started Demo (3.74x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 512, 495, 268]
```

#### Spring-boot Getting Started Demo (4.70x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 607, 518, 213]
```

#### Spring PetClinic Demo (3.03x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 632, 572, 330]
```
