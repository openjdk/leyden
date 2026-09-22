
#include "jni.h"
#include "jni_util.h"
#include "jvm.h"

JNIEXPORT jboolean JNICALL
Java_java_net_URLClassLoader_registerForAOTLinkingImpl(JNIEnv *env, jobject loader, jobject parent, jstring classpath)
{
    return JVM_RegisterURLClassLoaderForAOTLinking(env, loader, parent, classpath);
}
