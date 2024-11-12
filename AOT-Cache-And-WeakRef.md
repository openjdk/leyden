This patch is an experiment to find out how WeakReference interacts AOT caching.

# Applying the patch

- Go to https://github.com/iklam/jdk/tree/8341587-example-of-weakref-problem-in-aot-cache
- The HEAD of this branch should be a commit with the message "An example of WeakReference not working when stored inside the AOT cache"
- Apply this commit on top of https://github.com/openjdk/leyden/tree/premain . (This patch was tested against https://github.com/openjdk/leyden/commit/52eee916777c6b7a7a70ea6f55750935e0033d87)
- Rebuild the JDK.


# Test program

```
import java.lang.invoke.MethodType;

public class MyTest {
    public static void main(String... av) {
        System.gc();
        System.gc();
        System.out.println("\n\n\nt1========================================");
        MethodType t1 = MethodType.methodType(Object.class, Test1.class);

        System.out.println("\n\n\nt2========================================");
        MethodType t2 = MethodType.methodType(Object.class, Test2.class);

        System.out.println("\n\n\nt3========================================");
        MethodType t3 = MethodType.methodType(Object.class, Test3.class);
    }
}

class Test1 {}
class Test2 {}
class Test3 {}
```

This program triggers the test following code inside `java/lang/invoke/MethodType$AOTHolder`:

```
public class MethodType {
    [....]
    static class AOTHolder {
        private static final @Stable MethodType[] objectOnlyTypes = new MethodType[20];
        private static @Stable HashMap<MethodType,MethodType> archivedMethodTypes;

        private static Object theObject = new String("non-null");

        private static WeakReference<Object> ref = new WeakReference<>(theObject);
        static void test1() {
            System.out.println("ref = " + ref.get());
        }
        static void test2() {
            theObject = null;
            System.gc();
            System.gc();
            System.out.println("ref = " + ref.get());
        }
        static void test3() {
            System.out.println("ref = " + ref.get());
        }
    }
```

Running this test normally shows the expected behavior -- when `theObject` is no longer references, `ref.get()` should return `null`:

```
$ java -Xshare:dump
$ java -cp ~/tmp  -Xlog:gc+ref=trace MyTest > x.x
$ egrep '(Weak)|(ref = )' x.x | grep -v Drop | grep -v Discover | grep -v 'Enque'
[0.064s][trace][gc,ref] GC(0) Abandoning WeakRef discovered list
[0.068s][debug][gc,ref] GC(0) Skipped SoftWeakFinalRefsPhase of Reference Processing: no references
[0.150s][trace][gc,ref] GC(1) Abandoning WeakRef discovered list
[0.156s][debug][gc,ref] GC(1) Skipped SoftWeakFinalRefsPhase of Reference Processing: no references
ref = non-null
[0.177s][trace][gc,ref] GC(2) Abandoning WeakRef discovered list
[0.184s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Soft before0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.184s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Weak before1 0 0 0 0 0 0 0 0 0 0 0 0 (1)
[0.184s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Final before0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.184s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Final after0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.196s][trace][gc,ref] GC(3) Abandoning WeakRef discovered list
[0.202s][debug][gc,ref] GC(3) Skipped SoftWeakFinalRefsPhase of Reference Processing: no references
ref = null
ref = null
```

However, if we recreate the JDK's default CDS archive with `-XX:+AOTClassLinking`


```
$ java -Xshare:dump -XX:+AOTClassLinking
$ java -cp ~/tmp  -Xlog:gc+ref=trace MyTest > y.y
$ egrep '(Weak)|(ref = )' y.y | grep -v Drop | grep -v Discover | grep -v 'Enque'
[0.067s][trace][gc,ref] GC(0) Abandoning WeakRef discovered list
[0.072s][debug][gc,ref] GC(0) Skipped SoftWeakFinalRefsPhase of Reference Processing: no references
[0.167s][trace][gc,ref] GC(1) Abandoning WeakRef discovered list
[0.177s][debug][gc,ref] GC(1) Skipped SoftWeakFinalRefsPhase of Reference Processing: no references
ref = non-null
[0.204s][trace][gc,ref] GC(2) Abandoning WeakRef discovered list
[0.214s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Soft before0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.214s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Weak before1 0 0 0 0 0 0 0 0 0 0 0 0 (1)
[0.214s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Final before0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.214s][trace][gc,ref] GC(2) SoftWeakFinalRefsPhase Final after0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.238s][trace][gc,ref] GC(3) Abandoning WeakRef discovered list
[0.248s][trace][gc,ref] GC(3) SoftWeakFinalRefsPhase Soft before0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.248s][trace][gc,ref] GC(3) SoftWeakFinalRefsPhase Weak before1 0 0 0 0 0 0 0 0 0 0 0 0 (1)
[0.248s][trace][gc,ref] GC(3) SoftWeakFinalRefsPhase Final before0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
[0.248s][trace][gc,ref] GC(3) SoftWeakFinalRefsPhase Final after0 0 0 0 0 0 0 0 0 0 0 0 0 (0)
ref = non-null
ref = non-null

```

