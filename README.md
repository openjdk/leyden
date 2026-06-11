# Welcome to the Leyden Repository

The purpose of the Leyden repository is to design and implement improvements to
the startup time, time to peak performance, and footprint of Java programs, as a part of
[Project Leyden](https://openjdk.org/projects/leyden). We solicit feedback from
the Java community, with the hope that these improvements will be
incorporated into future JDK releases.

## 0. Disclaimers

- *This repository contains experimental and unstable code. It is not intended to be used
   in a production environment.*
- *This repository is intended for developers of the JDK, and advanced Java developers who
   are familiar with building the JDK.*
- *The experimental features in this repository may be changed or removed without notice.
   Command line flags and workflows will change.*
- *The benchmarks results reported on this page are for illustrative purposes only. Your
   applications may get better or worse results.*

## 1. Overview

As of JDK 27, the Leyden Project has successfully delivered ahead-of-time (AOT)
optimizations JEPs:

- [JEP 483 - Ahead-of-Time Class Loading & Linking](https://openjdk.org/jeps/483)
- [JEP 514 - Ahead-of-Time Command-Line Ergonomics](https://openjdk.org/jeps/514)
- [JEP 515 - Ahead-of-Time Method Profiling](https://openjdk.org/jeps/515)
- [JEP 516 - Ahead-of-Time Object Caching with Any GC](https://openjdk.org/jeps/516)

Please refer to the above JEPs for a detailed discussion of AOT optimizations.

The Leyden "[premain2](https://github.com/openjdk/leyden/blob/premain2/)" branch
includes new AOT optimizations that are not yet integrated into the JDK mainline:

- **[Ahead-of-Time Code Compilation (JEP draft 8335368)](https://openjdk.org/jeps/8335368)**: Methods that are frequently used during the training run can be
  compiled and stored along with the AOT cache. As a result, as soon as the application starts up
  in the production run, its methods can be natively executed.
  - This feature is enabled by default when you create an AOT cache. It can be disabled with the diagnostic
    flag `-XX:-AOTCodeCaching`.

## 2. Building the Leyden Repository

The Leyden Repository can be built in the same way as the main-line JDK repository.
Please use the "premain2" branch. I.e., [https://github.com/openjdk/leyden/tree/premain2](https://github.com/openjdk/leyden/tree/premain2).

For build instructions please see the
[online documentation](https://git.openjdk.org/jdk/blob/master/doc/building.md),
or either of these files:

- [doc/building.html](doc/building.html) (html version)
- [doc/building.md](doc/building.md) (markdown version)

See <https://openjdk.org/> for more information about the OpenJDK
Community and the JDK and see <https://bugs.openjdk.org> for JDK issue
tracking.

## 3. Trying out Leyden Features

The easiest way to try out the Leyden optimizations is to build a JVM from the Leyden repository, and use it with your application with the `-XX:AOTCache` flag.

Here's a small benchmark that uses the JDK's built-in
[`JavaCompiler`](https://docs.oracle.com/en/java/javase/21/docs/api/java.compiler/javax/tools/JavaCompiler.html)
class to compile some Java source files. This benchmark spends a significant amount of start-up time
setting up the classes used by `JavaCompiler`, so it will benefit from the Leyden features.

First, download [JavacBenchApp.java](test/setup_aot/JavacBenchApp.java) and compile it into a JAR file.

(Remember to use the `java` program that you built from the Leyden repository.)

```
$ javac JavacBenchApp.java
$ jar cvf JavacBenchApp.jar JavacBenchApp*.class
added manifest
adding: JavacBenchApp$ClassFile.class(in = 1608) (out= 787)(deflated 51%)
adding: JavacBenchApp$FileManager.class(in = 2090) (out= 979)(deflated 53%)
adding: JavacBenchApp$SourceFile.class(in = 1351) (out= 671)(deflated 50%)
adding: JavacBenchApp.class(in = 7571) (out= 3302)(deflated 56%)
```

We can run this benchmark without any AOT optimizations. It takes 893 ms:

```
$ java -cp JavacBenchApp.jar JavacBenchApp 50
Generated source code for 51 classes and compiled them in 893 ms
```

To use AOT optimizations for JavacBenchApp, we should first perform a _training run_ and
capture the profiling information into `JavacBenchApp.aotconfig`

```
$ java -XX:AOTMode=record -XX:AOTConfiguration=JavacBenchApp.aotconfig \
       -cp JavacBenchApp.jar JavacBenchApp 50
$ ls -l JavacBenchApp.aotconfig
-rw-rw-r-- 1 iklam iklam 27652096 Mar  3 16:23 JavacBenchApp.aotconfig
```

With the `JavacBenchApp.aotconfig` file, we can create the AOT cache. This is called the _assembly phase_:

```
$ java -XX:AOTMode=create -XX:AOTConfiguration=JavacBenchApp.aotconfig \
       -cp JavacBenchApp.jar -XX:AOTCache=JavacBenchApp.aot
$ ls -l JavacBenchApp.aot
-r--r--r-- 1 iklam iklam 42332160 Mar  3 16:58 JavacBenchApp.aot
```

Alternatively, you can also combine the training run and assembly phase with a single command:

```
$ java -XX:AOTCacheOutput=JavacBenchApp.aot \
       -cp JavacBenchApp.jar JavacBenchApp 50
$ ls -l JavacBenchApp.aot
-r--r--r-- 1 iklam iklam 42332160 Mar  3 16:58 JavacBenchApp.aot
```

Now, we can make a _production run_ of the program using the AOT cache `JavacBenchApp.aot`. It finishes in 423 ms, or more than twice as fast as
before.

```
$ java -XX:AOTCache=JavacBenchApp.aot -cp JavacBenchApp.jar JavacBenchApp 50
Generated source code for 51 classes and compiled them in 423 ms
```

### Ending the Training Run Early

By default, training runs end when the application terminates.  You have other options to end training runs:

- `jcmd <pid> AOT.end_training`
-  new MXBean API `jdk.management.HotSpotAOTCacheMXBean.endRecording()`

### Diagnosing Potential Performance Issues

As mentioned below, parts or all of the AOT cache may be disabled under certain circumstances. This may lead
to lower performance than expected. To diagnose potential performance issues, you can add `-Xlog:aot*` to the
command line to see detailed information about what parts of the AOT cache are being utilized. For example, if the
the AOT-compiled code cannot be loaded, you will see a log message like this:

```
[0.008s][info][aot,codecache,init] AOT Code Cache disabled: it was created with GC = "g1 gc" vs current "serial gc"
[0.008s][info][aot,codecache,init] Unable to use AOT Code Cache.
```

### Diagnostic VM Flags

By default, all of the optimizations described
in the [Overview](#1-overview) section above are enabled by default. This ensures that you can get all the optimizations
without specifying them individually.

For diagnostic purposes, you can selectively disable some of the options:

- The `-XX:+AOTCodeCaching` flag affects only the assembly phase and the production run.
- The `-XX:+AOTRecordTraining` flag affects only the training run and the assembly phase.
- The `-XX:+AOTReplayTraining` flag affects only the production run.
- All other options affect only the assembly phase.

For example, you can disable the loading of AOT-compiled methods during the production run. Notice that the benchmark now
starts more slowly than it did when AOT-compiled methods was loaded.

```
$ java -XX:AOTCache=JavacBenchApp.aot -Xlog:aot=error \
       -XX:+UnlockDiagnosticVMOptions -XX:-AOTCodeCaching \
       -cp JavacBenchApp.jar JavacBenchApp 50
Generated source code for 51 classes and compiled them in 647 ms
```

You can also disable AOT compilation in the assembly phase. Note that the size of the AOT
cache is smaller because it no longer has AOT-compiled methods.

```
$ java -XX:AOTMode=create -XX:AOTConfiguration=JavacBenchApp.aotconfig \
       -XX:AOTCache=JavacBenchApp.aot \
       -XX:+UnlockDiagnosticVMOptions -XX:-AOTCodeCaching \
       -cp JavacBenchApp.jar
$ ls -l JavacBenchApp.aot
-r--r--r-- 1 iklam iklam 29990912 Mar  3 16:34 JavacBenchApp.aot
```


## 4. Limitations of the Leyden Prototype

When trying out the Leyden prototype, please pay attention to the following limitations.

### The Same CPU Must be Used between Training and Production Runs

The AOT-compiled code will be only used if the production run is on a machine with the same type of CPU
as used in the training run and assembly phase. If this is not the case (for example, the production run is on
a machine that has different AVX capabilities), the AOT-compiled code will be ignored.


### The Same Garbage Collector Must be Used between Training and Production Runs

The AOT code generated by the Leyden prototype includes machine instructions that are specific to
the garbage collector. We recommend that you explicitly specify the same collector during both
training and production runs. For example, if you prefer to use the SerialGC:

```
# assembly phase.
$ java -XX:AOTMode=create -XX:AOTConfiguration=JavacBenchApp.aotconfig \
       -cp JavacBenchApp.jar \
       -XX:AOTCache=JavacBenchApp.aot -XX:+UseSerialGC

# production run
$ java -XX:AOTCache=JavacBenchApp.aot -XX:+UseSerialGC -cp JavacBenchApp.jar \
       JavacBenchApp 50
```

Otherwise, the AOT code may not be usable for the production run, leading to suboptimal performance.
For example, sometimes you may perform the assembly phase run on a large development host, and then use
a container to run the application in a small production node. In the following scenario, as the collector
is not explicitly specified, the VM will automatically pick G1 for the assembly phase, and SerialGC for the
production run (due to its limited amount of memory).

**UPDATE for JDK 27**: this example will pass after [JEP 523 - Make G1 the Default Garbage Collector in All Environments](https://openjdk.org/jeps/523)

```
# Assembly phase (uses G1 by default)
$ java -XX:AOTMode=create -XX:AOTConfiguration=JavacBenchApp.aotconfig \
       -cp JavacBenchApp.jar -XX:AOTCache=JavacBenchApp.aot

# Production run (uses SerialGC)
$ docker run --rm -v /repos/leyden/build/linux-x64/images/jdk:/jdk -v $(pwd):/test \
    --memory=1024m \
    container-registry.oracle.com/java/openjdk \
    bash -c 'cd /test; ' \
            '/jdk/bin/java -XX:AOTCache=JavacBenchApp.aot ' \
            '    -cp JavacBenchApp.jar JavacBenchApp 50'
[0.001s][error][aot] AOT cache has aot-linked classes. It cannot be used because
                     GC used during dump time (G1) is not the same as runtime (Serial)
[0.001s][error][aot] An error has occurred while processing the AOT cache.
[0.001s][error][aot] Unable to map shared spaces
Error occurred during initialization of VM
Unable to use AOT cache.
```

### All GCs are Supported

All GCs are supported after [JEP 516 - Ahead-of-Time Object Caching with Any GC](https://openjdk.org/jeps/516)

