# Notes on InvokeDynamic and AOT


## Overview

This document is current as of the GIT revision date. It describes the behavior of the branch that contains this document.

CDS Indy optimization is enabled by the -XX:+ArchiveInvokeDynamic flag. This must be specified when
creating the **static CDS archive**. I.e., with -Xshare:dump.

Notes:

- Indy call sites are pre-resolved only for the static CDS archive. Therefore,
  if the AOT wants to generate code for pre-resolved indy call sites in a class X,
  then X must be in the static CDS archive.

- CDS does not pre-resolve all indys. Instead, it only pre-resolve indys that were actually
  executed when generating the classlist. Therefore, you should make sure that your classlist generation
  covers all indy call sites that you want to generate good AOT code for.

- Only two BSMs are supported so far. See `ClassPrelinker::should_preresolve_invokedynamic()` in 
  [classPrelinker.cpp](../src/hotspot/share/cds/classPrelinker.cpp)

    - `StringConcatFactory::makeConcatWithConstants`
    - `LambdaMetafactory::metafactory`


## Simple Example


```
class ConcatA {
    static {
        bar("000", "222");
        foo("000", "222");
    }

    public static void main(String args[]) throws Exception {
        foo("000", "222");
        System.out.println(x);
        System.out.println(ConcatB.foo("123" , "abc"));

        doit(() -> {
                System.out.println("Hello Lambda");
                Thread.dumpStack();
            });
    }

    static void doit(Runnable r) {
        r.run();
    }

    static String x;
    static void foo(String a, String b) {
        x = a + b;
    }
    static void bar(String a, String b) {
        x = a + b;
    }
}

class ConcatB {
  static String foo(String a, String b) {
    return "a" + b;
  }
}
```

You can compile it using:

```
(
cd ~/tmp
rm -rf tmpclasses
mkdir -p tmpclasses
javac -d tmpclasses ConcatA.java && jar cvf ConcatA.jar -C tmpclasses .
java -cp ConcatA.jar ConcatA
)
```

Then, create classlist, dump static archive with `-XX:+ArchiveInvokeDynamic` and run it. The `-Xshare:dump` step prints some warnings,
which can be ignored.


```
java -Xshare:off -cp ~/tmp/ConcatA.jar -XX:DumpLoadedClassList=concata.lst ConcatA
java -XX:+ArchiveInvokeDynamic -Xshare:dump -XX:SharedClassListFile=concata.lst -cp ~/tmp/ConcatA.jar -XX:SharedArchiveFile=concata.jsa
java -cp ~/tmp/ConcatA.jar -XX:SharedArchiveFile=concata.jsa ConcatA
```

## Verify that the call sites are archived

To verify that the indy call sites are indeed archived, compare the following output:

```
java -Xlog:methodhandles -cp ~/tmp/ConcatA.jar -XX:SharedArchiveFile=concata.jsa ConcatA
vs
java -Xlog:methodhandles -cp ~/tmp/ConcatA.jar ConcatA
```

The second one shows a lot more output than the first one.

You can also use `-XX:+TraceBytecodes`. With `-XX:+ArchiveInvokeDynamic`, you can see that the first
indy for string concat took only a handful of bytecodes before reaching the `StringConcatHelper.simpleConcat()`
method. The BSM is completely skipped.


