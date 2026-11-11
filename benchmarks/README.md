# Disclaimers

- *This repository contains experimental and unstable code. It is not intended to be used
   in a production environment.*
- *This repository is intended for developers of the JDK, and advanced Java developers who
   are familiar with building the JDK.*
- *The experimental features in this repository may be changed or removed without notice.
   Command line flags and workflows will change.*
- *The benchmarks results reported on this page are for illustrative purposes only. Your
   applications may get better or worse results.*

# Overview

We use a small set of benchmarks to demonstrate the performance of the optimizations in the Leyden repo.

| Benchmark  | Version | Source |
| ------------- | ------------- | ------------- |
|[helidon-quickstart-se](helidon-quickstart-se) | 4.0.7 | https://helidon.io/docs/v4/se/guides/quickstart|
|[javac-bench](javac_bench) | - | Using Javac to compile 50 source files |
|[micronaut-first-app](micronaut-first-app) | 4.4.0 | https://guides.micronaut.io/latest/creating-your-first-micronaut-app-maven-java.html|
|[quarkus-getting-started](quarkus-getting-started) | 1.0.0 | https://quarkus.io/guides/getting-started|
|[spring-boot-getting-started](spring-boot-getting-started) | [3.3.0](https://github.com/spring-guides/gs-spring-boot/tree/99085f078b01fed27667c0b8fbbb957758911f4a) | https://spring.io/guides/gs/spring-boot|
|[spring-petclinic](spring-petclinic) | [3.2.0](https://github.com/spring-projects/spring-petclinic/tree/80fd11067c4662486e4c635deceba927375b621c) | https://github.com/spring-projects/spring-petclinic|

# Benchmarking Against JDK Main-line

To can compare the performance of Leyden vs the main-line JDK, you need:

- An official build of JDK 21
- An up-to-date build of the JDK main-line
- The latest Leyden build
- Maven (ideally 3.8 or later, as required by some of the demos). Note: if you are behind
  a firewall, you may need to [set up proxies for Maven](https://maven.apache.org/guides/mini/guide-proxies.html)

The same steps are used for benchmarking all of the above demos. For example:

```
$ cd helidon-quickstart-se
$ make PREMAIN_HOME=/repos/leyden/build/linux-x64/images/jdk \
       MAINLINE_HOME=/repos/jdk/build/linux-x64/images/jdk \
       BLDJDK_HOME=/usr/local/jdk21 \
       bench
run,mainline default,mainline custom static cds,mainline aot cache,premain aot cache
1,253,137,71,54
2,267,129,67,56
3,256,130,70,56
4,249,132,70,55
5,252,133,70,56
6,257,132,73,55
7,261,136,71,55
8,253,129,74,58
9,255,131,107,53
10,260,139,74,53
Geomean,256.25,132.76,74.05,55.08 (4.65x improvement)
Stdev,4.96,3.28,10.95,1.45
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

# Preliminary Benchmark Results

The following charts show the relative start-up performance of the leyden/premain2 branch vs
the JDK main-line.

For example, a number of "premain2 aot cache: 255" indicates that if the application takes
1000 ms to start-up with the JDK main-line, it takes only 255 ms to start up when all the
current set of Leyden optimizations are enabled.

The benchmark results are collected with `make bench` in the following directories:

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
| **premain2 aot cache**          |Run benchmark with a custom AOT cache (Leyden "premain2" branch)|

We have benchmark results from two types of configurations using the
script [bench_data/do_bench.sh](bench_data/do_bench.sh):

- Desktop/Server Class: these are the results when running on a modern desktop or server, using the
  command `bash bench_data/do_bench.sh`.
- 2 Cores Only: these are the results when running in a limited configuration where only two cores.
  are available, using the command `taskset -c 1,2 bash bench_data/do_bench.sh`

The 2 Cores Only setting is intended to emulate microservice configurations where a very small number
of cores are allocated for small Java programs. In this setting, the JIT compiler may compete for CPU
with the Java program, making start-up slower. The **premain2 aot cache** numbers usually are much better in
this setting because most of the start-up code has been AOT-compiled, so the app can spend most of the
available CPUs to execute application logic.

These JDK versions were used in the comparisons:

- JDK mainline: https://github.com/openjdk/jdk/tree/2365ecc5fd83cf27c6c55aaca5aa6bad6ae9dbbf
- Leyden: https://github.com/openjdk/leyden/tree/b6ba32f3b5dd9a3485cb2a7f7d12aaf0a28979b7

For details information about the hardware and raw numbers, see [bench.20260929.txt](bench_data/bench.20260929.txt)
 and [bench.20260929-2cpu.txt](bench_data/bench.20260929-2cpu.txt)

# Premain2 AOT Performance Summary

This is the speed up of **premain2 aot cache** vs **mainline default** in the two types of configurations, in the Leyden "premain2" branch.

| Benchmark | Desktop/Server Class (28 Cores) | 2 Cores Only|
|:-------------|-------------:| -------------:|
| Helidon Quick Start | 4.42x | 4.18x |
| JavacBenchApp 50 source files | 2.39x | 2.03x |
| Micronaut First App Demo | 3.99x | 5.44x |
| Quarkus Getting Started Demo | 4.11x | 4.87x |
| Spring-boot Getting Started Demo | 4.71x | 4.25x |
| Spring PetClinic Demo | 4.73x | 3.93x |

# Benchmark Results - Desktop/Server Class (28 Cores)

## Helidon Quick Start (4.42x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 522, 292, 226]
```

## JavacBenchApp 50 source files (2.39x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 793, 511, 419]
```

## Micronaut First App Demo (3.99x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 507, 293, 250]
```

## Quarkus Getting Started Demo (4.11x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 438, 284, 244]
```

## Spring-boot Getting Started Demo (4.71x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 488, 295, 212]
```

## Spring PetClinic Demo (4.73x improvement - Desktop/Server Class (28 Cores))

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 609, 562, 211]
```
# Benchmark Results - 2 Cores Only

## Helidon Quick Start (4.18x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 563, 426, 239]
```

## JavacBenchApp 50 source files (2.03x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 871, 809, 492]
```

## Micronaut First App Demo (5.44x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 406, 330, 184]
```

## Quarkus Getting Started Demo (4.87x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 447, 364, 205]
```

## Spring-boot Getting Started Demo (4.25x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 547, 462, 235]
```

## Spring PetClinic Demo (3.93x improvement - 2 Cores Only)

```mermaid
---
config:
    theme: "forest"
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static cds", "mainline aot cache", "premain2 aot cache"]
    y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
    bar [1000, 635, 570, 254]
```
