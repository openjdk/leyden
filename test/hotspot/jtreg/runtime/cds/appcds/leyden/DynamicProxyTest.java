/*
 * Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

/*
 * @test
 * @summary Test for archiving dynamic proxies
 * @requires vm.cds.write.archived.java.heap
 * @library /test/jdk/lib/testlibrary /test/lib
 * @build DynamicProxyTest testpkg.PublicTester
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app1.jar
 *                 DynamicProxyApp MyInvocationHandler
 *                 DynamicProxyTest$Foo DynamicProxyTest$Boo DynamicProxyTest$Coo
 *                 DynamicProxyTest$DefaultMethodWithUnlinkedClass Fruit
 *                 testpkg.PublicTester testpkg.NonPublicInterface testpkg.MyInvocationHandler2
 *                 jdk.test.lib.Asserts 
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app2.jar
 *                 Fruit Apple
 * @run driver DynamicProxyTest
 */


import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.security.ProtectionDomain;
import java.io.File;
import java.util.logging.Filter;
import java.util.Map;

import jdk.test.lib.Asserts;
import jdk.test.lib.cds.CDSAppTester;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class DynamicProxyTest {
    static final String app1Jar = ClassFileInstaller.getJarPath("app1.jar");
    static final String app2Jar = ClassFileInstaller.getJarPath("app2.jar");
    static final String mainClass = "DynamicProxyApp";

    public static void main(String[] args) throws Exception {
        {
          Tester tester = new Tester();
          tester.run(new String[] {"AOT"} );
        }
        {
          Tester tester = new Tester();
          tester.run(new String[] {"LEYDEN"} );
        }
    }

    static class Tester extends CDSAppTester {
        public Tester() {
            super("DynamicProxyTest");
        }

        @Override
        public String classpath(RunMode runMode) {
            switch (runMode) {
            case RunMode.TRAINING:
            case RunMode.TRAINING0:
            case RunMode.TRAINING1:
            case RunMode.DUMP_STATIC:
                return app1Jar;
            default:
                return app1Jar + File.pathSeparator + app2Jar;
            }
        }

        @Override
        public String[] appCommandLine(RunMode runMode) {
            return new String[] {
                "DynamicProxyApp", runMode.name()
            };
        }

        @Override
        public String[] vmArgs(RunMode runMode) {
            return new String[] {
                "-XX:+ArchiveDynamicProxies",
                "-Xlog:cds+dynamic+proxy:file=DynamicProxyTest.proxies." + runMode + ".log"
            };
        }
    }

    public static interface Foo {
        public Object bar();
    }

    public static interface Boo {
        public Object far();
    }

    static interface Coo {
        public Object car();
    }

    public static interface DefaultMethodWithUnlinkedClass {
        default Object doit(boolean flag) {
            if (flag) {
                Fruit f = Fruit.get();
                f.peel();
                return f;
            } else {
                return "Do not link Fruit class";
            }
        }
    }
}

class MyInvocationHandler implements InvocationHandler {
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        if (method.getName().equals("isLoggable")) {
            return Boolean.TRUE;
        } else {
            return method.getName() + "() in " + proxy.getClass() + " is called";
        }
    }
}

class DynamicProxyApp {
    static String[] classes = {
        "java.io.Serializable",
        "java.lang.annotation.Documented",
        "java.lang.annotation.Inherited",
        "java.lang.annotation.Retention",
        "java.lang.annotation.Target",
        "java.lang.Enum",
        "java.lang.Integer",
        "java.lang.management.BufferPoolMXBean",
        "java.lang.management.ClassLoadingMXBean",
        "java.lang.management.CompilationMXBean",
        "java.lang.management.GarbageCollectorMXBean",
        "java.lang.management.MemoryManagerMXBean",
        "java.lang.management.MemoryMXBean",
        "java.lang.management.MemoryPoolMXBean",
        "java.lang.management.OperatingSystemMXBean",
        "java.lang.management.PlatformLoggingMXBean",
        "java.lang.management.PlatformManagedObject",
        "java.lang.management.RuntimeMXBean",
        "java.lang.management.ThreadMXBean",
        "java.lang.Number",
        "java.lang.Object",
        "java.lang.Package$1PackageInfoProxy",
        "java.lang.reflect.Proxy",
        "java.lang.String",
        "java.time.LocalDate",
        "java.util.Collections$UnmodifiableMap",
        "java.util.Dictionary",
        "java.util.Hashtable",
        "java.util.Properties",
        "javax.management.NotificationBroadcaster",
        "javax.management.NotificationEmitter",
        "javax.naming.Referenceable",
        "javax.sql.CommonDataSource",
        "javax.sql.DataSource",
        "jdk.management.jfr.FlightRecorderMXBean",
    };