```
[3289356] static void ConcatA.bar(jobject, jobject)
[3289356]   192633     0  nofast_aload_0
[3289356]   192634     1  aload_1
[3289356]   192635     2  invokedynamic bsm=90 54 <makeConcatWithConstants(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;>

[3289356] static jobject java.lang.invoke.Invokers$Holder.linkToTargetMethod(jobject, jobject, jobject)
[3289356]   192636     0  aload_2
[3289356]   192637     1  checkcast 12 <java/lang/invoke/MethodHandle>
[3289356]   192638     4  nofast_aload_0
[3289356]   192639     5  aload_1
[3289356]   192640     6  invokehandle 45 <java/lang/invoke/MethodHandle.invokeBasic(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;> 

[3289356] static jobject java.lang.invoke.DelegatingMethodHandle$Holder.reinvoke_L(jobject, jobject, jobject)
[3289356]   192641     0  nofast_aload_0
[3289356]   192642     1  checkcast 12 <java/lang/invoke/BoundMethodHandle$Species_L>
[3289356]   192643     4  nofast_getfield 16 <java/lang/invoke/BoundMethodHandle$Species_L.argL0/Ljava/lang/Object;> 
[3289356]   192644     7  astore_3
[3289356]   192645     8  aload_3
[3289356]   192646     9  checkcast 18 <java/lang/invoke/MethodHandle>
[3289356]   192647    12  aload_1
[3289356]   192648    13  aload_2
[3289356]   192649    14  invokehandle 67 <java/lang/invoke/MethodHandle.invokeBasic(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;> 

[3289356] static jobject java.lang.invoke.DirectMethodHandle$Holder.invokeStatic(jobject, jobject, jobject)
[3289356]   192650     0  nofast_aload_0
[3289356]   192651     1  invokestatic 16 <java/lang/invoke/DirectMethodHandle.internalMemberName(Ljava/lang/Object;)Ljava/lang/Object;> 

[3289356] static jobject java.lang.invoke.DirectMethodHandle.internalMemberName(jobject)
[3289356]   192652     0  nofast_aload_0
[3289356]   192653     1  checkcast 3 <java/lang/invoke/DirectMethodHandle>
[3289356]   192654     4  nofast_getfield 80 <java/lang/invoke/DirectMethodHandle.member/Ljava/lang/invoke/MemberName;> 
[3289356]   192655     7  areturn

[3289356] static jobject java.lang.invoke.DirectMethodHandle$Holder.invokeStatic(jobject, jobject, jobject)
[3289356]   192656     4  astore_3
[3289356]   192657     5  aload_1
[3289356]   192658     6  aload_2
[3289356]   192659     7  aload_3
[3289356]   192660     8  checkcast 21 <java/lang/invoke/MemberName>
[3289356]   192661    11  invokestatic 257 <java/lang/invoke/MethodHandle.linkToStatic(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/invoke/MemberName;)Ljava/lang/Object;> 

[3289356] static jobject java.lang.StringConcatHelper.simpleConcat(jobject, jobject)
```

The same is true for Lambda proxies:

```
[3289356] static void ConcatA.main(jobject)
[3289356]   199257    29  invokedynamic bsm=79 42 <run()Ljava/lang/Runnable;>

[3289356] static jobject java.lang.invoke.Invokers$Holder.linkToTargetMethod(jobject)
[3289356]   199258     0  nofast_aload_0
[3289356]   199259     1  checkcast 12 <java/lang/invoke/MethodHandle>
[3289356]   199260     4  invokehandle 56 <java/lang/invoke/MethodHandle.invokeBasic()Ljava/lang/Object;> 

[3289356] static jobject java.lang.invoke.LambdaForm$MH/0x800000003.invoke(jobject)
[3289356]   199261     0  nofast_aload_0
[3289356]   199262     1  checkcast 12 <java/lang/invoke/BoundMethodHandle$Species_L>
[3289356]   199263     4  nofast_getfield 16 <java/lang/invoke/BoundMethodHandle$Species_L.argL0/Ljava/lang/Object;> 
[3289356]   199264     7  areturn

[3289356] static jobject java.lang.invoke.Invokers$Holder.linkToTargetMethod(jobject)
[3289356]   199265     7  areturn

[3289356] static void ConcatA.main(jobject)
[3289356]   199266    34  invokestatic 46 <ConcatA.doit(Ljava/lang/Runnable;)V> 

[3289356] static void ConcatA.doit(jobject)
[3289356]   199267     0  nofast_aload_0
[3289356]   199268     1  invokeinterface 50 <java/lang/Runnable.run()V> 

[3289356] virtual void ConcatA$$Lambda/0x800000005.run()
[3289356]   199269     0  invokestatic 16 <ConcatA.lambda$main$0()V> 

[3289356] static void ConcatA.lambda$main$0()
[3289356]   199270     0  getstatic 17 <java/lang/System.out/Ljava/io/PrintStream;> 
[3289356]   199271     3  fast_aldc Hello Lambda
[3289356]   199272     5  invokevirtual 27 <java/io/PrintStream.println(Ljava/lang/String;)V> 
```

