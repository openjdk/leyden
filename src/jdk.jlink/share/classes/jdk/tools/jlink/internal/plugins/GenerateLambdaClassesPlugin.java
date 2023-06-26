/*
 * Copyright (c) 2016, 2020, Oracle and/or its affiliates. All rights reserved.
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
 * Inc., 51 Franklin    St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
/* Based on GenerateJLIClassesPlugin */
package jdk.tools.jlink.internal.plugins;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.lang.constant.ClassDesc;
import java.lang.constant.ConstantDesc;
import java.lang.constant.ConstantDescs;
import java.lang.constant.DirectMethodHandleDesc;
import java.lang.constant.DirectMethodHandleDesc.Kind;
import java.lang.constant.MethodTypeDesc;
import java.lang.invoke.LambdaConversionException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.stream.Stream;

import jdk.internal.classfile.ClassModel;
import jdk.internal.classfile.*;
import jdk.internal.classfile.instruction.*;
import jdk.internal.classfile.attribute.*;
import jdk.internal.classfile.constantpool.ClassEntry;
import jdk.internal.access.JavaLangInvokeAccess;
import jdk.internal.access.SharedSecrets;
import jdk.tools.jlink.plugin.PluginException;
import jdk.tools.jlink.plugin.ResourcePool;
import jdk.tools.jlink.plugin.ResourcePoolBuilder;
import jdk.tools.jlink.plugin.ResourcePoolEntry;
import jdk.tools.jlink.plugin.ResourcePoolModule;


public final class GenerateLambdaClassesPlugin extends AbstractPlugin {

    private static final JavaLangInvokeAccess JLIA
            = SharedSecrets.getJavaLangInvokeAccess();

    private String mainArgument;
    private Stream<String> traceFileStream;

    private ClassDesc LAMBDA_METAFACTORY_CLASSDESC
        = ClassDesc.of("java.lang.invoke.LambdaMetafactory");

    public GenerateLambdaClassesPlugin() {
        super("generate-lambda-classes");
    }

    @Override
    public Set<State> getState() {
        return EnumSet.of(State.AUTO_ENABLED, State.FUNCTIONAL);
    }

    @Override
    public boolean hasArguments() {
        return false;
    }

    @Override
    public void configure(Map<String, String> config) {
        mainArgument = config.get(getName());
    }

    public void initialize(ResourcePool in) {
    }

