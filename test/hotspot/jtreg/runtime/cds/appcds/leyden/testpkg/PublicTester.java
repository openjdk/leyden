/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

package testpkg;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

import jdk.test.lib.Asserts;

public class PublicTester {
    public static Class<?> test() {
        NonPublicInterface npi = (NonPublicInterface) Proxy.newProxyInstance(
            PublicTester.class.getClassLoader(), 
            new Class[] { NonPublicInterface.class, Runnable.class },
            new MyInvocationHandler2());
        System.out.println(npi.getClass());
        System.out.println(npi.getClass().getPackage());
        System.out.println(npi.getClass().getModule());
        System.out.println(PublicTester.class.getModule());
        System.out.println(npi.m());

        Asserts.assertSame(PublicTester.class.getModule(), npi.getClass().getModule(),
                           "proxies for non-public interfaces should be in same module as the interface");
        Asserts.assertSame(PublicTester.class.getPackage(), npi.getClass().getPackage(),
                           "proxies for non-public interfaces should be in same package as the interface");

        return npi.getClass();
    }
}

class MyInvocationHandler2 implements InvocationHandler {
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        return method.getName() + " in() " + proxy.getClass() + " is called";
    }
}

interface NonPublicInterface {
    Object m();
}
