
#include "jni.h"
#include "jni_util.h"
#include "jvm.h"

JNIEXPORT jboolean JNICALL
Java_java_net_URLClassLoader_registerAsAOTSafeImpl(JNIEnv *env, jobject loader, jstring aot_id, jstring classpath)
{
    return JVM_RegisterURLClassLoaderAsAOTSafeLoader(env, loader, aot_id, classpath);
}
