/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassModel;
import java.lang.classfile.ClassTransform;
import java.lang.classfile.CodeModel;
import java.lang.classfile.MethodBuilder;
import java.lang.classfile.MethodElement;
import java.lang.classfile.MethodModel;
import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.IllegalClassFormatException;
import java.lang.instrument.Instrumentation;
import java.lang.instrument.UnmodifiableClassException;
import java.security.ProtectionDomain;

// Redefine RedefineHotMethodApp::increment to: { return 34; }
public class RedefineHotMethodAgent implements ClassFileTransformer {
    private static final String classToRedefine = "RedefineHotMethodApp";

    public static void premain(String agentArguments, Instrumentation inst) {
        inst.addTransformer(new RedefineHotMethodAgent(), /*canRetransform=*/true);

        for (Class c : inst.getAllLoadedClasses()) {
            if (c.getName().equals(classToRedefine)) {
                try {
                    inst.retransformClasses(c);
                    System.out.println("========= Success: " + c.getName());
                } catch (UnmodifiableClassException e) {
                    System.out.println("========== Failed: " + c.getName());
                    e.printStackTrace(System.out);
                }
            }
        }
    }

    public byte[] transform(ClassLoader loader, String name, Class<?> classBeingRedefined,
                            ProtectionDomain pd, byte[] buffer) throws IllegalClassFormatException {
        if (name.equals(classToRedefine)) {
            try {
                System.out.println((classBeingRedefined == null ? "retransforming " : "redefining ") + name);
                ClassFile cf = ClassFile.of();
                ClassModel model = cf.parse(buffer);

                ClassTransform transform =
                    ClassTransform.transformingMethods((MethodModel method) -> method.methodName().equalsString("increment"),
                                                       RedefineHotMethodAgent::replaceMethodBody);
                return cf.transformClass(model, transform);
            } catch (Throwable t) {
                t.printStackTrace();
                throw new RuntimeException("Unexpected", t);
            }
        }
        return null;
    }


    private static void replaceMethodBody(MethodBuilder builder, MethodElement element) {
        if (element instanceof CodeModel) {
            builder.withCode(code -> code
                .bipush(34)
                .ireturn()
            );
        } else {
            builder.with(element);
        }
    }
}