# How does this patch create WeakReferece inside AOT cache

Although many classes are initialized during the assembly phase (`java -Xshare:dump`), most of these
classes are stored into the AOT cache as if they were not initialized. In the production run, the `<clinit>`
of these classes will be executed (again) to initialize static fields, etc.

Only a very limited set of classes are stored into the AOT cache in the "initialized" state (and this happens only
when -XX:+AOTClassLinking is specified). One of them is the `java/lang/invoke/MethodType$AOTHolder` class
used by this patch.

To see how the static fields in `MethodType$AOTHolder` are cached, try this:


```
java -Xshare:dump -XX:+AOTClassLinking -Xlog:cds+map,cds+map+oops=trace:file=cds.map:none:filesize=0
```

In `cds.map`, you can see something like this that shows all 4 static fields of `MethodType$AOTHolder`:


```
0x00000007ffcf0298: @@ Object (0xfff9e053) java.lang.Class
 - klass: 'java/lang/Class' 0x00000008001d7bd8
 - fields (16 words):
 [.......]
 - signature: Ljava/lang/invoke/MethodType$AOTHolder;
 - ---- static fields (4):
 - private static final 'objectOnlyTypes' '[Ljava/lang/invoke/MethodType;' @112 0x00000007ffc6dd80 (0xfff8dbb0) [Ljava.lang.invoke.MethodType; length: 20
 - private static 'archivedMethodTypes' 'Ljava/util/HashMap;' @116 0x00000007ffc6e378 (0xfff8dc6f) java.util.HashMap
 - private static 'theObject' 'Ljava/lang/Object;' @120 0x00000007ffc7c280 (0xfff8f850) java.lang.String
 - private static 'ref' 'Ljava/lang/ref/WeakReference;' @124 0x00000007ffc7c298 (0xfff8f853) java.lang.ref.WeakReference

```

Following the address of `0x00000007ffc7c298`, you can find the `WeakReference` for `MethodType$AOTHolder.ref`:


```
0x00000007ffc7c298: @@ Object (0xfff8f853) java.lang.ref.WeakReference
 - klass: 'java/lang/ref/WeakReference' 0x0000000800205e60
 - fields (4 words):
 - private 'referent' 'Ljava/lang/Object;' @12 0x00000007ffc7c280 (0xfff8f850) java.lang.String
 - volatile 'queue' 'Ljava/lang/ref/ReferenceQueue;' @16 0x00000007ffd78e10 (0xfffaf1c2) java.lang.ref.ReferenceQueue$Null
 - volatile 'next' 'Ljava/lang/ref/Reference;' @20 null
 - private transient 'discovered' 'Ljava/lang/ref/Reference;' @24 null
```

Note that the `ReferenceQueue` class is NOT cached in the "initialized" state:

```
0x00000007ffcec8b8: @@ Object (0xfff9d917) java.lang.Class
 - klass: 'java/lang/Class' 0x00000008001d7bd8
 - fields (16 words):
 [.......]
 - signature: Ljava/lang/ref/ReferenceQueue;
 - ---- static fields (2):
 - static final 'NULL' 'Ljava/lang/ref/ReferenceQueue;' @112 null
 - static final 'ENQUEUED' 'Ljava/lang/ref/ReferenceQueue;' @116 null
 - static final synthetic '$assertionsDisabled' 'Z' @120  false (0x00)
```

This means that so the static field `ReferenceQueue.NULL` is stored as a `null`, and will be initialized at production time by this code:

```
public class ReferenceQueue<T> {
    [....]
    static final ReferenceQueue<Object> NULL = new Null();
```

However, if you look at the `Reference` constructor, you can see that for `MethodType$AOTHolder.ref`, the `queue` field is initialized
to `ReferenceQueue.NULL`.


```
public class Reference<T> .... {
    [....]
    Reference(T referent, ReferenceQueue<? super T> queue) {
        this.referent = referent;
        this.queue = (queue == null) ? ReferenceQueue.NULL : queue;
    }
```

So the problem seems to be that `MethodType$AOTHolder.ref.queue` ends up to hold
the value of `ReferenceQueue.NULL` during the assembly phase.

Two fixes seems possible:

- At production run, update `MethodType$AOTHolder.ref.queue` to point to the correct `ReferenceQueue.NULL`
- Cache the `ReferenceQueue` in "initialized" state so `ReferenceQueue.NULL` won't be initialized again during production run, so that the cached version of `MethodType$AOTHolder.ref.queue` would be correct.

But I don't know if doing the above would be sufficient for getting cached WeakReferences to work. Perhaps more steps are needed.


