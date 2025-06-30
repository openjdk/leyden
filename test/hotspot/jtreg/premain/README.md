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

## Benchmarking

We use a small set of benchmarks to demonstrate the performance of the optimizations in the Leyden repo.

| Benchmark  | Source |
| ------------- | ------------- |
|[helidon-quickstart-se](helidon-quickstart-se)|https://helidon.io/docs/v4/se/guides/quickstart|
|[micronaut-first-app](micronaut-first-app)|https://guides.micronaut.io/latest/creating-your-first-micronaut-app-maven-java.html|
|[quarkus-getting-started](quarkus-getting-started)|https://quarkus.io/guides/getting-started|
|[spring-boot-getting-started](spring-boot-getting-started)|https://spring.io/guides/gs/spring-boot|
|[spring-petclinic](spring-petclinic)|https://github.com/spring-projects/spring-petclinic|

See [README.md in the repo root](../../../../README.md) for some sample benchmark results.

## Docs

- [InvokeDynamic.md](InvokeDynamic.md) CDS optimizations for invokedynamic

## Regression Testing

Leyden-specific tests have been added to the following directories in the repo:

- [test/hotspot/jtreg/runtime/cds/appcds/applications](../runtime/cds/appcds/applications)
- [test/hotspot/jtreg/runtime/cds/appcds/indy](../runtime/cds/appcds/indy)
- [test/hotspot/jtreg/runtime/cds/appcds/leyden](../runtime/cds/appcds/leyden)
- [test/hotspot/jtreg/runtime/cds/appcds/preloadedClasses](../runtime/cds/appcds/preloadedClasses)

These test cases can be executed using jtreg. Some of the tests (in the applications directories)
require binaries to be built separately. Please refer to the script [lib/build-for-jtreg.sh](lib/build-for-jtreg.sh)