## Inspect the archived call sites

To inspect the archived call sites, the easiest is to call the `findclass` function inside gdb:


- `gdb --args java -cp ~/tmp/ConcatA.jar -XX:SharedArchiveFile=concata.jsa ConcatA`
- set a breakpoint at `exit_globals`
- When this breakpoint is hit, do the following. You can see that the appendix objects of the call sites have a "CDS perm index", which can be used by the compiler to inline such objects into AOT code.


```
Thread 2 "java" hit Breakpoint 2, exit_globals () at /jdk3/le1/open/src/hotspot/share/runtime/init.cpp:205
205	  if (!destructorsCalled) {
(gdb) call findclass("ConcatA", 0xff)

"Executing findclass"
flags (bitmask):
   0x01  - print names of methods
   0x02  - print bytecodes
   0x04  - print the address of bytecodes
   0x08  - print info for invokedynamic
   0x10  - print info for invokehandle

[  0] 0x0000000800256120 class ConcatA loader data: 0x00007ffff032fe70 for instance a 'jdk/internal/loader/ClassLoaders$AppClassLoader'{0x00000007ffce0f68}
0x0000000800256848 method <init> : ()V
0x0000000800731ae0    0 nofast_aload_0
0x0000000800731ae1    1 invokespecial 1 <java/lang/Object.<init>()V> 
0x0000000800731ae4    4 return

0x00000008002568c8 static method <clinit> : ()V
0x0000000800731b20    0 fast_aldc 000
0x0000000800731b22    2 fast_aldc 222
0x0000000800731b24    4 invokestatic 64 <ConcatA.bar(Ljava/lang/String;Ljava/lang/String;)V> 
0x0000000800731b27    7 fast_aldc 000
0x0000000800731b29    9 fast_aldc 222
0x0000000800731b2b   11 invokestatic 11 <ConcatA.foo(Ljava/lang/String;Ljava/lang/String;)V> 
0x0000000800731b2e   14 return

0x0000000800256948 static method main : ([Ljava/lang/String;)V
0x0000000800731b70    0 fast_aldc 000
0x0000000800731b72    2 fast_aldc 222
0x0000000800731b74    4 invokestatic 11 <ConcatA.foo(Ljava/lang/String;Ljava/lang/String;)V> 
0x0000000800731b77    7 getstatic 17 <java/lang/System.out/Ljava/io/PrintStream;> 
0x0000000800731b7a   10 getstatic 23 <ConcatA.x/Ljava/lang/String;> 
0x0000000800731b7d   13 invokevirtual 27 <java/io/PrintStream.println(Ljava/lang/String;)V> 
0x0000000800731b80   16 getstatic 17 <java/lang/System.out/Ljava/io/PrintStream;> 
0x0000000800731b83   19 fast_aldc 123
0x0000000800731b85   21 fast_aldc abc
0x0000000800731b87   23 invokestatic 37 <ConcatB.foo(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;> 
0x0000000800731b8a   26 invokevirtual 27 <java/io/PrintStream.println(Ljava/lang/String;)V> 
0x0000000800731b8d   29 invokedynamic bsm=79 42 <run()Ljava/lang/Runnable;>
  BSM: REF_invokeStatic 80 <java/lang/invoke/LambdaMetafactory.metafactory(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;> 
  arguments[3] = {
     <MethodType> 6 Symbol: '()V' count 65535
     <MethodHandle of kind 6 index at 88> 88 <ConcatA.lambda$main$0()V> 
     <MethodType> 6 Symbol: '()V' count 65535
  }
  ResolvedIndyEntry: Resolved InvokeDynamic Info:
 - Method: 0x00000008000f5e40 java.lang.Object java.lang.invoke.Invokers$Holder.linkToTargetMethod(java.lang.Object)
 - Resolved References Index: 12
 - CP Index: 42
 - Num Parameters: 1
 - Return type: object
 - Has Appendix: 1
 - Resolution Failed 0
 - appendix = 0x00000007ffc93648, CDS perm index = 14038
0x0000000800731b92   34 invokestatic 46 <ConcatA.doit(Ljava/lang/Runnable;)V> 
0x0000000800731b95   37 return

0x00000008002569c8 static method doit : (Ljava/lang/Runnable;)V
0x0000000800731be0    0 nofast_aload_0
0x0000000800731be1    1 invokeinterface 50 <java/lang/Runnable.run()V> 
0x0000000800731be6    6 return

0x0000000800256a48 static method foo : (Ljava/lang/String;Ljava/lang/String;)V
0x0000000800731c28    0 nofast_aload_0
0x0000000800731c29    1 aload_1
0x0000000800731c2a    2 invokedynamic bsm=90 54 <makeConcatWithConstants(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;>
  BSM: REF_invokeStatic 91 <java/lang/invoke/StringConcatFactory.makeConcatWithConstants(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;> 
  arguments[1] = {
     
  }
  ResolvedIndyEntry: Resolved InvokeDynamic Info:
 - Method: 0x00000008000f6540 java.lang.Object java.lang.invoke.Invokers$Holder.linkToTargetMethod(java.lang.Object, java.lang.Object, java.lang.Object)
 - Resolved References Index: 11
 - CP Index: 54
 - Num Parameters: 3
 - Return type: object
 - Has Appendix: 1
 - Resolution Failed 0
 - appendix = 0x00000007ffc93620, CDS perm index = 14037
0x0000000800731c2f    7 putstatic 23 <ConcatA.x/Ljava/lang/String;> 
0x0000000800731c32   10 return

0x0000000800256ac8 static method bar : (Ljava/lang/String;Ljava/lang/String;)V
0x0000000800731c70    0 nofast_aload_0
0x0000000800731c71    1 aload_1
0x0000000800731c72    2 invokedynamic bsm=90 54 <makeConcatWithConstants(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;>
  BSM: REF_invokeStatic 91 <java/lang/invoke/StringConcatFactory.makeConcatWithConstants(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;> 
  arguments[1] = {
     
  }
  ResolvedIndyEntry: Resolved InvokeDynamic Info:
 - Method: 0x00000008000f6540 java.lang.Object java.lang.invoke.Invokers$Holder.linkToTargetMethod(java.lang.Object, java.lang.Object, java.lang.Object)
 - Resolved References Index: 10
 - CP Index: 54
 - Num Parameters: 3
 - Return type: object
 - Has Appendix: 1
 - Resolution Failed 0
 - appendix = 0x00000007ffc93508, CDS perm index = 14029
0x0000000800731c77    7 putstatic 23 <ConcatA.x/Ljava/lang/String;> 
0x0000000800731c7a   10 return

0x0000000800256b48 static method lambda$main$0 : ()V
0x0000000800731cb8    0 getstatic 17 <java/lang/System.out/Ljava/io/PrintStream;> 
0x0000000800731cbb    3 fast_aldc Hello Lambda
0x0000000800731cbd    5 invokevirtual 27 <java/io/PrintStream.println(Ljava/lang/String;)V> 
0x0000000800731cc0    8 invokestatic 59 <java/lang/Thread.dumpStack()V> 
0x0000000800731cc3   11 return
(gdb) 
```

## Bigger Test Case

Currently the code is pretty rough, but it's able to archive about 177 indy calls sites when running Javac to compile a 
class with 3000 methods and 3000 fields. As of 2023/07/28, this test case passes with AOT enabled (`-XX:+LoadSharedCode`).

- See [IndyAndJavac.java](../runtime/cds/appcds/indy/IndyAndJavac.java).
- Run this test case in jtreg
- When completed, go to the scratch directory and do this:

```
$ grep cds,resolve *0001-dump.stdout | grep indy | wc
    177    2188   64186
```
