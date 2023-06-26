/*
 * Copyright (c) 2012, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 */

 package java.lang.invoke;

 import jdk.internal.misc.CDS;
 import jdk.internal.org.objectweb.asm.*;
 import jdk.internal.util.ClassFileDumper;
 import sun.invoke.util.BytecodeDescriptor;
 import sun.invoke.util.VerifyAccess;
 import sun.security.action.GetPropertyAction;
 import sun.security.action.GetBooleanAction;

 import java.io.FilePermission;
 import java.io.Serializable;
 import java.lang.constant.ClassDesc;
 import java.lang.constant.ConstantDescs;
 import java.lang.constant.DirectMethodHandleDesc;
 import java.lang.constant.MethodHandleDesc;
 import java.lang.constant.MethodTypeDesc;
 import java.lang.invoke.MethodHandles.Lookup;
 import java.lang.reflect.Modifier;
 import java.nio.file.Path;
 import java.security.AccessController;
 import java.security.PrivilegedAction;
 import java.util.LinkedHashSet;
 import java.util.concurrent.atomic.AtomicInteger;
 import java.util.PropertyPermission;
 import java.util.Set;

 import static java.lang.invoke.MethodHandleStatics.CLASSFILE_VERSION;
 import static java.lang.invoke.MethodHandles.Lookup.ClassOption.NESTMATE;
 import static java.lang.invoke.MethodHandles.Lookup.ClassOption.STRONG;
 import static java.lang.invoke.MethodType.methodType;
 import static jdk.internal.org.objectweb.asm.Opcodes.*;

 /**
  * The class generator for the Lambda metafactory implementation
  * which dynamically creates an inner-class-like class per lambda callsite.
  *
  * This is separated out to allow jlink to use it as well
  *
  * @see InnerClassLambdaMetafactory
  */
 /* package */ final class InnerClassLambdaGenerator {
	 private static final String METHOD_DESCRIPTOR_VOID = Type.getMethodDescriptor(Type.VOID_TYPE);
	 private static final String JAVA_LANG_OBJECT = "java/lang/Object";
	 private static final String NAME_CTOR = "<init>"; //keep
	 private static final String LAMBDA_INSTANCE_FIELD = "LAMBDA_INSTANCE$";

	 //Serialization support
	 private static final String NAME_SERIALIZED_LAMBDA = "java/lang/invoke/SerializedLambda";
	 private static final String NAME_NOT_SERIALIZABLE_EXCEPTION = "java/io/NotSerializableException";
	 private static final String DESCR_METHOD_WRITE_REPLACE = "()Ljava/lang/Object;";
	 private static final String DESCR_METHOD_WRITE_OBJECT = "(Ljava/io/ObjectOutputStream;)V";
	 private static final String DESCR_METHOD_READ_OBJECT = "(Ljava/io/ObjectInputStream;)V";

	 private static final String NAME_METHOD_WRITE_REPLACE = "writeReplace";
	 private static final String NAME_METHOD_READ_OBJECT = "readObject";
	 private static final String NAME_METHOD_WRITE_OBJECT = "writeObject";

	 private static final String DESCR_CLASS = "Ljava/lang/Class;";
	 private static final String DESCR_STRING = "Ljava/lang/String;";
	 private static final String DESCR_OBJECT = "Ljava/lang/Object;";
	 private static final String DESCR_CTOR_SERIALIZED_LAMBDA
			 = "(" + DESCR_CLASS + DESCR_STRING + DESCR_STRING + DESCR_STRING + "I"
			 + DESCR_STRING + DESCR_STRING + DESCR_STRING + DESCR_STRING + "[" + DESCR_OBJECT + ")V";

	 private static final String DESCR_CTOR_NOT_SERIALIZABLE_EXCEPTION = "(Ljava/lang/String;)V";
	 private static final String[] SER_HOSTILE_EXCEPTIONS = new String[] {NAME_NOT_SERIALIZABLE_EXCEPTION};

	 private static final String[] EMPTY_STRING_ARRAY = new String[0];

	 // Used to ensure that each spun class name is unique
	 private static final AtomicInteger counter = new AtomicInteger();

	 // For dumping generated classes to disk, for debugging purposes
	 private static final ClassFileDumper lambdaProxyClassFileDumper;

	 private static final boolean disableEagerInitialization;

	 // condy to load implMethod from class data
	 private static final ConstantDynamic implMethodCondy;

	 static {
		// To dump the lambda proxy classes, set this system property:
        //    -Djdk.invoke.LambdaMetafactory.dumpProxyClassFiles
        // or -Djdk.invoke.LambdaMetafactory.dumpProxyClassFiles=true
        final String dumpProxyClassesKey = "jdk.invoke.LambdaMetafactory.dumpProxyClassFiles";
        lambdaProxyClassFileDumper = ClassFileDumper.getInstance(dumpProxyClassesKey, Path.of("DUMP_LAMBDA_PROXY_CLASS_FILES"));

		 final String disableEagerInitializationKey = "jdk.internal.lambda.disableEagerInitialization";
		 disableEagerInitialization = GetBooleanAction.privilegedGetProperty(disableEagerInitializationKey);

		 // condy to load implMethod from class data
		 MethodType classDataMType = methodType(Object.class, MethodHandles.Lookup.class, String.class, Class.class);
		 Handle classDataBsm = new Handle(H_INVOKESTATIC, Type.getInternalName(MethodHandles.class), "classData",
										  classDataMType.descriptorString(), false);
		 implMethodCondy = new ConstantDynamic(ConstantDescs.DEFAULT_NAME, MethodHandle.class.descriptorString(), classDataBsm);
	 }

	 private static String lambdaClassName(ClassDesc targetClassDesc) {
		 // ClassDesc can't be a  hidden class as it comes from the bytecode
		 ClassDesc lambdaDesc = ClassDesc.of(targetClassDesc.packageName(), targetClassDesc.displayName()+ "$$Lambda$" + counter.incrementAndGet());
		 String descString = strippedDescriptor(lambdaDesc); //.descriptorString().substring(1, lambdaDesc.descriptorString().length() - 1);
		 return descString;
	 }

	 //Keep
	 private final ClassWriter cw;                    // ASM class writer
	 private final String[] argNames;                 // Generated names for the constructor arguments
	 private final String[] argDescs;                 // Type descriptors for the constructor arguments
	 private final String lambdaClassName;            // Generated name for the generated class "X$$Lambda$1"
	 private final MethodTypeDesc constructorTypeDesc;        // Generated class constructor type "(CC)void"
	 final int implKind;                       // Invocation kind for implementation "5"=invokevirtual
	 private final String implMethodClassName;        // Name of type containing implementation "CC"
	 private final String implMethodName;             // Name of implementation method "impl"
	 private final String implMethodDesc;             // Type descriptor for implementation methods "(I)Ljava/lang/String;"
	 final MethodTypeDesc[] altMethodDescs;            // Signatures of additional methods to bridge


	 private final boolean useImplMethodHandle;       // use MethodHandle invocation instead of symbolic bytecode invocation

	 String[] interfaceNames;
	 boolean isSerializable;
	 boolean accidentallySerializable;
	 MethodTypeDesc factoryTypeDesc;
	 String interfaceMethodName;
	 MethodTypeDesc interfaceMethodTypeDesc;
	 MethodTypeDesc dynamicMethodTypeDesc;
	 ClassDesc targetClassDesc;
	 DirectMethodHandleDesc implMHDesc;
	 MethodTypeDesc invokeTypeDesc;  // Equivalent to `implInfo.getMethodType()`, not `DirectMethodHandleDesc.invocationType` - leading receiver is different

	 public InnerClassLambdaGenerator(String interfaceMethodName, MethodTypeDesc interfaceMethodTypeDesc, MethodTypeDesc dynamicMethodTypeDesc, ClassDesc targetClassDesc, String[] interfaceNames, MethodTypeDesc factoryTypeDesc, boolean isSerializable, boolean accidentallySerializable, DirectMethodHandleDesc implMHDesc, int implKind, MethodTypeDesc[] altMethodDescs) {
		 this.interfaceMethodName = interfaceMethodName;
		 this.interfaceMethodTypeDesc = interfaceMethodTypeDesc;
		 this.dynamicMethodTypeDesc = dynamicMethodTypeDesc;
		 this.targetClassDesc = targetClassDesc;
		 lambdaClassName = lambdaClassName(targetClassDesc);
		 this.interfaceNames = interfaceNames;
		 this.isSerializable = isSerializable;
		 this.accidentallySerializable = accidentallySerializable;
		 this.factoryTypeDesc = factoryTypeDesc;
		 this.implKind = implKind;
		 this.altMethodDescs = altMethodDescs;
		 this.implMHDesc = implMHDesc;

		 String implMethodClassDescString = implMHDesc.owner().descriptorString();
		 implMethodClassName = strippedDescriptor(implMHDesc.owner()); //implMethodClassDescString.substring(1, implMethodClassDescString.length() - 1); // Remove the "L" & ";"
		 implMethodName = implMHDesc.methodName();
		 // TODO: should this be a method on DirectMethodHandleDesc?  maybe `MT lookupType()`?
		 MethodTypeDesc invokeDesc = switch(implMHDesc.kind()) {
			 case VIRTUAL,
				  SPECIAL,
				  INTERFACE_SPECIAL -> implMHDesc.invocationType().dropParameterTypes(0,1);
			 case INTERFACE_VIRTUAL -> implMHDesc.invocationType().dropParameterTypes(0,1);
			 case CONSTRUCTOR -> implMHDesc.invocationType().changeReturnType(ConstantDescs.CD_void);
			 default -> implMHDesc.invocationType();
		 };
		 invokeTypeDesc = invokeDesc;
		 implMethodDesc = invokeDesc.descriptorString();
		 useImplMethodHandle = false; //TODO - figure out the right answer here

		 constructorTypeDesc = factoryTypeDesc.changeReturnType(ConstantDescs.CD_void);

		 int parameterCount = factoryTypeDesc.parameterCount();
		 if (parameterCount > 0) {
			 argNames = new String[parameterCount];
			 argDescs = new String[parameterCount];
			 for (int i = 0; i < parameterCount; i++) {
				 argNames[i] = "arg$" + (i + 1);
				 argDescs[i] = factoryTypeDesc.parameterType(i).descriptorString();
			 }
		 } else {
			 argNames = argDescs = EMPTY_STRING_ARRAY;
		 }

		 cw = new ClassWriter(ClassWriter.COMPUTE_MAXS);
	 }

	 public byte[] noop() { return null; };

	 @SuppressWarnings("removal")
	 public byte[] generateInnerClassBytecode() throws LambdaConversionException {

		 cw.visit(CLASSFILE_VERSION, ACC_SUPER + ACC_FINAL + ACC_SYNTHETIC,
					 lambdaClassName, null,
					 JAVA_LANG_OBJECT, interfaceNames);

		 // Generate final fields to be filled in by constructor
		 for (int i = 0; i < argDescs.length; i++) {
			 FieldVisitor fv = cw.visitField(ACC_PRIVATE + ACC_FINAL,
											 argNames[i],
											 argDescs[i],
											 null, null);
			 fv.visitEnd();
		 }

		 generateConstructor();
		 generateThunk();

		 if (factoryTypeDesc.parameterCount() == 0 && disableEagerInitialization) {
			 generateClassInitializer();
		 }

		 // Forward the SAM method
		 MethodVisitor mv = cw.visitMethod(ACC_PUBLIC, interfaceMethodName,
											 interfaceMethodTypeDesc.descriptorString(), null, null);
		 new ForwardingMethodGenerator(mv).generate(interfaceMethodTypeDesc);

		 // Forward the altMethods
		 if (altMethodDescs != null) {
			 for (MethodTypeDesc mtDesc : altMethodDescs) {
				 mv = cw.visitMethod(ACC_PUBLIC, interfaceMethodName,
									 mtDesc.descriptorString(), null, null);
				 new ForwardingMethodGenerator(mv).generate(mtDesc);
			 }
		 }

		 if (isSerializable)
			 generateSerializationFriendlyMethods();
		 else if (accidentallySerializable)
			 generateSerializationHostileMethods();

		 cw.visitEnd();

		 final byte[] classBytes = cw.toByteArray();
		 // If requested, dump out to a file for debugging purposes
		 // if (dumper != null) {
		 //     AccessController.doPrivileged(new PrivilegedAction<>() {
		 //         @Override
		 //         public Void run() {
		 //             dumper.dumpClass(lambdaClassName, classBytes);
		 //             return null;
		 //         }
		 //     }, null,
		 //     new FilePermission("<<ALL FILES>>", "read, write"),
		 //     // createDirectories may need it
		 //     new PropertyPermission("user.dir", "read"));
		 // }
		 return classBytes;
	 }

	 /**
	  * Generate a static field and a static initializer that sets this field to an instance of the lambda
	  */
	 private void generateClassInitializer() {
		 String lambdaTypeDescriptor = factoryTypeDesc.returnType().descriptorString();

		 // Generate the static final field that holds the lambda singleton
		 FieldVisitor fv = cw.visitField(ACC_PRIVATE | ACC_STATIC | ACC_FINAL,
				 LAMBDA_INSTANCE_FIELD, lambdaTypeDescriptor, null, null);
		 fv.visitEnd();

		 // Instantiate the lambda and store it to the static final field
		 MethodVisitor clinit = cw.visitMethod(ACC_STATIC, "<clinit>", "()V", null, null);
		 clinit.visitCode();

		 clinit.visitTypeInsn(NEW, lambdaClassName);
		 clinit.visitInsn(Opcodes.DUP);
		 assert factoryTypeDesc.parameterCount() == 0;
		 clinit.visitMethodInsn(INVOKESPECIAL, lambdaClassName, NAME_CTOR, constructorTypeDesc.descriptorString(), false);
		 clinit.visitFieldInsn(PUTSTATIC, lambdaClassName, LAMBDA_INSTANCE_FIELD, lambdaTypeDescriptor);

		 clinit.visitInsn(RETURN);
		 clinit.visitMaxs(-1, -1);
		 clinit.visitEnd();
	 }

	 /**
	  * Generate the constructor for the class
	  */
	 private void generateConstructor() {
		 // Generate constructor
		 MethodVisitor ctor = cw.visitMethod(ACC_PRIVATE, NAME_CTOR,
											 constructorTypeDesc.descriptorString(), null, null); // fix ctorType
		 ctor.visitCode();
		 ctor.visitVarInsn(ALOAD, 0);
		 ctor.visitMethodInsn(INVOKESPECIAL, JAVA_LANG_OBJECT, NAME_CTOR,
							  METHOD_DESCRIPTOR_VOID, false);
		 int parameterCount = factoryTypeDesc.parameterCount();
		 for (int i = 0, lvIndex = 0; i < parameterCount; i++) {
			 ctor.visitVarInsn(ALOAD, 0);
			 ClassDesc argType = factoryTypeDesc.parameterType(i);
			 ctor.visitVarInsn(getLoadOpcode(argType), lvIndex + 1);
			 lvIndex += getParameterSize(argType);
			 ctor.visitFieldInsn(PUTFIELD, lambdaClassName, argNames[i], argDescs[i]);
		 }
		 ctor.visitInsn(RETURN);
		 // Maxs computed by ClassWriter.COMPUTE_MAXS, these arguments ignored
		 ctor.visitMaxs(-1, -1);
		 ctor.visitEnd();
	 }

	 /**
	  * Generate a thunk with the same signature as the invokedyanmic
	  */
	 private void generateThunk() {
		 MethodVisitor thunk = cw.visitMethod(ACC_PRIVATE | ACC_STATIC, "thunk",
											 factoryTypeDesc.descriptorString(), null, null); // fix ctorType
		 thunk.visitCode();
		 thunk.visitTypeInsn(NEW, lambdaClassName);
		 thunk.visitInsn(DUP);
		 // re-push parameters as args
		 int parameterCount = factoryTypeDesc.parameterCount();
		 for (int i = 0, lvIndex = 0; i < parameterCount; i++) {
			 ClassDesc argType = factoryTypeDesc.parameterType(i);
			 thunk.visitVarInsn(getLoadOpcode(argType), lvIndex);
			 lvIndex += getParameterSize(argType);
		 }
		 // call ctor
		 thunk.visitMethodInsn(INVOKESPECIAL, lambdaClassName, NAME_CTOR,
			 constructorTypeDesc.descriptorString(), false);

		 //return instance
		 thunk.visitInsn(ARETURN);
		 // Maxs computed by ClassWriter.COMPUTE_MAXS, these arguments ignored
		 thunk.visitMaxs(-1, -1);
		 thunk.visitEnd();
	 }

	 /**
	  * Remove the leading 'L' & trailing ';' from a descriptor String
	  * for use in contexts that want the '/' form of the name rather than
	  * the '.' form.
	  *
	  * If it's an not an 'L' type or is an array, just return the descriptor.
	  *
	  * @param cd The ClassDesc to get the descriptor from
	  * @return the descriptor without the leading 'L' & trailing ';'
	  */
	 private static String strippedDescriptor(ClassDesc cd) {
		 String desc = cd.descriptorString();
		 if (desc.charAt(0) == 'L') {
			 desc = desc.substring(1, desc.length() - 1);
		 }
		 return desc;
	 }

	 /**
	  * Generate a writeReplace method that supports serialization
	  */
	 private void generateSerializationFriendlyMethods() {
		 TypeConvertingMethodAdapter mv
				 = new TypeConvertingMethodAdapter(
					 cw.visitMethod(ACC_PRIVATE + ACC_FINAL,
					 NAME_METHOD_WRITE_REPLACE, DESCR_METHOD_WRITE_REPLACE,
					 null, null));

		 mv.visitCode();
		 mv.visitTypeInsn(NEW, NAME_SERIALIZED_LAMBDA);
		 mv.visitInsn(DUP);
		 mv.visitLdcInsn(targetClassDesc.descriptorString());
		 mv.visitLdcInsn(strippedDescriptor(factoryTypeDesc.returnType()));
		 mv.visitLdcInsn(interfaceMethodName);
		 mv.visitLdcInsn(interfaceMethodTypeDesc.descriptorString());
		 mv.visitLdcInsn(implKind);

		 mv.visitLdcInsn(strippedDescriptor(implMHDesc.owner()));
		 mv.visitLdcInsn(implMHDesc.methodName());
		 mv.visitLdcInsn(invokeTypeDesc.descriptorString()); //maybe? was invocationType()
		 mv.visitLdcInsn(dynamicMethodTypeDesc.descriptorString());
		 mv.iconst(argDescs.length);
		 mv.visitTypeInsn(ANEWARRAY, JAVA_LANG_OBJECT);
		 for (int i = 0; i < argDescs.length; i++) {
			 mv.visitInsn(DUP);
			 mv.iconst(i);
			 mv.visitVarInsn(ALOAD, 0);
			 mv.visitFieldInsn(GETFIELD, lambdaClassName, argNames[i], argDescs[i]);
			 mv.boxIfTypePrimitive(Type.getType(argDescs[i]));
			 mv.visitInsn(AASTORE);
		 }
		 mv.visitMethodInsn(INVOKESPECIAL, NAME_SERIALIZED_LAMBDA, NAME_CTOR,
				 DESCR_CTOR_SERIALIZED_LAMBDA, false);
		 mv.visitInsn(ARETURN);
		 // Maxs computed by ClassWriter.COMPUTE_MAXS, these arguments ignored
		 mv.visitMaxs(-1, -1);
		 mv.visitEnd();
	 }

	 /**
	  * Generate a readObject/writeObject method that is hostile to serialization
	  */
	 private void generateSerializationHostileMethods() {
		 MethodVisitor mv = cw.visitMethod(ACC_PRIVATE + ACC_FINAL,
										   NAME_METHOD_WRITE_OBJECT, DESCR_METHOD_WRITE_OBJECT,
										   null, SER_HOSTILE_EXCEPTIONS);
		 mv.visitCode();
		 mv.visitTypeInsn(NEW, NAME_NOT_SERIALIZABLE_EXCEPTION);
		 mv.visitInsn(DUP);
		 mv.visitLdcInsn("Non-serializable lambda");
		 mv.visitMethodInsn(INVOKESPECIAL, NAME_NOT_SERIALIZABLE_EXCEPTION, NAME_CTOR,
							DESCR_CTOR_NOT_SERIALIZABLE_EXCEPTION, false);
		 mv.visitInsn(ATHROW);
		 mv.visitMaxs(-1, -1);
		 mv.visitEnd();

		 mv = cw.visitMethod(ACC_PRIVATE + ACC_FINAL,
							 NAME_METHOD_READ_OBJECT, DESCR_METHOD_READ_OBJECT,
							 null, SER_HOSTILE_EXCEPTIONS);
		 mv.visitCode();
		 mv.visitTypeInsn(NEW, NAME_NOT_SERIALIZABLE_EXCEPTION);
		 mv.visitInsn(DUP);
		 mv.visitLdcInsn("Non-serializable lambda");
		 mv.visitMethodInsn(INVOKESPECIAL, NAME_NOT_SERIALIZABLE_EXCEPTION, NAME_CTOR,
							DESCR_CTOR_NOT_SERIALIZABLE_EXCEPTION, false);
		 mv.visitInsn(ATHROW);
		 mv.visitMaxs(-1, -1);
		 mv.visitEnd();
	 }

	 /**
	  * This class generates a method body which calls the lambda implementation
	  * method, converting arguments, as needed.
	  */
	 private class ForwardingMethodGenerator extends TypeConvertingMethodAdapter {

		 ForwardingMethodGenerator(MethodVisitor mv) {
			 super(mv);
		 }

		 void generate(MethodTypeDesc methodTypeDesc) {
			 visitCode();

			 if (implKind == MethodHandleInfo.REF_newInvokeSpecial) {
				 visitTypeInsn(NEW, implMethodClassName);
				 visitInsn(DUP);
			 }
			 if (useImplMethodHandle) {
				 visitLdcInsn(implMethodCondy);
			 }
			 for (int i = 0; i < argNames.length; i++) {
				 visitVarInsn(ALOAD, 0);
				 visitFieldInsn(GETFIELD, lambdaClassName, argNames[i], argDescs[i]);
			 }

			 convertArgumentTypes(methodTypeDesc);

			 if (useImplMethodHandle) {
				 MethodTypeDesc mtypedesc = invokeTypeDesc; // was implMHDesc.invocationType(); //maybe
				 if (implKind != MethodHandleInfo.REF_invokeStatic) {
					 mtypedesc = mtypedesc.insertParameterTypes(0, implMHDesc.owner());
				 }
				 visitMethodInsn(INVOKEVIRTUAL, "java/lang/invoke/MethodHandle",
								 "invokeExact", mtypedesc.descriptorString(), false);
			 } else {
				 // Invoke the method we want to forward to
				 visitMethodInsn(invocationOpcode(), implMethodClassName,
								 implMethodName, implMethodDesc,
								 implMHDesc.isOwnerInterface());
			 }
			 // Convert the return value (if any) and return it
			 // Note: if adapting from non-void to void, the 'return'
			 // instruction will pop the unneeded result
			 ClassDesc implReturnClassDesc = implMHDesc.invocationType().returnType(); //implMethodTypeDesc.returnType();
			 ClassDesc samReturnClassDesc = methodTypeDesc.returnType();
			 convertType(implReturnClassDesc, samReturnClassDesc, samReturnClassDesc);
			 visitInsn(getReturnOpcode(samReturnClassDesc));
			 // Maxs computed by ClassWriter.COMPUTE_MAXS,these arguments ignored
			 visitMaxs(-1, -1);
			 visitEnd();
		 }

		 private void convertArgumentTypes(MethodTypeDesc samType) {
			 int lvIndex = 0;
			 int samParametersLength = samType.parameterCount();
			 int captureArity = factoryTypeDesc.parameterCount();
			 for (int i = 0; i < samParametersLength; i++) {
				 ClassDesc argType = samType.parameterType(i);
				 visitVarInsn(getLoadOpcode(argType), lvIndex + 1);
				 lvIndex += getParameterSize(argType);
				 convertType(argType, implMHDesc.invocationType().parameterType(captureArity + i), dynamicMethodTypeDesc.parameterType(i)); //maybe - looks right based on Abstractvalidation...
			 }
		 }

		 private int invocationOpcode() throws InternalError {
			 return switch (implKind) {
				 case MethodHandleInfo.REF_invokeStatic     -> INVOKESTATIC;
				 case MethodHandleInfo.REF_newInvokeSpecial -> INVOKESPECIAL;
				 case MethodHandleInfo.REF_invokeVirtual    -> INVOKEVIRTUAL;
				 case MethodHandleInfo.REF_invokeInterface  -> INVOKEINTERFACE;
				 case MethodHandleInfo.REF_invokeSpecial    -> INVOKESPECIAL;
				 default -> throw new InternalError("Unexpected invocation kind: " + implKind);
			 };
		 }
	 }

	 static int getParameterSize(Class<?> c) {
		 if (c == Void.TYPE) {
			 return 0;
		 } else if (c == Long.TYPE || c == Double.TYPE) {
			 return 2;
		 }
		 return 1;
	 }

	 static int getParameterSize(ClassDesc c) {
		 if (c.equals(ConstantDescs.CD_void)) {
			 return 0;
		 } else if (c.equals(ConstantDescs.CD_long) || c.equals(ConstantDescs.CD_double)) {
			 return 2;
		 }
		 return 1;
	 }

	 static int getLoadOpcode(ClassDesc c) {
		 if(c.equals(ConstantDescs.CD_void)) {
			 throw new InternalError("Unexpected void type of load opcode");
		 }
		 return ILOAD + getOpcodeOffset(c);
	 }

	 static int getReturnOpcode(ClassDesc c) {
		 if(c.equals(ConstantDescs.CD_void)) {
			 return RETURN;
		 }
		 return IRETURN + getOpcodeOffset(c);
	 }

	 private static int getOpcodeOffset(ClassDesc c) {
		 if (c.isPrimitive()) {
			 if (c.equals(ConstantDescs.CD_long)) {
				 return 1;
			 } else if (c.equals(ConstantDescs.CD_float)) {
				 return 2;
			 } else if (c.equals(ConstantDescs.CD_double)) {
				 return 3;
			 }
			 return 0;
		 } else {
			 return 4;
		 }
	 }

 }