    @Override
    public ResourcePool transform(final ResourcePool in, ResourcePoolBuilder out) {
        initialize(in);

        // Identify any lambda metafactory calls in each ResourcePoolEntry
        // and pre-generate the appropriate classes for each lambda
        // Fix up the original class to:
        //     - change the invokedynamic to appropriate lambda class call
        //     - NestMates?  Update the NestHost and NestMember
        final HashMap<String, ArrayList<ClassDesc>> nestAdditions = new HashMap<>();
        ResourcePoolHolder mid = new ResourcePoolHolder();
        in.transformAndCopy(entry -> {
                ResourcePoolEntry res = entry;
                if (entry.type().equals(ResourcePoolEntry.Type.CLASS_OR_RESOURCE)) {
                    final String path = entry.path();
                    if (path.endsWith(".class")) {
                        if (path.endsWith("module-info.class")) {
                            // No lambdas in a module-info class
                        } else {
                            // ClassHierarchyResolver is needed to ensure that the classes can be found while
                            // building the StackMapTables for the updated methods.  The conversion from
                            // invokedynamic -> invokestatic may require StackMapTable recomputation which
                            // means all referenced classes need to be accessible.  Currently using a strategy
                            // that looks at the ResourcePool's view of the modules to find a matching class
                            // and if that fails, looks at a map of ClassDesc->ClassHierarchyInfo for the
                            // pre-generated lambda implementation classes.
                            final HashMap<ClassDesc, ClassHierarchyResolver.ClassHierarchyInfo> generatedInfos = new HashMap<>();
                            ClassHierarchyResolver generatedResolver = (ClassDesc desc) -> {
                                return generatedInfos.get(desc);
                            };
                            ClassHierarchyResolver resourcePoolResolver = ClassHierarchyResolver.ofCached((ClassDesc desc) -> {
                                String d = strippedDescriptor(desc) + ".class";
                                return in.moduleView().modules()
                                    .map( m -> in.findEntry("/" + m.name() + "/" + d))
                                    .filter( o -> o.isPresent())
                                    .map( e -> e.get().content())
                                    .findFirst()
                                    .orElse(null);
                            });
                            final ArrayList<ClassDesc> lambdaNestAdditions = new ArrayList<>();
                            final ClassModel cm = Classfile.parse(entry.contentBytes(),
                                Classfile.Option.classHierarchyResolver(resourcePoolResolver.orElse(generatedResolver)));
                            // Pregenerate the Lambda expression implementation classes and convert
                            // the invokedynamic calls to an invokestatic call on the generated class.
                            // This converts from runtime generation to pregeneration but currently
                            // loses the caching of a single instance provided by
                            // InnerClassLmabdaMetaFactory::buildCallsite.
                            byte[] delambdaBytes = cm.transform((clb, cle) -> {
                                if (cle instanceof MethodModel mm) {
                                    clb.transformMethod(mm, (mb, me) -> {
                                        if (me instanceof CodeModel xm) {
                                            mb.transformCode(xm, (builder, element) -> {
                                                if (element instanceof InvokeDynamicInstruction i
                                                && isMetafactory(i)
                                                ) {
                                                    ClassModel genModel = generateLambdaInnerClass(entry, out, cm, me, i);
                                                    // register the generated class with the generatedResolver so it can be found
                                                    // for StackMapTable fixups
                                                    generatedInfos.put(
                                                        genModel.thisClass().asSymbol(),
                                                        new ClassHierarchyResolver.ClassHierarchyInfo(genModel.thisClass().asSymbol(), false /*isInterface*/, genModel.superclass().orElseThrow().asSymbol()));
                                                    // convert the indy -> invokstatic of the "thunk" helper method.  This lets the "new dup" dance
                                                    // be embedded in the helper rather than having to be inserted here.
                                                    builder.invokestatic(genModel.thisClass().asSymbol(), "thunk", i.typeSymbol());
                                                    // Track the set of classes that need to be added to this class's NestHost's list of members
                                                    lambdaNestAdditions.add(genModel.thisClass().asSymbol());
                                                } else {
                                                    builder.with(element);
                                                }
                                            });
                                        } else {
                                            mb.with(me);
                                        }
                                    });
                                } else {
                                    clb.with(cle);
                                }
                            });


                            byte[] content = delambdaBytes;

                            // Add the generated lambda classes as NestMembers to the appropriate NestHost.
                            // If the Host is the same as the ResourcePoolEntry we're processing, we can
                            // update it immediate. Otherwise, we need to save the data away to update in a
                            // later pass.
                            if (!lambdaNestAdditions.isEmpty()) {
                                Optional<NestHostAttribute> nestHost = cm.findAttribute(Attributes.NEST_HOST);
                                if (!nestHost.isPresent()) {
                                    // If there isn't a NestHost attribute, that means the current class can
                                    // be the NestHost and we can add (or update) its NestMembers attribute.
                                    content = Classfile.parse(content).transform(singleAttribT(lambdaNestAdditions));
                                } else {
                                    // Need to update the Class.nestHost to include the new members
                                    nestAdditions.computeIfAbsent(
                                        nestHost.get().nestHost().asInternalName(),
                                        k -> new ArrayList<ClassDesc>()
                                    ).addAll(lambdaNestAdditions);
                                }
                            }

                            res = entry.copyWithContent(content);
                        }
                    }
                }
                return res;
            }, mid);

        if (nestAdditions.isEmpty()) {
            // Copy from the holding pool to Out
            mid.drain(e -> out.add(e));
        } else {
            mid.drain(e -> {
                ResourcePoolEntry entry = e;
                if (entry.type().equals(ResourcePoolEntry.Type.CLASS_OR_RESOURCE)) {
                    String path = entry.path();
                    if (path.endsWith(".class")) {
                        if (path.endsWith("module-info.class")) {
                            // No lambdas in a module-info class
                        } else {
                            final ClassModel cm = Classfile.parse(entry.contentBytes());
                            ArrayList<ClassDesc> newNestMembers = nestAdditions.remove(cm.thisClass().asInternalName());
                            if (newNestMembers != null && !newNestMembers.isEmpty()) {
                                if (cm.findAttribute(Attributes.NEST_HOST).isPresent()) {
                                    throw throwConflictingNestAttributesState(cm, newNestMembers);
                                }
                                byte[] updated = cm.transform(singleAttribT(newNestMembers));
                                entry = entry.copyWithContent(updated);
                            }
                        }
                    }
                }
                out.add(entry);
            });
        }

        return out.build();
    }

    IllegalStateException throwConflictingNestAttributesState(ClassModel cm, ArrayList<ClassDesc> newMembers) {
        Optional<NestHostAttribute> nestHost = cm.findAttribute(Attributes.NEST_HOST);
        Optional<NestMembersAttribute> nestMembers = cm.findAttribute(Attributes.NEST_MEMBERS);

        StringBuffer sb = new StringBuffer();
        sb.append("-- Host --\n");
        nestHost.ifPresent( n -> sb.append(n.nestHost().asInternalName()));
        sb.append("\n-- Existing Members --\n");
        nestMembers.ifPresent( n -> sb.append(n.nestMembers()));
        sb.append("\n-- New Members " + newMembers.size() + "--\n");
        sb.append(newMembers);
        sb.append("\n-- End --\n");
        throw new IllegalStateException("Nest Attribute Conflict: " + cm.thisClass().asInternalName() +"\n" + sb.toString());
    }


