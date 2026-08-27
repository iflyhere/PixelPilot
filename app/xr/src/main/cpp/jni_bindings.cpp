#include <jni.h>

#include "XrGoggleSession.h"

#define JNI_METHOD(return_type, method_name) \
    extern "C" JNIEXPORT return_type JNICALL Java_com_openipc_xr_XrGoggleSession_##method_name

static inline XrGoggleSession* native(jlong handle)
{
    return reinterpret_cast<XrGoggleSession*>(handle);
}

JNI_METHOD(jlong, nativeAlloc)(JNIEnv* env, jclass clazz)
{
    (void) env;
    (void) clazz;
    return reinterpret_cast<jlong>(new XrGoggleSession());
}

JNI_METHOD(void, nativeFree)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    delete native(handle);
}

JNI_METHOD(void, nativeSetSwapchainSize)
(JNIEnv* env, jclass clazz, jlong handle, jint width, jint height)
{
    (void) env;
    (void) clazz;
    native(handle)->setSwapchainSize(width, height);
}

JNI_METHOD(void, nativeSetManifestDir)(JNIEnv* env, jclass clazz, jlong handle, jstring dir)
{
    (void) clazz;
    if (dir == nullptr) return;
    const char* chars = env->GetStringUTFChars(dir, nullptr);
    if (chars != nullptr)
    {
        native(handle)->setManifestDir(std::string(chars));
        env->ReleaseStringUTFChars(dir, chars);
    }
}

JNI_METHOD(jboolean, nativeCreate)(JNIEnv* env, jclass clazz, jlong handle, jobject activity)
{
    (void) clazz;
    return native(handle)->create(env, activity) ? JNI_TRUE : JNI_FALSE;
}

JNI_METHOD(jobject, nativeGetVideoSurface)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    return native(handle)->videoSurface();
}

JNI_METHOD(void, nativeRunLoop)(JNIEnv* env, jclass clazz, jlong handle, jobject listener)
{
    (void) clazz;
    native(handle)->runLoop(env, listener);
}

JNI_METHOD(void, nativeRequestStop)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    native(handle)->requestStop();
}

JNI_METHOD(void, nativeDestroy)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) clazz;
    native(handle)->destroy(env);
}

JNI_METHOD(jstring, nativeLastError)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) clazz;
    const std::string error = native(handle)->lastError();
    if (error.empty()) return nullptr;
    return env->NewStringUTF(error.c_str());
}

JNI_METHOD(jfloatArray, nativeGetRefreshRates)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) clazz;
    const std::vector<float> rates = native(handle)->refreshRates();
    jfloatArray              out   = env->NewFloatArray((jsize) rates.size());
    if (out == nullptr) return nullptr;
    if (!rates.empty())
    {
        env->SetFloatArrayRegion(out, 0, (jsize) rates.size(), rates.data());
    }
    return out;
}

JNI_METHOD(void, nativeSetVideoResolution)
(JNIEnv* env, jclass clazz, jlong handle, jint width, jint height)
{
    (void) env;
    (void) clazz;
    native(handle)->setVideoResolution(width, height);
}

JNI_METHOD(void, nativeSetQuadDistance)(JNIEnv* env, jclass clazz, jlong handle, jfloat meters)
{
    (void) env;
    (void) clazz;
    native(handle)->setQuadDistance(meters);
}

JNI_METHOD(void, nativeSetQuadWidth)(JNIEnv* env, jclass clazz, jlong handle, jfloat meters)
{
    (void) env;
    (void) clazz;
    native(handle)->setQuadWidth(meters);
}

JNI_METHOD(void, nativeSetPassthrough)(JNIEnv* env, jclass clazz, jlong handle, jboolean enabled)
{
    (void) env;
    (void) clazz;
    native(handle)->setPassthrough(enabled == JNI_TRUE);
}

JNI_METHOD(void, nativeSetSharpening)(JNIEnv* env, jclass clazz, jlong handle, jboolean enabled)
{
    (void) env;
    (void) clazz;
    native(handle)->setSharpening(enabled == JNI_TRUE);
}

JNI_METHOD(void, nativeSetHeadLocked)(JNIEnv* env, jclass clazz, jlong handle, jboolean enabled)
{
    (void) env;
    (void) clazz;
    native(handle)->setHeadLocked(enabled == JNI_TRUE);
}

JNI_METHOD(void, nativeRequestRefreshRate)(JNIEnv* env, jclass clazz, jlong handle, jfloat hz)
{
    (void) env;
    (void) clazz;
    native(handle)->requestRefreshRate(hz);
}

JNI_METHOD(void, nativeRequestRecenter)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    native(handle)->requestRecenter();
}

JNI_METHOD(void, nativeHaptic)
(JNIEnv* env, jclass clazz, jlong handle, jfloat amplitude, jint durationMs)
{
    (void) env;
    (void) clazz;
    native(handle)->requestHaptic(amplitude, durationMs);
}

JNI_METHOD(jfloat, nativeGetQuadDistance)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    return native(handle)->quadDistance();
}

JNI_METHOD(jfloat, nativeGetQuadWidth)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    return native(handle)->quadWidth();
}

JNI_METHOD(jboolean, nativeIsHeadLocked)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    return native(handle)->headLocked() ? JNI_TRUE : JNI_FALSE;
}

JNI_METHOD(jboolean, nativeIsPassthroughEnabled)(JNIEnv* env, jclass clazz, jlong handle)
{
    (void) env;
    (void) clazz;
    return native(handle)->passthroughEnabled() ? JNI_TRUE : JNI_FALSE;
}