    static void checkRead(boolean expected, Module proxyModule, Module targetModule) {
        Asserts.assertEQ(proxyModule.canRead(targetModule), expected,
                         "(" + proxyModule + ") canRead (" +
                         targetModule + " of loader " + targetModule.getClassLoader() + ") = " +
                         proxyModule.canRead(targetModule) + ", but should be " + expected);
    }

    static boolean hasArchivedProxies;

    private static void checkArchivedProxy(Class c, boolean shouldBeArchived) {
        if (hasArchivedProxies) {
            // We can't use WhiteBox, which would disable full module graph. So we can only check the name.
            if (shouldBeArchived && !c.getName().contains("$Proxy0")) {
                throw new RuntimeException("Proxy class " + c + " does not seem to be archived");
            }
            if (!shouldBeArchived && c.getName().contains("$Proxy00")) {
                throw new RuntimeException("Proxy class " + c + " shouldn't be archived");
            }

            System.out.println(c + (shouldBeArchived ? " is " : " is not ") + "archived, as expected");
        }
    }

    public static void main(String args[]) {
        // We should have archived dynamic proxies after the static archive has been dumped.
        hasArchivedProxies = args[0].contains("PRODUCTION");

        // Create a Proxy for the java.util.Map interface
        System.out.println("================test 1: Proxy for java.util.Map");
        Map instance1 = (Map) Proxy.newProxyInstance(
            DynamicProxyApp.class.getClassLoader(),
            new Class[] { Map.class },
            new MyInvocationHandler());
        System.out.println(instance1.getClass());
        System.out.println(instance1.getClass().getClassLoader());
        System.out.println(instance1.getClass().getPackage());
        System.out.println(instance1.get("5678"));
        System.out.println(instance1.getClass().getModule());

        // Proxy$ProxyBuilder defines the proxy classes with a null protection domain, resulting in the
        // following behavior. This must be preserved by CDS.
        // TODO: after JDK-8322322 is implemented, add tests for Proxy creation by boot loader code.
        ProtectionDomain pd = instance1.getClass().getProtectionDomain();
        Asserts.assertNotNull(pd);
        Asserts.assertNull(pd.getClassLoader());
        Asserts.assertNull(pd.getCodeSource());


        Module dynModule = instance1.getClass().getModule();
        // instance1.getClass() should be in a dynamic module like jdk.proxy1, which
        // contains all proxies that implement only public interfaces for the app loader.

        // This module should be able to read the module that contains Map (java.base)
        checkRead(true,  dynModule, Map.class.getModule());

        // No proxies have been created for these modules yet, so they are't readable by dynModule yet.
        checkRead(false, dynModule, DynamicProxyApp.class.getModule());
        checkRead(false, dynModule, Filter.class.getModule());

        checkArchivedProxy(instance1.getClass(), true);

        System.out.println("=================test 2: Proxy for both Map and Runnable");
        Map instance2 = (Map) Proxy.newProxyInstance(
            DynamicProxyApp.class.getClassLoader(),
            new Class[] { Map.class, Runnable.class },
            new MyInvocationHandler());
        System.out.println(instance2.getClass());
        System.out.println(instance2.getClass().getPackage());
        System.out.println(instance2.get("5678"));

        checkArchivedProxy(instance2.getClass(), true);

        System.out.println("=================test 3: Proxy for an interface in unnamed module");
        DynamicProxyTest.Foo instance3 = (DynamicProxyTest.Foo) Proxy.newProxyInstance(
            DynamicProxyApp.class.getClassLoader(),
            new Class[] { DynamicProxyTest.Foo.class, Runnable.class },
            new MyInvocationHandler());
        System.out.println(instance3.getClass());
        System.out.println(instance3.getClass().getPackage());
        System.out.println(instance3.bar());
        System.out.println(instance3.getClass().getModule());

        // Now, dynModule should have access to the UNNAMED module
        Asserts.assertSame(instance3.getClass().getModule(), dynModule, "proxies of only public interfaces should go in the same module");
        checkRead(true, dynModule, DynamicProxyApp.class.getModule());
        checkArchivedProxy(instance3.getClass(), true);

        if (!args[0].equals("CLASSLIST") && !args[0].startsWith("TRAINING")) {
            // This dynamic proxy is not loaded in the training run, so it shouldn't be
            // archived.
            System.out.println("=================test 3: Proxy (unarchived) for an interface in unnamed module");
            DynamicProxyTest.Boo instance3a = (DynamicProxyTest.Boo) Proxy.newProxyInstance(
                DynamicProxyApp.class.getClassLoader(),
                new Class[] { DynamicProxyTest.Boo.class, Runnable.class },
                new MyInvocationHandler());
            System.out.println(instance3a.getClass());
            System.out.println(instance3a.getClass().getPackage());
            System.out.println(instance3a.far());

            Asserts.assertSame(instance3.getClass().getPackage(), instance3a.getClass().getPackage(), "should be the same package");
            checkArchivedProxy(instance3a.getClass(), false);
        }

        System.out.println("=================test 4: Proxy for java.util.logging.Filter in java.logging module");
        Filter instance4 = (Filter) Proxy.newProxyInstance(
            DynamicProxyApp.class.getClassLoader(),
            new Class[] { Filter.class },
            new MyInvocationHandler());
        System.out.println(instance4.getClass());
        System.out.println(instance4.getClass().getPackage());
        System.out.println(instance4.isLoggable(null));

        // Now dynModule should have access to the java.logging module
        Asserts.assertSame(instance4.getClass().getModule(), dynModule, "proxies of only public interfaces should go in the same module");
        checkRead(true, dynModule, Filter.class.getModule());

        System.out.println("=================test 5: Proxy for non-public interface not in any package");
        DynamicProxyTest.Coo instance5 = (DynamicProxyTest.Coo) Proxy.newProxyInstance(
            DynamicProxyApp.class.getClassLoader(),
            new Class[] { DynamicProxyTest.Coo.class},
            new MyInvocationHandler());
        System.out.println(instance5.getClass());
        System.out.println(instance5.getClass().getPackage());
        System.out.println(instance5.getClass().getModule());
        System.out.println(instance5.car());

        Asserts.assertSame(instance5.getClass().getPackage(), DynamicProxyApp.class.getPackage(), "should be the same package");
        Asserts.assertSame(instance5.getClass().getModule(), DynamicProxyApp.class.getModule(), "should be the same module");
        checkArchivedProxy(instance5.getClass(), false); // CDS doesn't (yet) support such proxies.

        System.out.println("=================test 6: Proxy for non-public interface in a package");
        {
            Class<?> c = testpkg.PublicTester.test();
            checkArchivedProxy(c, false); // CDS doesn't (yet) support such proxies.
        }

        System.out.println("=================test 7: Proxy with a default method that references unlinked classes");
        DynamicProxyTest.DefaultMethodWithUnlinkedClass instance7 = (DynamicProxyTest.DefaultMethodWithUnlinkedClass) Proxy.newProxyInstance(
            DynamicProxyApp.class.getClassLoader(),
            new Class[] { DynamicProxyTest.DefaultMethodWithUnlinkedClass.class, Runnable.class },
            new MyInvocationHandler());
        System.out.println(instance7.getClass());
        System.out.println(instance7.getClass().getPackage());
        System.out.println(instance7.doit(false));
        System.out.println(instance7.getClass().getModule());

        Asserts.assertSame(instance7.getClass().getModule(), dynModule, "proxies of only public interfaces should go in the same module");
        checkArchivedProxy(instance7.getClass(), true);

        // Get annotations -- this will cause a bunch of proxies to be generated
        ClassLoader loader = DynamicProxyApp.class.getClassLoader();
        for (String className: classes) {
            try {
                Class.forName(className, false, loader).getDeclaredAnnotations();
            } catch (Throwable t) {}
        }
    }
}

class Fruit {
    void peel() {}
    static Fruit get() {
        return new Apple();
    }
}

// This class is never included in the JAR files. This means that the Fruit class cannot
// be verified. But our test case doesn't link the Fruit class, so it should run without
// any error.
class Apple extends Fruit {}