    ClassTransform singleAttribT(final ArrayList<ClassDesc> nestMembers) {
        return ClassTransform.ofStateful( () -> {
            return new SingleAttributeTransform<NestMembersAttribute>(
                NestMembersAttribute.class,
                nma -> { return NestMembersAttribute.of(ClassEntry.addingSymbols(((NestMembersAttribute)nma).nestMembers(), nestMembers));},
                () -> { return NestMembersAttribute.ofSymbols(nestMembers);});
            }
        );
    }

    final class SingleAttributeTransform<T extends Attribute<T>> implements ClassTransform {
        Class<T> type;
        ClassElement attrib;
        Function<ClassElement, ClassElement> ifPresent;
        Supplier<ClassElement> ifAbsent;

        public SingleAttributeTransform(Class<T> type, Function<ClassElement,ClassElement> ifPresent, Supplier<ClassElement> ifAbsent) {
            this.type = type;
            try {
                type.asSubclass(ClassElement.class);
            } catch(ClassCastException e) {
                throw new IllegalArgumentException("The expected Attribute (" + type + ") must be a ClassElement", e);
            }
            this.ifPresent = ifPresent;
            this.ifAbsent = ifAbsent;
        }

        public void accept(ClassBuilder b, ClassElement e) {
            if (type.isInstance(e)) { attrib = e; }
            else { b.with(e); }
        }

        public void atEnd(ClassBuilder b) {
            if (attrib != null) {
                b.with(ifPresent.apply(attrib));
            } else {
                b.with(ifAbsent.get());
            }
        }

    }

    static void print(CodeModel codeModel, InvokeDynamicInstruction indy) {
        MethodModel meth = codeModel.parent().orElse(null);
        ClassModel cm = meth.parent().orElse(null);

        System.out.println("{  Class: " + cm.thisClass().asInternalName());
        System.out.println("     Method: " + meth.methodName().stringValue() + meth.methodType().stringValue() );
        System.out.println("       Indy " + indy);
        System.out.println("          Owner:" + indy.bootstrapMethod().owner() + "  equals:" + indy.bootstrapMethod().owner().equals(ClassDesc.of("java.lang.invoke.LambdaMetafactory")));
        System.out.println("}");
    }

    boolean isMetafactory(InvokeDynamicInstruction indy) {
        //STATIC/LambdaMetafactory::metafactory(MethodHandles$Lookup,String,MethodType,MethodType,MethodHandle,MethodType)CallSite]
        DirectMethodHandleDesc bsm = indy.bootstrapMethod();
        if (bsm.methodName().equals("metafactory")
        && LAMBDA_METAFACTORY_CLASSDESC.equals(bsm.owner())
        && bsm.kind().equals(DirectMethodHandleDesc.Kind.STATIC)
        ) {
            return true;
        }
        return false;
    }

    boolean isAltMetafactory(InvokeDynamicInstruction indy) {
        //STATIC/LambdaMetafactory::altMetafactory(MethodHandles$Lookup,String,MethodType,Object...)CallSite]
        DirectMethodHandleDesc bsm = indy.bootstrapMethod();
        if (bsm.methodName().equals("altMetafactory")
        && LAMBDA_METAFACTORY_CLASSDESC.equals(bsm.owner())
        && bsm.kind().equals(DirectMethodHandleDesc.Kind.STATIC)
        ) {
            return true;
        }
        return false;
    }

    /*
     * Return the name of the nest host for this class.
     *
     * Either the value found in the NestHost attribute
     * or the name of the current class if there is no
     * NestHost attribute.
     *
     */
    static String nestHostInternalName(ClassModel cm) {
        return cm.findAttribute(Attributes.NEST_HOST)
            .map(nh -> nh.nestHost().asInternalName())
            .orElse(cm.thisClass().asInternalName());
    }


