/* JNI bridge for ai.phonlet.wake.Wake over the public phonlet_wake_* C API.
 * Built as libwakejni; it dlopens libkws_engine (linked dependency). */
#include <jni.h>
#include <phonlet_wake.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void throw_error(JNIEnv *env, const char *message) {
  jclass cls = (*env)->FindClass(env, "ai/phonlet/wake/WakeException");
  if (cls) (*env)->ThrowNew(env, cls, message && *message ? message : "wake engine failure");
}

JNIEXPORT jint JNICALL Java_ai_phonlet_wake_WakeEngine_nativeInit
    (JNIEnv *env, jclass cls, jstring model_path, jstring enrollments,
     jdoubleArray sensitivities, jlongArray handle) {
  (void)cls;
  const char *path = (*env)->GetStringUTFChars(env, model_path, NULL);
  if (!path) return PW_ERR_INVALID_ARGUMENT;
  const char *json = (*env)->GetStringUTFChars(env, enrollments, NULL);
  if (!json) { (*env)->ReleaseStringUTFChars(env, model_path, path); return PW_ERR_INVALID_ARGUMENT; }
  jsize count = (*env)->GetArrayLength(env, sensitivities);
  jdouble *values = count ? (*env)->GetDoubleArrayElements(env, sensitivities, NULL) : NULL;
  pw_wake *wake = NULL;
  pw_status status = phonlet_wake_init(path, json, values, (int32_t)count, &wake);
  if (values) (*env)->ReleaseDoubleArrayElements(env, sensitivities, values, JNI_ABORT);
  (*env)->ReleaseStringUTFChars(env, enrollments, json);
  (*env)->ReleaseStringUTFChars(env, model_path, path);
  if (status == PW_OK) {
    jlong packed = (jlong)(intptr_t)wake;
    (*env)->SetLongArrayRegion(env, handle, 0, 1, &packed);
  }
  return (jint)status;
}

JNIEXPORT jint JNICALL Java_ai_phonlet_wake_WakeEngine_nativeInitBytes
    (JNIEnv *env, jclass cls, jbyteArray model, jstring enrollments,
     jdoubleArray sensitivities, jlongArray handle) {
  (void)cls;
  if (!model) return PW_ERR_INVALID_ARGUMENT;
  jsize size = (*env)->GetArrayLength(env, model);
  jbyte *bytes = (*env)->GetByteArrayElements(env, model, NULL);
  if (!bytes) return PW_ERR_RUNTIME;
  const char *json = (*env)->GetStringUTFChars(env, enrollments, NULL);
  if (!json) { (*env)->ReleaseByteArrayElements(env, model, bytes, JNI_ABORT); return PW_ERR_INVALID_ARGUMENT; }
  jsize count = (*env)->GetArrayLength(env, sensitivities);
  jdouble *values = count ? (*env)->GetDoubleArrayElements(env, sensitivities, NULL) : NULL;
  pw_wake *wake = NULL;
  pw_status status = phonlet_wake_init_bytes((const uint8_t *)bytes, (int32_t)size,
                                             json, values, (int32_t)count, &wake);
  if (values) (*env)->ReleaseDoubleArrayElements(env, sensitivities, values, JNI_ABORT);
  (*env)->ReleaseStringUTFChars(env, enrollments, json);
  (*env)->ReleaseByteArrayElements(env, model, bytes, JNI_ABORT);
  if (status == PW_OK) {
    jlong packed = (jlong)(intptr_t)wake;
    (*env)->SetLongArrayRegion(env, handle, 0, 1, &packed);
  }
  return (jint)status;
}

JNIEXPORT jint JNICALL Java_ai_phonlet_wake_WakeEngine_nativeProcess
    (JNIEnv *env, jclass cls, jlong handle, jshortArray pcm, jint offset, jint samples, jintArray index_out) {
  (void)cls;
  pw_wake *wake = (pw_wake *)(intptr_t)handle;
  if (!wake || !pcm || !index_out || offset < 0 || samples < 0) return PW_ERR_INVALID_ARGUMENT;
  /* Copy only the requested range, through a fixed stack buffer:
   * GetShortArrayElements may copy the whole Java array on every call (ART and
   * HotSpot usually do), which costs more the larger the caller's buffer. */
  jshort chunk[2048];
  int32_t first = -1;
  for (jint done = 0; done < samples;) {
    const jint n = samples - done < 2048 ? samples - done : 2048;
    (*env)->GetShortArrayRegion(env, pcm, offset + done, n, chunk);
    if ((*env)->ExceptionCheck(env)) return PW_ERR_INVALID_ARGUMENT;
    int32_t index = -1;
    const pw_status status = phonlet_wake_process(wake, chunk, n, &index);
    if (status != PW_OK) return (jint)status;
    if (first < 0) first = index;
    done += n;
  }
  (*env)->SetIntArrayRegion(env, index_out, 0, 1, &first);
  return PW_OK;
}

JNIEXPORT jint JNICALL Java_ai_phonlet_wake_WakeEngine_nativeFinish
    (JNIEnv *env, jclass cls, jlong handle, jintArray discarded_out) {
  (void)cls;
  int32_t discarded = -1;
  pw_status status = phonlet_wake_finish((pw_wake *)(intptr_t)handle, &discarded);
  if (status == PW_OK && discarded_out)
    (*env)->SetIntArrayRegion(env, discarded_out, 0, 1, &discarded);
  return (jint)status;
}

JNIEXPORT void JNICALL Java_ai_phonlet_wake_WakeEngine_nativeDelete
    (JNIEnv *env, jclass cls, jlong handle) {
  (void)env; (void)cls;
  phonlet_wake_delete((pw_wake *)(intptr_t)handle);
}

JNIEXPORT jstring JNICALL Java_ai_phonlet_wake_WakeEngine_nativeError
    (JNIEnv *env, jclass cls) {
  (void)cls;
  const char *message = phonlet_wake_error();
  return (*env)->NewStringUTF(env, message ? message : "");
}

JNIEXPORT void JNICALL Java_ai_phonlet_wake_WakeEngine_throwLastError
    (JNIEnv *env, jclass cls, jint status) {
  (void)cls;
  char text[128];
  const char *message = phonlet_wake_error();
  snprintf(text, sizeof text, "wake error %d: %s", (int)status, message && *message ? message : "no details");
  throw_error(env, text);
}

JNIEXPORT jint JNICALL Java_ai_phonlet_wake_WakeEngine_nativeVersion
    (JNIEnv *env, jclass cls) { (void)env; (void)cls; return phonlet_wake_version(); }

JNIEXPORT jint JNICALL Java_ai_phonlet_wake_WakeEngine_nativeFrameLength
    (JNIEnv *env, jclass cls) { (void)env; (void)cls; return phonlet_wake_frame_length(); }

JNIEXPORT jint JNICALL Java_ai_phonlet_wake_WakeEngine_nativeSampleRate
    (JNIEnv *env, jclass cls) { (void)env; (void)cls; return phonlet_wake_sample_rate(); }
