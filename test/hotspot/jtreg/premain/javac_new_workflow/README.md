# Proposed Leyden Terminal Stage Workflow

This is a new propsed workflow for the "terminal stage" of the [Leyden
condenser pipeline](https://openjdk.org/projects/leyden/notes/03-toward-condensers)

- The CDS and AOT caches are automatically generated with a single `java` command.

- The caches are stored in the file specified by the `-XX:CacheDataStore=<app>.cds` option
    - The implementation is still a work in progress. AOT integration is not done yet.
    - As an intermediate step, the AOT cache may be stored in a separate file.

- The `-XX:CacheDataStore` option is intended to be a replacement for the existing
  `-XX:SharedArchiveFile` option.

- We no longer need a separate "training run". Instead, the `-XX:CacheDataStore=<app>.cds`
  option should be added to the command-line of the production run of your application. For example

    ```
    java -Xlog:cds -XX:CacheDataStore=javac.cds com.sun.tools.javac.Main ~/tmp/HelloWorld.java
    ```

- If the specified file doesn't exist, it will be created automatically when the JVM process exits:

    - The loaded classes and their compiler profile are dumped into a temporary file with a `.preimage`
      prefix. E.g., `javac.cds.preimage`
    - A JVM subprocess is launched to convert `javac.cds.preimage` to the final CDS image, `javac.cds`
        - See the end of `MetaspaceShared::preload_and_dump_impl()` in
          [metaspaceShared.cpp](../../../../../src/hotspot/share/cds/metaspaceShared.cpp)

- In the next run of your application, the `javac.cds` file will be automatically loaded at start-up. Your
  application will see the benefit of CDS (and soon, AOT).


- By default, the following VM options are used when `-XX:CacheDataStore=<app>.cds` is specified. This way, you
  can automatically use all the Leyden-premain optimizations without specifying any extra flags.

    - `AOTRecordTraining` is set to `true` when the VM is *writing* the `<app>.cds.preimage` file.
    - `AOTRecordTraining`, `AOTReplayTraining` and `StoreCachedCode` are set to `true` when the VM is *writing* the final CDS image file.
    - `AOTReplayTraining` and `LoadCachedCode` are set to `true` when the VM is *loading* the final CDS image file.
    - `CachedCodeFile` is set to `<app>.cds.code`.

  However, you can explicitly disable some of these flags for diagnostic purposes. For example, the
  following command-line will automatically generate `app.cds` and `app.cds.code` on its first run. However, it will
  only load `app.cds` on subsequent runs, but not `app.cds.code`.


    ```
    java -XX:CacheDataStore=app.cds -XX:-LoadCachedCode -cp app.jar MyApp

    ```

- See [run.sh](run.sh) in this directory for an example of using `-XX:CacheDataStore=<app>.cds`

## Notes

- For applications that do not exit automatically, you may need to hand-craft a training like this, so you
  app exits voluntarily, to allow the subprocess to be launched to complete the generation of `app.cds`.

    ```
    rm -f app.cds
    java -XX:CacheDataStore=app.cds -cp app.jar MyApp -exit-after-start
    ```

- In the future, we may add a `jcmd` option to connect to a long running JVM and trigger the creation of
  the CacheDataStore.

- By default, the subprocess is automatically forked at JVM exit. For debugging purpose, you can use the
  `-XX:+CDSManualFinalImage` option to disable the automatic forking. This allows you to debug the the
   subprocess more easily.
    - When `-XX:+CDSManualFinalImage` is specified, the JVM will create only the `<app>.cds.preimage`
      file at exit. It will then print out a command-line that you can execute manually to create the
      final `<app>.cds` file.

## AOT Code Generation

AOT support is not fully implemented yet. As of Sep 18, 2023, at the end of `MetaspaceShared::preload_and_dump()`,
the compiler will be executed to compile a single method, `String::charAt`. The nmethod will be stored inside the
`CachedCodeFile`.

The intended design is to, at this point, compile all methods that were recorded in the traing data during the
training run. This is TBD.

## Benchmark

(Sep 11, 2023)


- Without `-XX:CacheDataStore`

```
$ perf stat -r 20 java com.sun.tools.javac.Main HelloWorld.java

 Performance counter stats for 'java com.sun.tools.javac.Main HelloWorld.java' (20 runs):

       643.10 msec task-clock        #   2.374 CPUs utilized    ( +-  0.24% )
        4,318      context-switches  #   6.800 K/sec            ( +-  1.84% )
           29      cpu-migrations    #  45.666 /sec             ( +-  5.89% )
       15,003      page-faults       #  23.625 K/sec            ( +-  0.20% )
2,936,972,438      cycles            #   4.625 GHz              ( +-  0.24% )
3,262,915,553      instructions      #   1.12  insn per cycle   ( +-  0.10% )
  644,286,520      branches          #   1.015 G/sec            ( +-  0.11% )
   29,099,407      branch-misses     #   4.57% of all branches  ( +-  0.15% )

      0.27091 +- 0.00107 seconds time elapsed  ( +-  0.40% )
```

- With `-XX:CacheDataStore` (note: AOT is not yet supported)

```
$ perf stat -r 20 java -XX:+AOTReplayTraining -XX:CacheDataStore=javac.cds com.sun.tools.javac.Main HelloWorld.java

 Performance counter stats for 'java -XX:+AOTReplayTraining -XX:CacheDataStore=javac.cds com.sun.tools.javac.Main HelloWorld.java' (20 runs):

       234.72 msec task-clock        #   2.165 CPUs utilized    ( +-  0.29% )
        1,839      context-switches  #   7.735 K/sec            ( +-  1.22% )
           14      cpu-migrations    #  58.883 /sec             ( +-  4.13% )
        9,003      page-faults       #  37.866 K/sec            ( +-  0.22% )
1,070,819,957      cycles            #   4.504 GHz              ( +-  0.30% )
1,170,776,369      instructions      #   1.08  insn per cycle   ( +-  0.35% )
  229,314,097      branches          # 964.471 M/sec            ( +-  0.36% )
    9,544,981      branch-misses     #   4.09% of all branches  ( +-  0.38% )

     0.108406 +- 0.000844 seconds time elapsed  ( +-  0.78% )
```