    /**
     * Generate the InnerClass for the lambda meta factory
     *
     * @param entry ResourcePoolEntry representing the original class
     * @param out ResourcePoolBuilder to ensure class is added to the jlinked image
     * @param cm ClassModel for class defining the lambda (invokedynamic instruction)
     * @param methodElement MethodElement for context around the indy instruction
     * @param indy the instruction itself to pull the symbolic data from
     * @return a ClassModel representing the generated class
     */
    ClassModel generateLambdaInnerClass(ResourcePoolEntry entry, ResourcePoolBuilder out, ClassModel cm, MethodElement methodElement, InvokeDynamicInstruction indy) {
        //                                                    lookup, String interfaceMethodName, factoryType, interfaceMethodType, impl, dynamicMethodType
        //STATIC/LambdaMetafactory::metafactory(MethodHandles$Lookup,
        // String --> interfaceMethodName,
        // MethodType --> factoryType,
        // MethodType --> interfaceMethodType,
        // MethodHandle --> implementation,
        // MethodType --> dynamicMethodType )CallSite]
        /*
         {  Class: jdk/nio/zipfs/ZipFileSystem
            Method: close()V
            Indy InvokeDynamic[OP=INVOKEDYNAMIC,
                bsm=MethodHandleDesc[STATIC/LambdaMetafactory::metafactory(MethodHandles$Lookup,String,MethodType,MethodType,MethodHandle,MethodType)CallSite]
                    0 [MethodTypeDesc[()Object],
                    1 MethodHandleDesc[STATIC/ZipFileSystem::lambda$close$10(Path)Boolean],
                    2 MethodTypeDesc[()Boolean]]]
                bsm Owner:ClassDesc[LambdaMetafactory]  equals:true
         }
        */
        List<ConstantDesc> bsmArgs = indy.bootstrapArgs();
        MethodTypeDesc indyTypeSymbol = indy.typeSymbol();


        String interfaceMethodName = indy.name().stringValue();
        MethodTypeDesc interfaceMethodType = (MethodTypeDesc) bsmArgs.get(0);
        MethodTypeDesc dynamicMethodType = (MethodTypeDesc) bsmArgs.get(2);
        ClassDesc targetClassDesc = cm.thisClass().asSymbol(); //Class calling the metafactory
        MethodTypeDesc factoryTypeDesc = indy.typeSymbol();
        ClassDesc interfaceDesc = factoryTypeDesc.returnType();
        String[] interfaceNames = new String[] { strippedDescriptor(interfaceDesc) }; // from the altMetafactory Classes[] altInterfaces array.  Must include interface name
        boolean isSerializable = false; // only set by altMetafactory
        boolean accidentallySerializable = false; // TODO: not sure we can tell at jlink time without a full dictionary of the classes (heirarchy)
        DirectMethodHandleDesc implementation = (DirectMethodHandleDesc) bsmArgs.get(1);
        int implKind = implementation.refKind();
        MethodTypeDesc[] altMethodDescs = null; // OK for metafactory, fill in for altMetafactory

        try {
            byte[] bytes = JLIA.generateLambdaInnerClasses(
                interfaceMethodName,
                interfaceMethodType,
                dynamicMethodType,
                targetClassDesc,
                interfaceNames,
                factoryTypeDesc,
                isSerializable,
                accidentallySerializable,
                implementation, implKind,
                altMethodDescs);
            ClassModel genModel = Classfile.parse(bytes);

            // set the nest host - probably better done when creating the classfile originally....
            NestHostAttribute genHost = determineNestHost(cm);
            bytes = genModel.transform(ClassTransform.endHandler(b -> b.with(genHost)));

            String entryName = "/" + entry.moduleName() + "/" + genModel.thisClass().asInternalName() + ".class";
            ResourcePoolEntry ndata = ResourcePoolEntry.create(entryName, bytes);
            out.add(ndata);
            return genModel;
        } catch(LambdaConversionException e) {
            //TODO: log and continue
        }
        return null; // TODO
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
     * Determine the NestHost for a given ClassModel.
     *
     * There are three cases:
     * 1) The class has a NestHost attribute
     * 2) The class has a NestMembers attribute and is therefore its own NestHost
     * 3) The class has no recorded Nest relationships and is therefore its own NestHost
     * @param cm The ClassModel to find the NestHost for
     * @return a NestHostAttribute for the given ClassModel
     */
    NestHostAttribute determineNestHost(ClassModel cm) {
        return cm.findAttribute(Attributes.NEST_HOST)
            .orElse(NestHostAttribute.of(cm.thisClass()));
    }

    /*
     * Temporary holding pool for Resources that may need a second transformation
     * by the current Plugin.
     */
    static class ResourcePoolHolder implements ResourcePoolBuilder {
        private HashSet<ResourcePoolEntry> entries = new HashSet<>();
        private boolean drained;

        public ResourcePoolHolder() {
            super();
        }

        @Override
        public void add(ResourcePoolEntry data) {
            validateIfAlreadyDrained();
            entries.add(data);
        }

        @Override
        public ResourcePool build() {
            throw new UnsupportedOperationException("Can't build this pool.  Must be manaully drained");
        }

        public void drain(Consumer<? super ResourcePoolEntry> action) {
            validateIfAlreadyDrained();
            drained = true;
            entries.forEach(action);
        }

        private void validateIfAlreadyDrained() {
            if (drained == true) {
                throw new IllegalStateException("Already drained()");
            }
        }
    }

}

