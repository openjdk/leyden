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
import java.lang.classfile.constantpool.ConstantPoolBuilder;
import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.IllegalClassFormatException;
import java.lang.instrument.Instrumentation;
import java.lang.instrument.UnmodifiableClassException;
import java.security.ProtectionDomain;

public class RedefineAllAgent implements ClassFileTransformer {
    /* FIXME: if REDEFINE_BOOT_CLASSES is set to true:
       ======================================================================
       #  Internal Error (/jdk3/le5/open/src/hotspot/share/code/nmethod.cpp:3239), pid=3831637, tid=3831829
       #  assert(!method->is_old()) failed: Should not be installing old methods

       Current CompileTask:
       C2:12190 6444         4       com.sun.tools.javac.code.Symtab::getClass (42 bytes)

       Stack: [0x00007a965d8fd000,0x00007a965d9fd000],  sp=0x00007a965d9f85e0,  free space=1005k
       Native frames: (J=compiled Java code, A=AOT compiled, P=AOT preloaded, j=interpreted, Vv=VM code, C=native code)
       V  [libjvm.so+0x15a8c5a]  VerifyMetadataClosure::do_metadata(Metadata*)+0x5a  (nmethod.cpp:3239)
       V  [libjvm.so+0x15996d1]  nmethod::metadata_do(MetadataClosure*)+0x161  (nmethod.cpp:2640)
       V  [libjvm.so+0x159d870]  nmethod::verify()+0x1f0  (nmethod.cpp:3288)
       V  [libjvm.so+0x15a3f3e]  nmethod::new_nmethod(methodHandle const&, int, int, CodeOffsets*, int, DebugInformationRecorder*, Dependencies*, CodeBuffer*, int, OopMapSet*, ExceptionHandlerTable*, ImplicitExceptionTable*, AbstractCompiler*, CompLevel, nmethod::Flags)+0x34e  (nmethod.cpp:1167)
       V  [libjvm.so+0xa3f277]  ciEnv::register_method(ciMethod*, int, CodeOffsets*, int, CodeBuffer*, int, OopMapSet*, ExceptionHandlerTable*, ImplicitExceptionTable*, AbstractCompiler*, bool, bool, bool, bool, bool, bool, int, bool)+0x387  (ciEnv.cpp:1235)
       V  [libjvm.so+0x16536b0]  PhaseOutput::install_code(ciMethod*, int, AbstractCompiler*, bool, bool)+0x150  (output.cpp:3212)
       V  [libjvm.so+0xb74fdd]  Compile::Code_Gen()+0x68d  (compile.cpp:3180)
       V  [libjvm.so+0xb7a6b4]  Compile::Compile(ciEnv*, ciMethod*, int, Options, DirectiveSet*)+0x20d4  (compile.cpp:916)
       V  [libjvm.so+0x9aca91]  C2Compiler::compile_method(ciEnv*, ciMethod*, int, bool, DirectiveSet*)+0x231  (c2compiler.cpp:156)
       V  [libjvm.so+0xb88281]  CompileBroker::invoke_compiler_on_method(CompileTask*)+0xc51  (compileBroker.cpp:2220)
       V  [libjvm.so+0xb88cb0]  CompileBroker::compiler_thread_loop()+0x4f0  (compileBroker.cpp:1925)
       V  [libjvm.so+0x10a5f7e]  JavaThread::thread_main_inner()+0xee  (javaThread.cpp:651)
       V  [libjvm.so+0x19b9a0a]  Thread::call_run()+0xba  (thread.cpp:243)
       V  [libjvm.so+0x16318b3]  thread_native_entry(Thread*)+0x113  (os_linux.cpp:931)
       C  [libc.so.6+0x9caa4]
       ======================================================================
     */
    private static final boolean REDEFINE_BOOT_CLASSES = false;

    public static void premain(String agentArguments, Instrumentation inst) {
        inst.addTransformer(new RedefineAllAgent(), /*canRetransform=*/true);

        for (Class c : inst.getAllLoadedClasses()) {
            if ((c.getClassLoader() != null || REDEFINE_BOOT_CLASSES) && !c.isArray() && !c.isHidden() && inst.isModifiableClass(c)) {
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
        if (loader != null || REDEFINE_BOOT_CLASSES) {
            try {
                System.out.println((classBeingRedefined == null ? "retransforming " : "redefining ") + name);
                ClassFile cf = ClassFile.of();
                ClassModel model = cf.parse(buffer);
                ConstantPoolBuilder cp = ConstantPoolBuilder.of(model);
                cp.utf8Entry("Hello");
                buffer = cf.build(model.thisClass(), cp,
                                  cb -> cb.transform(model, ClassTransform.ACCEPT_ALL));
            } catch (Throwable t) {
                t.printStackTrace();
                throw new RuntimeException("Unexpected", t);
            }
            return buffer;
        }
        return null;
    }
}
