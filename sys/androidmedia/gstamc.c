/*
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_ORC
#include <orc/orc.h>
#else
#define orc_memcpy memcpy
#endif

#include "gstamc.h"
#include "gstamc-constants.h"

#include "gstamcvideodec.h"
#include "gstamcvideoenc.h"
#include "gstamcaudiodec.h"

#include <gmodule.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <string.h>
#include <jni.h>

/* getExceptionSummary() and getStackTrace() taken from Android's
 *   platform/libnativehelper/JNIHelp.cpp
 * Modified to work with normal C strings and without C++.
 *
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Returns a human-readable summary of an exception object. The buffer will
 * be populated with the "binary" class name and, if present, the
 * exception message.
 */
static gchar *
getExceptionSummary (JNIEnv * env, jthrowable exception)
{
  GString *gs = g_string_new ("");
  jclass exceptionClass = NULL, classClass = NULL;
  jmethodID classGetNameMethod, getMessage;
  jstring classNameStr = NULL, messageStr = NULL;
  const char *classNameChars, *messageChars;

  /* get the name of the exception's class */
  exceptionClass = (*env)->GetObjectClass (env, exception);
  classClass = (*env)->GetObjectClass (env, exceptionClass);
  classGetNameMethod =
      (*env)->GetMethodID (env, classClass, "getName", "()Ljava/lang/String;");

  classNameStr =
      (jstring) (*env)->CallObjectMethod (env, exceptionClass,
      classGetNameMethod);

  if (classNameStr == NULL) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    g_string_append (gs, "<error getting class name>");
    goto done;
  }

  classNameChars = (*env)->GetStringUTFChars (env, classNameStr, NULL);
  if (classNameChars == NULL) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    g_string_append (gs, "<error getting class name UTF-8>");
    goto done;
  }

  g_string_append (gs, classNameChars);

  (*env)->ReleaseStringUTFChars (env, classNameStr, classNameChars);

  /* if the exception has a detail message, get that */
  getMessage =
      (*env)->GetMethodID (env, exceptionClass, "getMessage",
      "()Ljava/lang/String;");
  messageStr = (jstring) (*env)->CallObjectMethod (env, exception, getMessage);
  if (messageStr == NULL) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    goto done;
  }
  g_string_append (gs, ": ");

  messageChars = (*env)->GetStringUTFChars (env, messageStr, NULL);
  if (messageChars != NULL) {
    g_string_append (gs, messageChars);
    (*env)->ReleaseStringUTFChars (env, messageStr, messageChars);
  } else {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    g_string_append (gs, "<error getting message>");
  }

done:
  if (exceptionClass)
    (*env)->DeleteLocalRef (env, exceptionClass);
  if (classClass)
    (*env)->DeleteLocalRef (env, classClass);
  if (classNameStr)
    (*env)->DeleteLocalRef (env, classNameStr);
  if (messageStr)
    (*env)->DeleteLocalRef (env, messageStr);

  return g_string_free (gs, FALSE);
}

/*
 * Returns an exception (with stack trace) as a string.
 */
static gchar *
getStackTrace (JNIEnv * env, jthrowable exception)
{
  GString *gs = g_string_new ("");
  jclass stringWriterClass = NULL, printWriterClass = NULL;
  jclass exceptionClass = NULL;
  jmethodID stringWriterCtor, stringWriterToStringMethod;
  jmethodID printWriterCtor, printStackTraceMethod;
  jobject stringWriter = NULL, printWriter = NULL;
  jstring messageStr = NULL;
  const char *utfChars;

  stringWriterClass = (*env)->FindClass (env, "java/io/StringWriter");

  if (stringWriterClass == NULL) {
    g_string_append (gs, "<error getting java.io.StringWriter class>");
    goto done;
  }

  stringWriterCtor =
      (*env)->GetMethodID (env, stringWriterClass, "<init>", "()V");
  stringWriterToStringMethod =
      (*env)->GetMethodID (env, stringWriterClass, "toString",
      "()Ljava/lang/String;");

  printWriterClass = (*env)->FindClass (env, "java/io/PrintWriter");
  if (printWriterClass == NULL) {
    g_string_append (gs, "<error getting java.io.PrintWriter class>");
    goto done;
  }

  printWriterCtor =
      (*env)->GetMethodID (env, printWriterClass, "<init>",
      "(Ljava/io/Writer;)V");
  stringWriter = (*env)->NewObject (env, stringWriterClass, stringWriterCtor);
  if (stringWriter == NULL) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    g_string_append (gs, "<error creating new StringWriter instance>");
    goto done;
  }

  printWriter =
      (*env)->NewObject (env, printWriterClass, printWriterCtor, stringWriter);
  if (printWriter == NULL) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    g_string_append (gs, "<error creating new PrintWriter instance>");
    goto done;
  }

  exceptionClass = (*env)->GetObjectClass (env, exception);
  printStackTraceMethod =
      (*env)->GetMethodID (env, exceptionClass, "printStackTrace",
      "(Ljava/io/PrintWriter;)V");
  (*env)->CallVoidMethod (env, exception, printStackTraceMethod, printWriter);
  if ((*env)->ExceptionCheck (env)) {
    (*env)->ExceptionClear (env);
    g_string_append (gs, "<exception while printing stack trace>");
    goto done;
  }

  messageStr = (jstring) (*env)->CallObjectMethod (env, stringWriter,
      stringWriterToStringMethod);
  if (messageStr == NULL) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    g_string_append (gs, "<failed to call StringWriter.toString()>");
    goto done;
  }

  utfChars = (*env)->GetStringUTFChars (env, messageStr, NULL);
  if (utfChars == NULL) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    g_string_append (gs, "<failed to get UTF chars for message>");
    goto done;
  }

  g_string_append (gs, utfChars);

  (*env)->ReleaseStringUTFChars (env, messageStr, utfChars);

done:
  if (stringWriterClass)
    (*env)->DeleteLocalRef (env, stringWriterClass);
  if (printWriterClass)
    (*env)->DeleteLocalRef (env, printWriterClass);
  if (exceptionClass)
    (*env)->DeleteLocalRef (env, exceptionClass);
  if (stringWriter)
    (*env)->DeleteLocalRef (env, stringWriter);
  if (printWriter)
    (*env)->DeleteLocalRef (env, printWriter);
  if (messageStr)
    (*env)->DeleteLocalRef (env, messageStr);

  return g_string_free (gs, FALSE);
}

#include <pthread.h>

GST_DEBUG_CATEGORY (gst_amc_debug);
#define GST_CAT_DEFAULT gst_amc_debug

GQuark gst_amc_codec_info_quark = 0;

static GList *codec_infos = NULL;
#ifdef GST_AMC_IGNORE_UNKNOWN_COLOR_FORMATS
static gboolean ignore_unknown_color_formats = TRUE;
#else
static gboolean ignore_unknown_color_formats = FALSE;
#endif

static GModule *java_module;
static jint (*get_created_java_vms) (JavaVM ** vmBuf, jsize bufLen,
    jsize * nVMs);
static jint (*create_java_vm) (JavaVM ** p_vm, JNIEnv ** p_env, void *vm_args);
static JavaVM *java_vm;
static gboolean started_java_vm = FALSE;

static gboolean accepted_color_formats (GstAmcCodecType * type,
    gboolean is_encoder);

/* Global cached references */
static struct
{
  jclass klass;
  jmethodID constructor;
} java_string;
static struct
{
  jclass klass;
  jmethodID configure;
  jmethodID create_by_codec_name;
  jmethodID dequeue_input_buffer;
  jmethodID dequeue_output_buffer;
  jmethodID flush;
  jmethodID get_input_buffers;
  jmethodID get_output_buffers;
  jmethodID get_output_format;
  jmethodID queue_input_buffer;
  jmethodID release;
  jmethodID release_output_buffer;
  jmethodID start;
  jmethodID stop;
} media_codec;
static struct
{
  jclass klass;
  jmethodID constructor;
  jfieldID flags;
  jfieldID offset;
  jfieldID presentation_time_us;
  jfieldID size;
} media_codec_buffer_info;
static struct
{
  jclass klass;
  jmethodID create_audio_format;
  jmethodID create_video_format;
  jmethodID to_string;
  jmethodID contains_key;
  jmethodID get_float;
  jmethodID set_float;
  jmethodID get_integer;
  jmethodID set_integer;
  jmethodID get_string;
  jmethodID set_string;
  jmethodID get_byte_buffer;
  jmethodID set_byte_buffer;
} media_format;

static pthread_key_t current_jni_env;

static JNIEnv *
gst_amc_attach_current_thread (void)
{
  JNIEnv *env;
  JavaVMAttachArgs args;

  GST_DEBUG ("Attaching thread %p", g_thread_self ());
  args.version = JNI_VERSION_1_6;
  args.name = NULL;
  args.group = NULL;

  if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
    GST_ERROR ("Failed to attach current thread");
    return NULL;
  }

  return env;
}

static void
gst_amc_detach_current_thread (void *env)
{
  GST_DEBUG ("Detaching thread %p", g_thread_self ());
  (*java_vm)->DetachCurrentThread (java_vm);
}

static JNIEnv *
gst_amc_get_jni_env (void)
{
  JNIEnv *env;

  if ((env = pthread_getspecific (current_jni_env)) == NULL) {
    env = gst_amc_attach_current_thread ();
    pthread_setspecific (current_jni_env, env);
  }

  return env;
}

static gboolean
check_nativehelper (void)
{
  GModule *module;
  void **jni_invocation = NULL;
  gboolean ret = FALSE;

  module = g_module_open (NULL, G_MODULE_BIND_LOCAL);
  if (!module)
    return ret;

  /* Check if libnativehelper is loaded in the process and if
   * it has these awful wrappers for JNI_CreateJavaVM and
   * JNI_GetCreatedJavaVMs that crash the app if you don't
   * create a JniInvocation instance first. If it isn't we
   * just fail here and don't initialize anything.
   * See this code for reference:
   * https://android.googlesource.com/platform/libnativehelper/+/master/JniInvocation.cpp
   */
  if (!g_module_symbol (module, "_ZN13JniInvocation15jni_invocation_E",
          (gpointer *) & jni_invocation)) {
    ret = TRUE;
  } else {
    ret = (jni_invocation != NULL && *jni_invocation != NULL);
  }

  g_module_close (module);

  return ret;
}

static gboolean
load_java_module (const gchar * name)
{
  java_module = g_module_open (name, G_MODULE_BIND_LOCAL);
  if (!java_module)
    goto load_failed;

  if (!g_module_symbol (java_module, "JNI_CreateJavaVM",
          (gpointer *) & create_java_vm))
    goto symbol_error;

  if (!g_module_symbol (java_module, "JNI_GetCreatedJavaVMs",
          (gpointer *) & get_created_java_vms))
    goto symbol_error;

  return TRUE;

load_failed:
  {
    GST_ERROR ("Failed to load Java module '%s': %s", GST_STR_NULL (name),
        g_module_error ());
    return FALSE;
  }
symbol_error:
  {
    GST_ERROR ("Failed to locate required JNI symbols in '%s': %s",
        GST_STR_NULL (name), g_module_error ());
    g_module_close (java_module);
    java_module = NULL;
    return FALSE;
  }
}

static gboolean
initialize_java_vm (void)
{
  jsize n_vms;

  /* Returns TRUE if we can safely
   * a) get the current VMs and
   * b) start a VM if none is started yet
   *
   * FIXME: On Android >= 4.4 we won't be able to safely start a
   * VM on our own without using private C++ API!
   */
  if (!check_nativehelper ()) {
    GST_ERROR ("Can't safely check for VMs or start a VM");
    return FALSE;
  }

  if (!load_java_module (NULL)) {
    if (!load_java_module ("libdvm"))
      return FALSE;
  }

  n_vms = 0;
  if (get_created_java_vms (&java_vm, 1, &n_vms) < 0)
    goto get_created_failed;

  if (n_vms > 0) {
    GST_DEBUG ("Successfully got existing Java VM %p", java_vm);
  } else {
    JNIEnv *env;
    JavaVMInitArgs vm_args;
    JavaVMOption options[4];

    GST_DEBUG ("Found no existing Java VM, trying to start one");

    options[0].optionString = "-verbose:jni";
    options[1].optionString = "-verbose:gc";
    options[2].optionString = "-Xcheck:jni";
    options[3].optionString = "-Xdebug";

    vm_args.version = JNI_VERSION_1_4;
    vm_args.options = options;
    vm_args.nOptions = 4;
    vm_args.ignoreUnrecognized = JNI_TRUE;
    if (create_java_vm (&java_vm, &env, &vm_args) < 0)
      goto create_failed;
    GST_DEBUG ("Successfully created Java VM %p", java_vm);

    started_java_vm = TRUE;
  }

  return java_vm != NULL;

get_created_failed:
  {
    GST_ERROR ("Failed to get already created VMs");
    g_module_close (java_module);
    java_module = NULL;
    return FALSE;
  }
create_failed:
  {
    GST_ERROR ("Failed to create a Java VM");
    g_module_close (java_module);
    java_module = NULL;
    return FALSE;
  }
}

static void
gst_amc_set_error_string (JNIEnv * env, GQuark domain, gint code, GError ** err,
    const gchar * message)
{
  jthrowable exception;

  if (!err) {
    if ((*env)->ExceptionCheck (env))
      (*env)->ExceptionClear (env);
    return;
  }

  if ((*env)->ExceptionCheck (env)) {
    if ((exception = (*env)->ExceptionOccurred (env))) {
      gchar *exception_description, *exception_stacktrace;

      /* Clear exception so that we can call Java methods again */
      (*env)->ExceptionClear (env);

      exception_description = getExceptionSummary (env, exception);
      exception_stacktrace = getStackTrace (env, exception);
      g_set_error (err, domain, code, "%s: %s\n%s", message,
          exception_description, exception_stacktrace);
      g_free (exception_description);
      g_free (exception_stacktrace);

      (*env)->DeleteLocalRef (env, exception);
    } else {
      (*env)->ExceptionClear (env);
      g_set_error (err, domain, code, "%s", message);
    }
  } else {
    g_set_error (err, domain, code, "%s", message);
  }
}

G_GNUC_PRINTF (5, 6)
     static void
         gst_amc_set_error (JNIEnv * env, GQuark domain, gint code,
    GError ** err, const gchar * format, ...)
{
  gchar *message;
  va_list var_args;

  va_start (var_args, format);
  message = g_strdup_vprintf (format, var_args);
  va_end (var_args);

  gst_amc_set_error_string (env, domain, code, err, message);

  g_free (message);
}

GstAmcCodec *
gst_amc_codec_new (const gchar * name, GError ** err)
{
  JNIEnv *env;
  GstAmcCodec *codec = NULL;
  jstring name_str;
  jobject object = NULL;

  g_return_val_if_fail (name != NULL, NULL);

  env = gst_amc_get_jni_env ();

  name_str = (*env)->NewStringUTF (env, name);
  if (name_str == NULL) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create Java String");
    goto error;
  }

  codec = g_slice_new0 (GstAmcCodec);

  object =
      (*env)->CallStaticObjectMethod (env, media_codec.klass,
      media_codec.create_by_codec_name, name_str);
  if ((*env)->ExceptionCheck (env) || !object) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create codec '%s'", name);
    goto error;
  }

  codec->object = (*env)->NewGlobalRef (env, object);
  if (!codec->object) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create global codec reference");
    goto error;
  }

done:
  if (object)
    (*env)->DeleteLocalRef (env, object);
  if (name_str)
    (*env)->DeleteLocalRef (env, name_str);
  name_str = NULL;

  return codec;

error:
  if (codec)
    g_slice_free (GstAmcCodec, codec);
  codec = NULL;
  goto done;
}

void
gst_amc_codec_free (GstAmcCodec * codec)
{
  JNIEnv *env;

  g_return_if_fail (codec != NULL);

  env = gst_amc_get_jni_env ();
  (*env)->DeleteGlobalRef (env, codec->object);
  g_slice_free (GstAmcCodec, codec);
}

gboolean
gst_amc_codec_configure (GstAmcCodec * codec, GstAmcFormat * format, gint flags,
    GError ** err)
{
  JNIEnv *env;
  gboolean ret = TRUE;

  g_return_val_if_fail (codec != NULL, FALSE);
  g_return_val_if_fail (format != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  (*env)->CallVoidMethod (env, codec->object, media_codec.configure,
      format->object, NULL, NULL, flags);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS, err,
        "Failed to configure codec");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

GstAmcFormat *
gst_amc_codec_get_output_format (GstAmcCodec * codec, GError ** err)
{
  JNIEnv *env;
  GstAmcFormat *ret = NULL;
  jobject object = NULL;

  g_return_val_if_fail (codec != NULL, NULL);

  env = gst_amc_get_jni_env ();

  object =
      (*env)->CallObjectMethod (env, codec->object,
      media_codec.get_output_format);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS, err,
        "Failed to get output format");
    goto done;
  }

  ret = g_slice_new0 (GstAmcFormat);

  ret->object = (*env)->NewGlobalRef (env, object);
  if (!ret->object) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS, err,
        "Failed to create global format reference");
    g_slice_free (GstAmcFormat, ret);
    ret = NULL;
  }

  (*env)->DeleteLocalRef (env, object);

done:

  return ret;
}

gboolean
gst_amc_codec_start (GstAmcCodec * codec, GError ** err)
{
  JNIEnv *env;
  gboolean ret = TRUE;

  g_return_val_if_fail (codec != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  (*env)->CallVoidMethod (env, codec->object, media_codec.start);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to start codec");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

gboolean
gst_amc_codec_stop (GstAmcCodec * codec, GError ** err)
{
  JNIEnv *env;
  gboolean ret = TRUE;

  g_return_val_if_fail (codec != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  (*env)->CallVoidMethod (env, codec->object, media_codec.stop);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to stop codec");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

gboolean
gst_amc_codec_flush (GstAmcCodec * codec, GError ** err)
{
  JNIEnv *env;
  gboolean ret = TRUE;

  g_return_val_if_fail (codec != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  (*env)->CallVoidMethod (env, codec->object, media_codec.flush);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to flush codec");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

gboolean
gst_amc_codec_release (GstAmcCodec * codec, GError ** err)
{
  JNIEnv *env;
  gboolean ret = TRUE;

  g_return_val_if_fail (codec != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  (*env)->CallVoidMethod (env, codec->object, media_codec.release);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to release codec");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

void
gst_amc_codec_free_buffers (GstAmcBuffer * buffers, gsize n_buffers)
{
  JNIEnv *env;
  jsize i;

  g_return_if_fail (buffers != NULL);

  env = gst_amc_get_jni_env ();

  for (i = 0; i < n_buffers; i++) {
    if (buffers[i].object)
      (*env)->DeleteGlobalRef (env, buffers[i].object);
  }
  g_free (buffers);
}

GstAmcBuffer *
gst_amc_codec_get_output_buffers (GstAmcCodec * codec, gsize * n_buffers,
    GError ** err)
{
  JNIEnv *env;
  jobject output_buffers = NULL;
  jsize n_output_buffers;
  GstAmcBuffer *ret = NULL;
  jsize i;

  g_return_val_if_fail (codec != NULL, NULL);
  g_return_val_if_fail (n_buffers != NULL, NULL);

  *n_buffers = 0;
  env = gst_amc_get_jni_env ();

  output_buffers =
      (*env)->CallObjectMethod (env, codec->object,
      media_codec.get_output_buffers);
  if ((*env)->ExceptionCheck (env) || !output_buffers) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get output buffers");
    goto done;
  }

  n_output_buffers = (*env)->GetArrayLength (env, output_buffers);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get output buffers array length");
    goto done;
  }

  *n_buffers = n_output_buffers;
  ret = g_new0 (GstAmcBuffer, n_output_buffers);

  for (i = 0; i < n_output_buffers; i++) {
    jobject buffer = NULL;

    buffer = (*env)->GetObjectArrayElement (env, output_buffers, i);
    if ((*env)->ExceptionCheck (env) || !buffer) {
      gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
          "Failed to get output buffer %d", i);
      goto error;
    }

    ret[i].object = (*env)->NewGlobalRef (env, buffer);
    (*env)->DeleteLocalRef (env, buffer);
    if (!ret[i].object) {
      gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
          "Failed to create global output buffer reference %d", i);
      goto error;
    }

    ret[i].data = (*env)->GetDirectBufferAddress (env, ret[i].object);
    if (!ret[i].data) {
      gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
          "Failed to get output buffer address %d", i);
      goto error;
    }
    ret[i].size = (*env)->GetDirectBufferCapacity (env, ret[i].object);
  }

done:
  if (output_buffers)
    (*env)->DeleteLocalRef (env, output_buffers);
  output_buffers = NULL;

  return ret;
error:
  if (ret)
    gst_amc_codec_free_buffers (ret, n_output_buffers);
  ret = NULL;
  *n_buffers = 0;
  goto done;
}

GstAmcBuffer *
gst_amc_codec_get_input_buffers (GstAmcCodec * codec, gsize * n_buffers,
    GError ** err)
{
  JNIEnv *env;
  jobject input_buffers = NULL;
  jsize n_input_buffers;
  GstAmcBuffer *ret = NULL;
  jsize i;

  g_return_val_if_fail (codec != NULL, NULL);
  g_return_val_if_fail (n_buffers != NULL, NULL);

  *n_buffers = 0;
  env = gst_amc_get_jni_env ();

  input_buffers =
      (*env)->CallObjectMethod (env, codec->object,
      media_codec.get_input_buffers);
  if ((*env)->ExceptionCheck (env) || !input_buffers) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get input buffers");
    goto done;
  }

  n_input_buffers = (*env)->GetArrayLength (env, input_buffers);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get input buffers array length");
    goto done;
  }

  *n_buffers = n_input_buffers;
  ret = g_new0 (GstAmcBuffer, n_input_buffers);

  for (i = 0; i < n_input_buffers; i++) {
    jobject buffer = NULL;

    buffer = (*env)->GetObjectArrayElement (env, input_buffers, i);
    if ((*env)->ExceptionCheck (env) || !buffer) {
      gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
          "Failed to get input buffer %d", i);
      goto error;
    }

    ret[i].object = (*env)->NewGlobalRef (env, buffer);
    (*env)->DeleteLocalRef (env, buffer);
    if (!ret[i].object) {
      gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
          "Failed to create global input buffer reference %d", i);
      goto error;
    }

    ret[i].data = (*env)->GetDirectBufferAddress (env, ret[i].object);
    if (!ret[i].data) {
      gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
          "Failed to get input buffer address %d", i);
      goto error;
    }
    ret[i].size = (*env)->GetDirectBufferCapacity (env, ret[i].object);
  }

done:
  if (input_buffers)
    (*env)->DeleteLocalRef (env, input_buffers);
  input_buffers = NULL;

  return ret;
error:
  if (ret)
    gst_amc_codec_free_buffers (ret, n_input_buffers);
  ret = NULL;
  *n_buffers = 0;
  goto done;
}

gint
gst_amc_codec_dequeue_input_buffer (GstAmcCodec * codec, gint64 timeoutUs,
    GError ** err)
{
  JNIEnv *env;
  gint ret = G_MININT;

  g_return_val_if_fail (codec != NULL, G_MININT);

  env = gst_amc_get_jni_env ();

  ret =
      (*env)->CallIntMethod (env, codec->object,
      media_codec.dequeue_input_buffer, timeoutUs);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to dequeue input buffer");
    ret = G_MININT;
    goto done;
  }

done:

  return ret;
}

static gboolean
gst_amc_codec_fill_buffer_info (JNIEnv * env, jobject buffer_info,
    GstAmcBufferInfo * info, GError ** err)
{
  g_return_val_if_fail (buffer_info != NULL, FALSE);

  info->flags =
      (*env)->GetIntField (env, buffer_info, media_codec_buffer_info.flags);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get buffer info flags");
    return FALSE;
  }

  info->offset =
      (*env)->GetIntField (env, buffer_info, media_codec_buffer_info.offset);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get buffer info offset");
    return FALSE;
  }

  info->presentation_time_us =
      (*env)->GetLongField (env, buffer_info,
      media_codec_buffer_info.presentation_time_us);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get buffer info pts");
    return FALSE;
  }

  info->size =
      (*env)->GetIntField (env, buffer_info, media_codec_buffer_info.size);
  if ((*env)->ExceptionCheck (env)) {
    (*env)->ExceptionClear (env);
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get buffer info size");
    return FALSE;
  }

  return TRUE;
}

gint
gst_amc_codec_dequeue_output_buffer (GstAmcCodec * codec,
    GstAmcBufferInfo * info, gint64 timeoutUs, GError ** err)
{
  JNIEnv *env;
  gint ret = G_MININT;
  jobject info_o = NULL;

  g_return_val_if_fail (codec != NULL, G_MININT);

  env = gst_amc_get_jni_env ();

  info_o =
      (*env)->NewObject (env, media_codec_buffer_info.klass,
      media_codec_buffer_info.constructor);
  if (!info_o) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create buffer info instance");
    goto done;
  }

  ret =
      (*env)->CallIntMethod (env, codec->object,
      media_codec.dequeue_output_buffer, info_o, timeoutUs);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to dequeue output buffer");
    ret = G_MININT;
    goto done;
  }

  if (!gst_amc_codec_fill_buffer_info (env, info_o, info, err)) {
    ret = G_MININT;
    goto done;
  }

done:
  if (info_o)
    (*env)->DeleteLocalRef (env, info_o);
  info_o = NULL;

  return ret;
}

gboolean
gst_amc_codec_queue_input_buffer (GstAmcCodec * codec, gint index,
    const GstAmcBufferInfo * info, GError ** err)
{
  JNIEnv *env;
  gboolean ret = TRUE;

  g_return_val_if_fail (codec != NULL, FALSE);
  g_return_val_if_fail (info != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  (*env)->CallVoidMethod (env, codec->object, media_codec.queue_input_buffer,
      index, info->offset, info->size, info->presentation_time_us, info->flags);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to queue input buffer");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

gboolean
gst_amc_codec_release_output_buffer (GstAmcCodec * codec, gint index,
    GError ** err)
{
  JNIEnv *env;
  gboolean ret = TRUE;

  g_return_val_if_fail (codec != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  (*env)->CallVoidMethod (env, codec->object, media_codec.release_output_buffer,
      index, JNI_FALSE);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to release output buffer");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

GstAmcFormat *
gst_amc_format_new_audio (const gchar * mime, gint sample_rate, gint channels,
    GError ** err)
{
  JNIEnv *env;
  GstAmcFormat *format = NULL;
  jstring mime_str;
  jobject object = NULL;

  g_return_val_if_fail (mime != NULL, NULL);

  env = gst_amc_get_jni_env ();

  mime_str = (*env)->NewStringUTF (env, mime);
  if (mime_str == NULL) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create Java string");
    goto error;
  }

  format = g_slice_new0 (GstAmcFormat);

  object =
      (*env)->CallStaticObjectMethod (env, media_format.klass,
      media_format.create_audio_format, mime_str, sample_rate, channels);
  if ((*env)->ExceptionCheck (env) || !object) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create format instance '%s'", mime);
    goto error;
  }

  format->object = (*env)->NewGlobalRef (env, object);
  if (!format->object) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create global format reference");
    goto error;
  }

done:
  if (object)
    (*env)->DeleteLocalRef (env, object);
  if (mime_str)
    (*env)->DeleteLocalRef (env, mime_str);
  mime_str = NULL;

  return format;

error:
  if (format)
    g_slice_free (GstAmcFormat, format);
  format = NULL;
  goto done;
}

GstAmcFormat *
gst_amc_format_new_video (const gchar * mime, gint width, gint height,
    GError ** err)
{
  JNIEnv *env;
  GstAmcFormat *format = NULL;
  jstring mime_str;
  jobject object = NULL;

  g_return_val_if_fail (mime != NULL, NULL);

  env = gst_amc_get_jni_env ();

  mime_str = (*env)->NewStringUTF (env, mime);
  if (mime_str == NULL) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create Java string");
    goto error;
  }

  format = g_slice_new0 (GstAmcFormat);

  object =
      (*env)->CallStaticObjectMethod (env, media_format.klass,
      media_format.create_video_format, mime_str, width, height);
  if ((*env)->ExceptionCheck (env) || !object) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create format instance '%s'", mime);
    goto error;
  }

  format->object = (*env)->NewGlobalRef (env, object);
  if (!format->object) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, err,
        "Failed to create global format reference");
    goto error;
  }

done:
  if (object)
    (*env)->DeleteLocalRef (env, object);
  if (mime_str)
    (*env)->DeleteLocalRef (env, mime_str);
  mime_str = NULL;

  return format;

error:
  if (format)
    g_slice_free (GstAmcFormat, format);
  format = NULL;
  goto done;
}

void
gst_amc_format_free (GstAmcFormat * format)
{
  JNIEnv *env;

  g_return_if_fail (format != NULL);

  env = gst_amc_get_jni_env ();
  (*env)->DeleteGlobalRef (env, format->object);
  g_slice_free (GstAmcFormat, format);
}

gchar *
gst_amc_format_to_string (GstAmcFormat * format, GError ** err)
{
  JNIEnv *env;
  jstring v_str = NULL;
  const gchar *v = NULL;
  gchar *ret = NULL;

  g_return_val_if_fail (format != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  v_str =
      (*env)->CallObjectMethod (env, format->object, media_format.to_string);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to convert format to string");
    goto done;
  }

  v = (*env)->GetStringUTFChars (env, v_str, NULL);
  if (!v) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to get UTF8 string");
    goto done;
  }

  ret = g_strdup (v);

done:
  if (v)
    (*env)->ReleaseStringUTFChars (env, v_str, v);
  if (v_str)
    (*env)->DeleteLocalRef (env, v_str);

  return ret;
}

gboolean
gst_amc_format_contains_key (GstAmcFormat * format, const gchar * key,
    GError ** err)
{
  JNIEnv *env;
  gboolean ret = FALSE;
  jstring key_str = NULL;

  g_return_val_if_fail (format != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  ret =
      (*env)->CallBooleanMethod (env, format->object, media_format.contains_key,
      key_str);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to check if format contains key '%s'", key);
    goto done;
  }

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);

  return ret;
}

gboolean
gst_amc_format_get_float (GstAmcFormat * format, const gchar * key,
    gfloat * value, GError ** err)
{
  JNIEnv *env;
  gboolean ret = FALSE;
  jstring key_str = NULL;

  g_return_val_if_fail (format != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  *value = 0;
  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  *value =
      (*env)->CallFloatMethod (env, format->object, media_format.get_float,
      key_str);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed get float key '%s'", key);
    goto done;
  }
  ret = TRUE;

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);

  return ret;
}

void
gst_amc_format_set_float (GstAmcFormat * format, const gchar * key,
    gfloat value, GError ** err)
{
  JNIEnv *env;
  jstring key_str = NULL;

  g_return_if_fail (format != NULL);
  g_return_if_fail (key != NULL);

  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  (*env)->CallVoidMethod (env, format->object, media_format.set_float, key_str,
      value);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed set float key '%s'", key);
    goto done;
  }

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);
}

gboolean
gst_amc_format_get_int (GstAmcFormat * format, const gchar * key, gint * value,
    GError ** err)
{
  JNIEnv *env;
  gboolean ret = FALSE;
  jstring key_str = NULL;

  g_return_val_if_fail (format != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  *value = 0;
  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  *value =
      (*env)->CallIntMethod (env, format->object, media_format.get_integer,
      key_str);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed get integer key '%s'", key);
    goto done;
  }
  ret = TRUE;

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);

  return ret;

}

void
gst_amc_format_set_int (GstAmcFormat * format, const gchar * key, gint value,
    GError ** err)
{
  JNIEnv *env;
  jstring key_str = NULL;

  g_return_if_fail (format != NULL);
  g_return_if_fail (key != NULL);

  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  (*env)->CallVoidMethod (env, format->object, media_format.set_integer,
      key_str, value);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed set integer key '%s'", key);
    goto done;
  }

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);
}

gboolean
gst_amc_format_get_string (GstAmcFormat * format, const gchar * key,
    gchar ** value, GError ** err)
{
  JNIEnv *env;
  gboolean ret = FALSE;
  jstring key_str = NULL;
  jstring v_str = NULL;
  const gchar *v = NULL;

  g_return_val_if_fail (format != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  *value = 0;
  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  v_str =
      (*env)->CallObjectMethod (env, format->object, media_format.get_string,
      key_str);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed get string key '%s'", key);
    goto done;
  }

  v = (*env)->GetStringUTFChars (env, v_str, NULL);
  if (!v) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed get string UTF8 characters");
    goto done;
  }

  *value = g_strdup (v);

  ret = TRUE;

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);
  if (v)
    (*env)->ReleaseStringUTFChars (env, v_str, v);
  if (v_str)
    (*env)->DeleteLocalRef (env, v_str);

  return ret;
}

void
gst_amc_format_set_string (GstAmcFormat * format, const gchar * key,
    const gchar * value, GError ** err)
{
  JNIEnv *env;
  jstring key_str = NULL;
  jstring v_str = NULL;

  g_return_if_fail (format != NULL);
  g_return_if_fail (key != NULL);
  g_return_if_fail (value != NULL);

  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  v_str = (*env)->NewStringUTF (env, value);
  if (!v_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  (*env)->CallVoidMethod (env, format->object, media_format.set_string, key_str,
      v_str);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed set string key '%s'", key);
    goto done;
  }

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);
  if (v_str)
    (*env)->DeleteLocalRef (env, v_str);
}

gboolean
gst_amc_format_get_buffer (GstAmcFormat * format, const gchar * key,
    guint8 ** data, gsize * size, GError ** err)
{
  JNIEnv *env;
  gboolean ret = FALSE;
  jstring key_str = NULL;
  jobject v = NULL;

  g_return_val_if_fail (format != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (size != NULL, FALSE);

  *data = NULL;
  *size = 0;
  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  v = (*env)->CallObjectMethod (env, format->object,
      media_format.get_byte_buffer, key_str);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed get buffer key '%s'", key);
    goto done;
  }

  *data = (*env)->GetDirectBufferAddress (env, v);
  if (!data) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed get buffer address");
    goto done;
  }
  *size = (*env)->GetDirectBufferCapacity (env, v);
  *data = g_memdup (*data, *size);

  ret = TRUE;

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);
  if (v)
    (*env)->DeleteLocalRef (env, v);

  return ret;
}

void
gst_amc_format_set_buffer (GstAmcFormat * format, const gchar * key,
    guint8 * data, gsize size, GError ** err)
{
  JNIEnv *env;
  jstring key_str = NULL;
  jobject v = NULL;

  g_return_if_fail (format != NULL);
  g_return_if_fail (key != NULL);
  g_return_if_fail (data != NULL);

  env = gst_amc_get_jni_env ();

  key_str = (*env)->NewStringUTF (env, key);
  if (!key_str) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed to create Java string");
    goto done;
  }

  /* FIXME: The memory must remain valid until the codec is stopped */
  v = (*env)->NewDirectByteBuffer (env, data, size);
  if (!v) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed create Java byte buffer");
    goto done;
  }

  (*env)->CallVoidMethod (env, format->object, media_format.set_byte_buffer,
      key_str, v);
  if ((*env)->ExceptionCheck (env)) {
    gst_amc_set_error (env, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED, err,
        "Failed set buffer key '%s'", key);
    goto done;
  }

done:
  if (key_str)
    (*env)->DeleteLocalRef (env, key_str);
  if (v)
    (*env)->DeleteLocalRef (env, v);
}

static gboolean
get_java_classes (void)
{
  gboolean ret = TRUE;
  JNIEnv *env;
  jclass tmp;

  GST_DEBUG ("Retrieving Java classes");

  env = gst_amc_get_jni_env ();

  tmp = (*env)->FindClass (env, "java/lang/String");
  if (!tmp) {
    ret = FALSE;
    GST_ERROR ("Failed to get string class");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }
  java_string.klass = (*env)->NewGlobalRef (env, tmp);
  if (!java_string.klass) {
    ret = FALSE;
    GST_ERROR ("Failed to get string class global reference");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }
  (*env)->DeleteLocalRef (env, tmp);
  tmp = NULL;

  java_string.constructor =
      (*env)->GetMethodID (env, java_string.klass, "<init>", "([C)V");
  if (!java_string.constructor) {
    ret = FALSE;
    GST_ERROR ("Failed to get string methods");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }

  tmp = (*env)->FindClass (env, "android/media/MediaCodec");
  if (!tmp) {
    ret = FALSE;
    GST_ERROR ("Failed to get codec class");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }
  media_codec.klass = (*env)->NewGlobalRef (env, tmp);
  if (!media_codec.klass) {
    ret = FALSE;
    GST_ERROR ("Failed to get codec class global reference");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }
  (*env)->DeleteLocalRef (env, tmp);
  tmp = NULL;

  media_codec.create_by_codec_name =
      (*env)->GetStaticMethodID (env, media_codec.klass, "createByCodecName",
      "(Ljava/lang/String;)Landroid/media/MediaCodec;");
  media_codec.configure =
      (*env)->GetMethodID (env, media_codec.klass, "configure",
      "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V");
  media_codec.dequeue_input_buffer =
      (*env)->GetMethodID (env, media_codec.klass, "dequeueInputBuffer",
      "(J)I");
  media_codec.dequeue_output_buffer =
      (*env)->GetMethodID (env, media_codec.klass, "dequeueOutputBuffer",
      "(Landroid/media/MediaCodec$BufferInfo;J)I");
  media_codec.flush =
      (*env)->GetMethodID (env, media_codec.klass, "flush", "()V");
  media_codec.get_input_buffers =
      (*env)->GetMethodID (env, media_codec.klass, "getInputBuffers",
      "()[Ljava/nio/ByteBuffer;");
  media_codec.get_output_buffers =
      (*env)->GetMethodID (env, media_codec.klass, "getOutputBuffers",
      "()[Ljava/nio/ByteBuffer;");
  media_codec.get_output_format =
      (*env)->GetMethodID (env, media_codec.klass, "getOutputFormat",
      "()Landroid/media/MediaFormat;");
  media_codec.queue_input_buffer =
      (*env)->GetMethodID (env, media_codec.klass, "queueInputBuffer",
      "(IIIJI)V");
  media_codec.release =
      (*env)->GetMethodID (env, media_codec.klass, "release", "()V");
  media_codec.release_output_buffer =
      (*env)->GetMethodID (env, media_codec.klass, "releaseOutputBuffer",
      "(IZ)V");
  media_codec.start =
      (*env)->GetMethodID (env, media_codec.klass, "start", "()V");
  media_codec.stop =
      (*env)->GetMethodID (env, media_codec.klass, "stop", "()V");

  if (!media_codec.configure ||
      !media_codec.create_by_codec_name ||
      !media_codec.dequeue_input_buffer ||
      !media_codec.dequeue_output_buffer ||
      !media_codec.flush ||
      !media_codec.get_input_buffers ||
      !media_codec.get_output_buffers ||
      !media_codec.get_output_format ||
      !media_codec.queue_input_buffer ||
      !media_codec.release ||
      !media_codec.release_output_buffer ||
      !media_codec.start || !media_codec.stop) {
    ret = FALSE;
    GST_ERROR ("Failed to get codec methods");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }

  tmp = (*env)->FindClass (env, "android/media/MediaCodec$BufferInfo");
  if (!tmp) {
    ret = FALSE;
    (*env)->ExceptionClear (env);
    GST_ERROR ("Failed to get codec buffer info class");
    goto done;
  }
  media_codec_buffer_info.klass = (*env)->NewGlobalRef (env, tmp);
  if (!media_codec_buffer_info.klass) {
    ret = FALSE;
    GST_ERROR ("Failed to get codec buffer info class global reference");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }
  (*env)->DeleteLocalRef (env, tmp);
  tmp = NULL;

  media_codec_buffer_info.constructor =
      (*env)->GetMethodID (env, media_codec_buffer_info.klass, "<init>", "()V");
  media_codec_buffer_info.flags =
      (*env)->GetFieldID (env, media_codec_buffer_info.klass, "flags", "I");
  media_codec_buffer_info.offset =
      (*env)->GetFieldID (env, media_codec_buffer_info.klass, "offset", "I");
  media_codec_buffer_info.presentation_time_us =
      (*env)->GetFieldID (env, media_codec_buffer_info.klass,
      "presentationTimeUs", "J");
  media_codec_buffer_info.size =
      (*env)->GetFieldID (env, media_codec_buffer_info.klass, "size", "I");
  if (!media_codec_buffer_info.constructor || !media_codec_buffer_info.flags
      || !media_codec_buffer_info.offset
      || !media_codec_buffer_info.presentation_time_us
      || !media_codec_buffer_info.size) {
    ret = FALSE;
    GST_ERROR ("Failed to get buffer info methods and fields");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }

  tmp = (*env)->FindClass (env, "android/media/MediaFormat");
  if (!tmp) {
    ret = FALSE;
    GST_ERROR ("Failed to get format class");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }
  media_format.klass = (*env)->NewGlobalRef (env, tmp);
  if (!media_format.klass) {
    ret = FALSE;
    GST_ERROR ("Failed to get format class global reference");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }
  (*env)->DeleteLocalRef (env, tmp);
  tmp = NULL;

  media_format.create_audio_format =
      (*env)->GetStaticMethodID (env, media_format.klass, "createAudioFormat",
      "(Ljava/lang/String;II)Landroid/media/MediaFormat;");
  media_format.create_video_format =
      (*env)->GetStaticMethodID (env, media_format.klass, "createVideoFormat",
      "(Ljava/lang/String;II)Landroid/media/MediaFormat;");
  media_format.to_string =
      (*env)->GetMethodID (env, media_format.klass, "toString",
      "()Ljava/lang/String;");
  media_format.contains_key =
      (*env)->GetMethodID (env, media_format.klass, "containsKey",
      "(Ljava/lang/String;)Z");
  media_format.get_float =
      (*env)->GetMethodID (env, media_format.klass, "getFloat",
      "(Ljava/lang/String;)F");
  media_format.set_float =
      (*env)->GetMethodID (env, media_format.klass, "setFloat",
      "(Ljava/lang/String;F)V");
  media_format.get_integer =
      (*env)->GetMethodID (env, media_format.klass, "getInteger",
      "(Ljava/lang/String;)I");
  media_format.set_integer =
      (*env)->GetMethodID (env, media_format.klass, "setInteger",
      "(Ljava/lang/String;I)V");
  media_format.get_string =
      (*env)->GetMethodID (env, media_format.klass, "getString",
      "(Ljava/lang/String;)Ljava/lang/String;");
  media_format.set_string =
      (*env)->GetMethodID (env, media_format.klass, "setString",
      "(Ljava/lang/String;Ljava/lang/String;)V");
  media_format.get_byte_buffer =
      (*env)->GetMethodID (env, media_format.klass, "getByteBuffer",
      "(Ljava/lang/String;)Ljava/nio/ByteBuffer;");
  media_format.set_byte_buffer =
      (*env)->GetMethodID (env, media_format.klass, "setByteBuffer",
      "(Ljava/lang/String;Ljava/nio/ByteBuffer;)V");
  if (!media_format.create_audio_format || !media_format.create_video_format
      || !media_format.contains_key || !media_format.get_float
      || !media_format.set_float || !media_format.get_integer
      || !media_format.set_integer || !media_format.get_string
      || !media_format.set_string || !media_format.get_byte_buffer
      || !media_format.set_byte_buffer) {
    ret = FALSE;
    GST_ERROR ("Failed to get format methods");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }

done:
  if (tmp)
    (*env)->DeleteLocalRef (env, tmp);
  tmp = NULL;

  return ret;
}

static gboolean
scan_codecs (GstPlugin * plugin)
{
  gboolean ret = TRUE;
  JNIEnv *env;
  jclass codec_list_class = NULL;
  jmethodID get_codec_count_id, get_codec_info_at_id;
  jint codec_count, i;
  const GstStructure *cache_data;

  GST_DEBUG ("Scanning codecs");

  if ((cache_data = gst_plugin_get_cache_data (plugin))) {
    const GValue *arr = gst_structure_get_value (cache_data, "codecs");
    guint i, n;

    GST_DEBUG ("Getting codecs from cache");
    n = gst_value_array_get_size (arr);
    for (i = 0; i < n; i++) {
      const GValue *cv = gst_value_array_get_value (arr, i);
      const GstStructure *cs = gst_value_get_structure (cv);
      const gchar *name;
      gboolean is_encoder;
      const GValue *starr;
      guint j, n2;
      GstAmcCodecInfo *gst_codec_info;

      gst_codec_info = g_new0 (GstAmcCodecInfo, 1);

      name = gst_structure_get_string (cs, "name");
      gst_structure_get_boolean (cs, "is-encoder", &is_encoder);
      gst_codec_info->name = g_strdup (name);
      gst_codec_info->is_encoder = is_encoder;

      starr = gst_structure_get_value (cs, "supported-types");
      n2 = gst_value_array_get_size (starr);

      gst_codec_info->n_supported_types = n2;
      gst_codec_info->supported_types = g_new0 (GstAmcCodecType, n2);

      for (j = 0; j < n2; j++) {
        const GValue *stv = gst_value_array_get_value (starr, j);
        const GstStructure *sts = gst_value_get_structure (stv);
        const gchar *mime;
        const GValue *cfarr;
        const GValue *plarr;
        guint k, n3;
        GstAmcCodecType *gst_codec_type = &gst_codec_info->supported_types[j];

        mime = gst_structure_get_string (sts, "mime");
        gst_codec_type->mime = g_strdup (mime);

        cfarr = gst_structure_get_value (sts, "color-formats");
        n3 = gst_value_array_get_size (cfarr);

        gst_codec_type->n_color_formats = n3;
        gst_codec_type->color_formats = g_new0 (gint, n3);

        for (k = 0; k < n3; k++) {
          const GValue *cfv = gst_value_array_get_value (cfarr, k);
          gint cf = g_value_get_int (cfv);

          gst_codec_type->color_formats[k] = cf;
        }

        plarr = gst_structure_get_value (sts, "profile-levels");
        n3 = gst_value_array_get_size (plarr);

        gst_codec_type->n_profile_levels = n3;
        gst_codec_type->profile_levels =
            g_malloc0 (sizeof (gst_codec_type->profile_levels[0]) * n3);

        for (k = 0; k < n3; k++) {
          const GValue *plv = gst_value_array_get_value (plarr, k);
          const GValue *p, *l;

          p = gst_value_array_get_value (plv, 0);
          l = gst_value_array_get_value (plv, 1);
          gst_codec_type->profile_levels[k].profile = g_value_get_int (p);
          gst_codec_type->profile_levels[k].level = g_value_get_int (l);
        }
      }

      codec_infos = g_list_append (codec_infos, gst_codec_info);
    }

    return TRUE;
  }

  env = gst_amc_get_jni_env ();

  codec_list_class = (*env)->FindClass (env, "android/media/MediaCodecList");
  if (!codec_list_class) {
    ret = FALSE;
    GST_ERROR ("Failed to get codec list class");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }

  get_codec_count_id =
      (*env)->GetStaticMethodID (env, codec_list_class, "getCodecCount", "()I");
  get_codec_info_at_id =
      (*env)->GetStaticMethodID (env, codec_list_class, "getCodecInfoAt",
      "(I)Landroid/media/MediaCodecInfo;");
  if (!get_codec_count_id || !get_codec_info_at_id) {
    ret = FALSE;
    GST_ERROR ("Failed to get codec list method IDs");
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
    }
    goto done;
  }

  codec_count =
      (*env)->CallStaticIntMethod (env, codec_list_class, get_codec_count_id);
  if ((*env)->ExceptionCheck (env)) {
    ret = FALSE;
    GST_ERROR ("Failed to get number of available codecs");
    (*env)->ExceptionDescribe (env);
    (*env)->ExceptionClear (env);
    goto done;
  }

  GST_LOG ("Found %d available codecs", codec_count);

  for (i = 0; i < codec_count; i++) {
    GstAmcCodecInfo *gst_codec_info;
    jobject codec_info = NULL;
    jclass codec_info_class = NULL;
    jmethodID get_capabilities_for_type_id, get_name_id;
    jmethodID get_supported_types_id, is_encoder_id;
    jobject name = NULL;
    const gchar *name_str = NULL;
    jboolean is_encoder;
    jarray supported_types = NULL;
    jsize n_supported_types;
    jsize j;
    gboolean valid_codec = TRUE;

    gst_codec_info = g_new0 (GstAmcCodecInfo, 1);

    codec_info =
        (*env)->CallStaticObjectMethod (env, codec_list_class,
        get_codec_info_at_id, i);
    if ((*env)->ExceptionCheck (env) || !codec_info) {
      GST_ERROR ("Failed to get codec info %d", i);
      if ((*env)->ExceptionCheck (env)) {
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
      }
      valid_codec = FALSE;
      goto next_codec;
    }

    codec_info_class = (*env)->GetObjectClass (env, codec_info);
    if (!codec_list_class) {
      GST_ERROR ("Failed to get codec info class");
      if ((*env)->ExceptionCheck (env)) {
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
      }
      valid_codec = FALSE;
      goto next_codec;
    }

    get_capabilities_for_type_id =
        (*env)->GetMethodID (env, codec_info_class, "getCapabilitiesForType",
        "(Ljava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;");
    get_name_id =
        (*env)->GetMethodID (env, codec_info_class, "getName",
        "()Ljava/lang/String;");
    get_supported_types_id =
        (*env)->GetMethodID (env, codec_info_class, "getSupportedTypes",
        "()[Ljava/lang/String;");
    is_encoder_id =
        (*env)->GetMethodID (env, codec_info_class, "isEncoder", "()Z");
    if (!get_capabilities_for_type_id || !get_name_id
        || !get_supported_types_id || !is_encoder_id) {
      GST_ERROR ("Failed to get codec info method IDs");
      if ((*env)->ExceptionCheck (env)) {
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
      }
      valid_codec = FALSE;
      goto next_codec;
    }

    name = (*env)->CallObjectMethod (env, codec_info, get_name_id);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to get codec name");
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
      valid_codec = FALSE;
      goto next_codec;
    }
    name_str = (*env)->GetStringUTFChars (env, name, NULL);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to convert codec name to UTF8");
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
      valid_codec = FALSE;
      goto next_codec;
    }

    GST_INFO ("Checking codec '%s'", name_str);

    /* Compatibility codec names */
    if (strcmp (name_str, "AACEncoder") == 0 ||
        strcmp (name_str, "OMX.google.raw.decoder") == 0) {
      GST_INFO ("Skipping compatibility codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }

    if (g_str_has_suffix (name_str, ".secure")) {
      GST_INFO ("Skipping DRM codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }

    /* FIXME: Non-Google codecs usually just don't work and hang forever
     * or crash when not used from a process that started the Java
     * VM via the non-public AndroidRuntime class. Can we somehow
     * initialize all this?
     */
    if (started_java_vm && !g_str_has_prefix (name_str, "OMX.google.")) {
      GST_INFO ("Skipping non-Google codec '%s' in standalone mode", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }

    if (g_str_has_prefix (name_str, "OMX.ARICENT.")) {
      GST_INFO ("Skipping possible broken codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }

    /* FIXME:
     *   - Vorbis: Generates clicks for multi-channel streams
     *   - *Law: Generates output with too low frequencies
     */
    if (strcmp (name_str, "OMX.google.vorbis.decoder") == 0 ||
        strcmp (name_str, "OMX.google.g711.alaw.decoder") == 0 ||
        strcmp (name_str, "OMX.google.g711.mlaw.decoder") == 0) {
      GST_INFO ("Skipping known broken codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }
    gst_codec_info->name = g_strdup (name_str);

    is_encoder = (*env)->CallBooleanMethod (env, codec_info, is_encoder_id);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to detect if codec is an encoder");
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
      valid_codec = FALSE;
      goto next_codec;
    }
    gst_codec_info->is_encoder = is_encoder;

    supported_types =
        (*env)->CallObjectMethod (env, codec_info, get_supported_types_id);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to get supported types");
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
      valid_codec = FALSE;
      goto next_codec;
    }

    n_supported_types = (*env)->GetArrayLength (env, supported_types);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to get supported types array length");
      (*env)->ExceptionDescribe (env);
      (*env)->ExceptionClear (env);
      valid_codec = FALSE;
      goto next_codec;
    }

    GST_INFO ("Codec '%s' has %d supported types", name_str, n_supported_types);

    gst_codec_info->supported_types =
        g_new0 (GstAmcCodecType, n_supported_types);
    gst_codec_info->n_supported_types = n_supported_types;

    if (n_supported_types == 0) {
      valid_codec = FALSE;
      GST_ERROR ("Codec has no supported types");
      goto next_codec;
    }

    for (j = 0; j < n_supported_types; j++) {
      GstAmcCodecType *gst_codec_type;
      jobject supported_type = NULL;
      const gchar *supported_type_str = NULL;
      jobject capabilities = NULL;
      jclass capabilities_class = NULL;
      jfieldID color_formats_id, profile_levels_id;
      jobject color_formats = NULL;
      jobject profile_levels = NULL;
      jint *color_formats_elems = NULL;
      jsize n_elems, k;

      gst_codec_type = &gst_codec_info->supported_types[j];

      supported_type = (*env)->GetObjectArrayElement (env, supported_types, j);
      if ((*env)->ExceptionCheck (env)) {
        GST_ERROR ("Failed to get %d-th supported type", j);
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      supported_type_str =
          (*env)->GetStringUTFChars (env, supported_type, NULL);
      if ((*env)->ExceptionCheck (env) || !supported_type_str) {
        GST_ERROR ("Failed to convert supported type to UTF8");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      GST_INFO ("Supported type '%s'", supported_type_str);
      gst_codec_type->mime = g_strdup (supported_type_str);

      capabilities =
          (*env)->CallObjectMethod (env, codec_info,
          get_capabilities_for_type_id, supported_type);
      if ((*env)->ExceptionCheck (env)) {
        GST_ERROR ("Failed to get capabilities for supported type");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      capabilities_class = (*env)->GetObjectClass (env, capabilities);
      if (!capabilities_class) {
        GST_ERROR ("Failed to get capabilities class");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      color_formats_id =
          (*env)->GetFieldID (env, capabilities_class, "colorFormats", "[I");
      profile_levels_id =
          (*env)->GetFieldID (env, capabilities_class, "profileLevels",
          "[Landroid/media/MediaCodecInfo$CodecProfileLevel;");
      if (!color_formats_id || !profile_levels_id) {
        GST_ERROR ("Failed to get capabilities field IDs");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      color_formats =
          (*env)->GetObjectField (env, capabilities, color_formats_id);
      if ((*env)->ExceptionCheck (env)) {
        GST_ERROR ("Failed to get color formats");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      n_elems = (*env)->GetArrayLength (env, color_formats);
      if ((*env)->ExceptionCheck (env)) {
        GST_ERROR ("Failed to get color formats array length");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }
      gst_codec_type->n_color_formats = n_elems;
      gst_codec_type->color_formats = g_new0 (gint, n_elems);
      color_formats_elems =
          (*env)->GetIntArrayElements (env, color_formats, NULL);
      if ((*env)->ExceptionCheck (env)) {
        GST_ERROR ("Failed to get color format elements");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      for (k = 0; k < n_elems; k++) {
        GST_INFO ("Color format %d: 0x%x", k, color_formats_elems[k]);
        gst_codec_type->color_formats[k] = color_formats_elems[k];
      }

      if (g_str_has_prefix (gst_codec_type->mime, "video/")) {
        if (!n_elems) {
          GST_ERROR ("No supported color formats for video codec");
          valid_codec = FALSE;
          goto next_supported_type;
        }

        if (!ignore_unknown_color_formats
            && !accepted_color_formats (gst_codec_type, is_encoder)) {
          GST_ERROR ("%s codec has unknown color formats, ignoring",
              is_encoder ? "Encoder" : "Decoder");
          valid_codec = FALSE;
          goto next_supported_type;
        }
      }

      profile_levels =
          (*env)->GetObjectField (env, capabilities, profile_levels_id);
      if ((*env)->ExceptionCheck (env)) {
        GST_ERROR ("Failed to get profile/levels");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      n_elems = (*env)->GetArrayLength (env, profile_levels);
      if ((*env)->ExceptionCheck (env)) {
        GST_ERROR ("Failed to get profile/levels array length");
        (*env)->ExceptionDescribe (env);
        (*env)->ExceptionClear (env);
        valid_codec = FALSE;
        goto next_supported_type;
      }
      gst_codec_type->n_profile_levels = n_elems;
      gst_codec_type->profile_levels =
          g_malloc0 (sizeof (gst_codec_type->profile_levels[0]) * n_elems);
      for (k = 0; k < n_elems; k++) {
        jobject profile_level = NULL;
        jclass profile_level_class = NULL;
        jfieldID level_id, profile_id;
        jint level, profile;

        profile_level = (*env)->GetObjectArrayElement (env, profile_levels, k);
        if ((*env)->ExceptionCheck (env)) {
          GST_ERROR ("Failed to get %d-th profile/level", k);
          (*env)->ExceptionDescribe (env);
          (*env)->ExceptionClear (env);
          valid_codec = FALSE;
          goto next_profile_level;
        }

        profile_level_class = (*env)->GetObjectClass (env, profile_level);
        if (!profile_level_class) {
          GST_ERROR ("Failed to get profile/level class");
          (*env)->ExceptionDescribe (env);
          (*env)->ExceptionClear (env);
          valid_codec = FALSE;
          goto next_profile_level;
        }

        level_id = (*env)->GetFieldID (env, profile_level_class, "level", "I");
        profile_id =
            (*env)->GetFieldID (env, profile_level_class, "profile", "I");
        if (!level_id || !profile_id) {
          GST_ERROR ("Failed to get profile/level field IDs");
          (*env)->ExceptionDescribe (env);
          (*env)->ExceptionClear (env);
          valid_codec = FALSE;
          goto next_profile_level;
        }

        level = (*env)->GetIntField (env, profile_level, level_id);
        if ((*env)->ExceptionCheck (env)) {
          GST_ERROR ("Failed to get level");
          (*env)->ExceptionDescribe (env);
          (*env)->ExceptionClear (env);
          valid_codec = FALSE;
          goto next_profile_level;
        }
        GST_INFO ("Level %d: 0x%08x", k, level);
        gst_codec_type->profile_levels[k].level = level;

        profile = (*env)->GetIntField (env, profile_level, profile_id);
        if ((*env)->ExceptionCheck (env)) {
          GST_ERROR ("Failed to get profile");
          (*env)->ExceptionDescribe (env);
          (*env)->ExceptionClear (env);
          valid_codec = FALSE;
          goto next_profile_level;
        }
        GST_INFO ("Profile %d: 0x%08x", k, profile);
        gst_codec_type->profile_levels[k].profile = profile;

      next_profile_level:
        if (profile_level)
          (*env)->DeleteLocalRef (env, profile_level);
        profile_level = NULL;
        if (profile_level_class)
          (*env)->DeleteLocalRef (env, profile_level_class);
        profile_level_class = NULL;
        if (!valid_codec)
          break;
      }

    next_supported_type:
      if (color_formats_elems)
        (*env)->ReleaseIntArrayElements (env, color_formats,
            color_formats_elems, JNI_ABORT);
      color_formats_elems = NULL;
      if (color_formats)
        (*env)->DeleteLocalRef (env, color_formats);
      color_formats = NULL;
      if (profile_levels)
        (*env)->DeleteLocalRef (env, profile_levels);
      color_formats = NULL;
      if (capabilities)
        (*env)->DeleteLocalRef (env, capabilities);
      capabilities = NULL;
      if (capabilities_class)
        (*env)->DeleteLocalRef (env, capabilities_class);
      capabilities_class = NULL;
      if (supported_type_str)
        (*env)->ReleaseStringUTFChars (env, supported_type, supported_type_str);
      supported_type_str = NULL;
      if (supported_type)
        (*env)->DeleteLocalRef (env, supported_type);
      supported_type = NULL;
      if (!valid_codec)
        break;
    }

    /* We need at least a valid supported type */
    if (valid_codec) {
      GST_LOG ("Successfully scanned codec '%s'", name_str);
      codec_infos = g_list_append (codec_infos, gst_codec_info);
      gst_codec_info = NULL;
    }

    /* Clean up of all local references we got */
  next_codec:
    if (name_str)
      (*env)->ReleaseStringUTFChars (env, name, name_str);
    name_str = NULL;
    if (name)
      (*env)->DeleteLocalRef (env, name);
    name = NULL;
    if (supported_types)
      (*env)->DeleteLocalRef (env, supported_types);
    supported_types = NULL;
    if (codec_info)
      (*env)->DeleteLocalRef (env, codec_info);
    codec_info = NULL;
    if (codec_info_class)
      (*env)->DeleteLocalRef (env, codec_info_class);
    codec_info_class = NULL;
    if (gst_codec_info) {
      gint j;

      for (j = 0; j < gst_codec_info->n_supported_types; j++) {
        GstAmcCodecType *gst_codec_type = &gst_codec_info->supported_types[j];

        g_free (gst_codec_type->mime);
        g_free (gst_codec_type->color_formats);
        g_free (gst_codec_type->profile_levels);
      }
      g_free (gst_codec_info->supported_types);
      g_free (gst_codec_info->name);
      g_free (gst_codec_info);
    }
    gst_codec_info = NULL;
    valid_codec = TRUE;
  }

  ret = codec_infos != NULL;

  /* If successful we store a cache of the codec information in
   * the registry. Otherwise we would always load all codecs during
   * plugin initialization which can take quite some time (because
   * of hardware) and also loads lots of shared libraries (which
   * number is limited by 64 in Android).
   */
  if (ret) {
    GstStructure *new_cache_data = gst_structure_new_empty ("gst-amc-cache");
    GList *l;
    GValue arr = { 0, };

    g_value_init (&arr, GST_TYPE_ARRAY);

    for (l = codec_infos; l; l = l->next) {
      GstAmcCodecInfo *gst_codec_info = l->data;
      GValue cv = { 0, };
      GstStructure *cs = gst_structure_new_empty ("gst-amc-codec");
      GValue starr = { 0, };
      gint i;

      gst_structure_set (cs, "name", G_TYPE_STRING, gst_codec_info->name,
          "is-encoder", G_TYPE_BOOLEAN, gst_codec_info->is_encoder, NULL);

      g_value_init (&starr, GST_TYPE_ARRAY);

      for (i = 0; i < gst_codec_info->n_supported_types; i++) {
        GstAmcCodecType *gst_codec_type = &gst_codec_info->supported_types[i];
        GstStructure *sts = gst_structure_new_empty ("gst-amc-supported-type");
        GValue stv = { 0, };
        GValue tmparr = { 0, };
        gint j;

        gst_structure_set (sts, "mime", G_TYPE_STRING, gst_codec_type->mime,
            NULL);

        g_value_init (&tmparr, GST_TYPE_ARRAY);
        for (j = 0; j < gst_codec_type->n_color_formats; j++) {
          GValue tmp = { 0, };

          g_value_init (&tmp, G_TYPE_INT);
          g_value_set_int (&tmp, gst_codec_type->color_formats[j]);
          gst_value_array_append_value (&tmparr, &tmp);
          g_value_unset (&tmp);
        }
        gst_structure_set_value (sts, "color-formats", &tmparr);
        g_value_unset (&tmparr);

        g_value_init (&tmparr, GST_TYPE_ARRAY);
        for (j = 0; j < gst_codec_type->n_profile_levels; j++) {
          GValue tmparr2 = { 0, };
          GValue tmp = { 0, };

          g_value_init (&tmparr2, GST_TYPE_ARRAY);
          g_value_init (&tmp, G_TYPE_INT);
          g_value_set_int (&tmp, gst_codec_type->profile_levels[j].profile);
          gst_value_array_append_value (&tmparr2, &tmp);
          g_value_set_int (&tmp, gst_codec_type->profile_levels[j].level);
          gst_value_array_append_value (&tmparr2, &tmp);
          gst_value_array_append_value (&tmparr, &tmparr2);
          g_value_unset (&tmp);
          g_value_unset (&tmparr2);
        }
        gst_structure_set_value (sts, "profile-levels", &tmparr);

        g_value_init (&stv, GST_TYPE_STRUCTURE);
        gst_value_set_structure (&stv, sts);
        gst_value_array_append_value (&starr, &stv);
        g_value_unset (&tmparr);
        gst_structure_free (sts);
      }

      gst_structure_set_value (cs, "supported-types", &starr);
      g_value_unset (&starr);

      g_value_init (&cv, GST_TYPE_STRUCTURE);
      gst_value_set_structure (&cv, cs);
      gst_value_array_append_value (&arr, &cv);
      g_value_unset (&cv);
      gst_structure_free (cs);
    }

    gst_structure_set_value (new_cache_data, "codecs", &arr);
    g_value_unset (&arr);

    gst_plugin_set_cache_data (plugin, new_cache_data);
  }

done:
  if (codec_list_class)
    (*env)->DeleteLocalRef (env, codec_list_class);

  return ret;
}

static const struct
{
  gint color_format;
  GstVideoFormat video_format;
} color_format_mapping_table[] = {
  {
  COLOR_FormatYUV420Planar, GST_VIDEO_FORMAT_I420}, {
  COLOR_FormatYUV420SemiPlanar, GST_VIDEO_FORMAT_NV12}, {
  COLOR_TI_FormatYUV420PackedSemiPlanar, GST_VIDEO_FORMAT_NV12}, {
  COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced, GST_VIDEO_FORMAT_NV12}, {
  COLOR_QCOM_FormatYUV420SemiPlanar, GST_VIDEO_FORMAT_NV12}, {
  COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka, GST_VIDEO_FORMAT_NV12}, {
  COLOR_QCOM_FormatYVU420SemiPlanar32m, GST_VIDEO_FORMAT_NV12}, {
  COLOR_OMX_SEC_FormatNV12Tiled, GST_VIDEO_FORMAT_NV12}, {
  COLOR_FormatYCbYCr, GST_VIDEO_FORMAT_YUY2}
};

static gboolean
accepted_color_formats (GstAmcCodecType * type, gboolean is_encoder)
{
  gint i, j;
  gint accepted = 0, all = type->n_color_formats;

  for (i = 0; i < type->n_color_formats; i++) {
    gboolean found = FALSE;
    /* We ignore this one */
    if (type->color_formats[i] == COLOR_FormatAndroidOpaque)
      all--;

    for (j = 0; j < G_N_ELEMENTS (color_format_mapping_table); j++) {
      if (color_format_mapping_table[j].color_format == type->color_formats[i]) {
        found = TRUE;
        accepted++;
        break;
      }
    }

    if (!found) {
      GST_DEBUG ("Unknown color format 0x%x, ignoring", type->color_formats[i]);
    }
  }

  if (is_encoder)
    return accepted > 0;
  else
    return accepted == all && all > 0;
}

GstVideoFormat
gst_amc_color_format_to_video_format (const GstAmcCodecInfo * codec_info,
    const gchar * mime, gint color_format)
{
  gint i;

  if (color_format == COLOR_FormatYCbYCr) {
    if (strcmp (codec_info->name, "OMX.k3.video.decoder.avc") == 0) {
      GST_INFO
          ("OMX.k3.video.decoder.avc: COLOR_FormatYCbYCr is actually GST_VIDEO_FORMAT_NV12.");
      return GST_VIDEO_FORMAT_NV12;
    }

    /* FIXME COLOR_FormatYCbYCr doesn't work properly for OMX.k3.video.encoder.avc temporarily. */
    if (strcmp (codec_info->name, "OMX.k3.video.encoder.avc") == 0) {
      GST_INFO
          ("OMX.k3.video.encoder.avc: COLOR_FormatYCbYCr is not supported yet.");
      return GST_VIDEO_FORMAT_UNKNOWN;
    }

    /* FIXME COLOR_FormatYCbYCr is not supported in gst_amc_color_format_info_set yet, mask it. */
    return GST_VIDEO_FORMAT_UNKNOWN;
  }

  if (color_format == COLOR_FormatYUV420SemiPlanar) {
    if (strcmp (codec_info->name, "OMX.k3.video.encoder.avc") == 0) {
      GST_INFO
          ("OMX.k3.video.encoder.avc: COLOR_FormatYUV420SemiPlanar is actually GST_VIDEO_FORMAT_NV21.");
      return GST_VIDEO_FORMAT_NV21;
    }
  }

  for (i = 0; i < G_N_ELEMENTS (color_format_mapping_table); i++) {
    if (color_format_mapping_table[i].color_format == color_format)
      return color_format_mapping_table[i].video_format;
  }

  return GST_VIDEO_FORMAT_UNKNOWN;
}

gint
gst_amc_video_format_to_color_format (const GstAmcCodecInfo * codec_info,
    const gchar * mime, GstVideoFormat video_format)
{
  const GstAmcCodecType *codec_type = NULL;
  gint i, j;

  for (i = 0; i < codec_info->n_supported_types; i++) {
    if (strcmp (codec_info->supported_types[i].mime, mime) == 0) {
      codec_type = &codec_info->supported_types[i];
      break;
    }
  }

  if (!codec_type)
    return -1;

  if (video_format == GST_VIDEO_FORMAT_NV12) {
    if (strcmp (codec_info->name, "OMX.k3.video.decoder.avc") == 0) {
      GST_INFO
          ("OMX.k3.video.decoder.avc: GST_VIDEO_FORMAT_NV12 is reported as COLOR_FormatYCbYCr.");

      return COLOR_FormatYCbYCr;
    }
  }

  if (video_format == GST_VIDEO_FORMAT_NV21) {
    if (strcmp (codec_info->name, "OMX.k3.video.encoder.avc") == 0) {
      GST_INFO
          ("OMX.k3.video.encoder.avc: GST_VIDEO_FORMAT_NV21 is reported as COLOR_FormatYUV420SemiPlanar.");

      return COLOR_FormatYUV420SemiPlanar;
    }
  }

  for (i = 0; i < G_N_ELEMENTS (color_format_mapping_table); i++) {
    if (color_format_mapping_table[i].video_format == video_format) {
      gint color_format = color_format_mapping_table[i].color_format;

      for (j = 0; j < codec_type->n_color_formats; j++)
        if (color_format == codec_type->color_formats[j])
          return color_format;
    }
  }

  return -1;
}

/*
 * The format is called QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka.
 * Which is actually NV12 (interleaved U&V).
 */
#define TILE_WIDTH 64
#define TILE_HEIGHT 32
#define TILE_SIZE (TILE_WIDTH * TILE_HEIGHT)
#define TILE_GROUP_SIZE (4 * TILE_SIZE)

/* get frame tile coordinate. XXX: nothing to be understood here, don't try. */
static size_t
tile_pos (size_t x, size_t y, size_t w, size_t h)
{
  size_t flim = x + (y & ~1) * w;

  if (y & 1) {
    flim += (x & ~3) + 2;
  } else if ((h & 1) == 0 || y != (h - 1)) {
    flim += (x + 2) & ~3;
  }

  return flim;
}

gboolean
gst_amc_color_format_info_set (GstAmcColorFormatInfo * color_format_info,
    const GstAmcCodecInfo * codec_info, const gchar * mime, gint color_format,
    gint width, gint height, gint stride, gint slice_height, gint crop_left,
    gint crop_right, gint crop_top, gint crop_bottom)
{
  gint frame_size = 0;

  if (color_format == COLOR_FormatYCbYCr) {
    if (strcmp (codec_info->name, "OMX.k3.video.decoder.avc") == 0)
      color_format = COLOR_FormatYUV420SemiPlanar;
  }

  /* Samsung Galaxy S3 seems to report wrong strides.
   * I.e. BigBuckBunny 854x480 H264 reports a stride of 864 when it is
   * actually 854, so we use width instead of stride here.
   * This is obviously bound to break in the future. */
  if (g_str_has_prefix (codec_info->name, "OMX.SEC.")) {
    stride = width;
  }

  if (strcmp (codec_info->name, "OMX.k3.video.decoder.avc") == 0) {
    stride = width;
    slice_height = height;
  }

  if (slice_height == 0) {
    /* NVidia Tegra 3 on Nexus 7 does not set this */
    if (g_str_has_prefix (codec_info->name, "OMX.Nvidia."))
      slice_height = GST_ROUND_UP_32 (height);
  }

  if (width == 0 || height == 0) {
    GST_ERROR ("Width or height is 0");
    return FALSE;
  }

  switch (color_format) {
    case COLOR_FormatYUV420Planar:{
      if (stride == 0 || slice_height == 0) {
        GST_ERROR ("Stride or slice height is 0");
        return FALSE;
      }

      frame_size =
          stride * slice_height + 2 * ((stride + 1) / 2) * ((slice_height +
              1) / 2);
      break;
    }
    case COLOR_TI_FormatYUV420PackedSemiPlanar:
    case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:{
      if (stride == 0 || slice_height == 0) {
        GST_ERROR ("Stride or slice height is 0");
        return FALSE;
      }

      frame_size =
          stride * (slice_height - crop_top / 2) +
          (GST_ROUND_UP_2 (stride) * ((slice_height + 1) / 2));
      break;
    }
    case COLOR_QCOM_FormatYUV420SemiPlanar:
    case COLOR_QCOM_FormatYVU420SemiPlanar32m:
    case COLOR_FormatYUV420SemiPlanar:{
      if (stride == 0 || slice_height == 0) {
        GST_ERROR ("Stride or slice height is 0");
        return FALSE;
      }

      frame_size = stride * slice_height + stride * ((slice_height + 1) / 2);
      break;
    }
    case COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:{
      const size_t tile_w = (width - 1) / TILE_WIDTH + 1;
      const size_t tile_w_align = (tile_w + 1) & ~1;
      const size_t tile_h_luma = (height - 1) / TILE_HEIGHT + 1;
      frame_size =
          tile_pos (tile_w, tile_h_luma, tile_w_align, tile_h_luma) * TILE_SIZE;
      break;
    }
    default:
      GST_ERROR ("Unsupported color format %d", color_format);
      return FALSE;
      break;
  }

  color_format_info->color_format = color_format;
  color_format_info->width = width;
  color_format_info->height = height;
  color_format_info->stride = stride;
  color_format_info->slice_height = slice_height;
  color_format_info->crop_left = crop_left;
  color_format_info->crop_right = crop_right;
  color_format_info->crop_top = crop_top;
  color_format_info->crop_bottom = crop_bottom;
  color_format_info->frame_size = frame_size;

  return TRUE;
}

/* The weird handling of cropping, alignment and everything is taken from
 * platform/frameworks/media/libstagefright/colorconversion/ColorConversion.cpp
 */
gboolean
gst_amc_color_format_copy (GstAmcColorFormatInfo * cinfo,
    GstAmcBuffer * cbuffer, const GstAmcBufferInfo * cbuffer_info,
    GstVideoInfo * vinfo, GstBuffer * vbuffer,
    GstAmcColorFormatCopyDirection direction)
{
  gboolean ret = FALSE;
  guint8 *cptr = NULL, *vptr = NULL;
  guint8 **src, **dest;

  if (direction == COLOR_FORMAT_COPY_OUT) {
    src = &cptr;
    dest = &vptr;
  } else {
    src = &vptr;
    dest = &cptr;
  }

  /* Same video format */
  if (cbuffer_info->size == gst_buffer_get_size (vbuffer)) {
    GstMapInfo minfo;

    GST_DEBUG ("Buffer sizes equal, doing fast copy");
    gst_buffer_map (vbuffer, &minfo, GST_MAP_WRITE);

    cptr = cbuffer->data + cbuffer_info->offset;
    vptr = minfo.data;
    orc_memcpy (*dest, *src, cbuffer_info->size);

    gst_buffer_unmap (vbuffer, &minfo);
    ret = TRUE;
    goto done;
  }

  GST_DEBUG ("Sizes not equal (%d vs %d), doing slow line-by-line copying",
      cbuffer_info->size, gst_buffer_get_size (vbuffer));

  /* Different video format, try to convert */
  switch (cinfo->color_format) {
    case COLOR_FormatYUV420Planar:{
      GstVideoFrame vframe;
      gint i, j, height;
      gint stride, slice_height;
      gint c_stride, v_stride;
      gint row_length;

      stride = cinfo->stride;
      slice_height = cinfo->slice_height;
      g_assert (stride > 0 && slice_height > 0);

      gst_video_frame_map (&vframe, vinfo, vbuffer, GST_MAP_WRITE);

      for (i = 0; i < 3; i++) {
        if (i == 0) {
          c_stride = stride;
          v_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
        } else {
          c_stride = (stride + 1) / 2;
          v_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
        }

        cptr = cbuffer->data + cbuffer_info->offset;

        if (i == 0) {
          cptr += cinfo->crop_top * stride;
          cptr += cinfo->crop_left;
          row_length = cinfo->width;
        } else if (i > 0) {
          /* skip the Y plane */
          cptr += slice_height * stride;

          /* crop_top/crop_left divided by two
           * because one byte of the U/V planes
           * corresponds to two pixels horizontally/vertically */
          cptr += cinfo->crop_top / 2 * c_stride;
          cptr += cinfo->crop_left / 2;
          row_length = (cinfo->width + 1) / 2;
        }
        if (i == 2) {
          /* skip the U plane */
          cptr += ((slice_height + 1) / 2) * ((stride + 1) / 2);
        }

        vptr = GST_VIDEO_FRAME_COMP_DATA (&vframe, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, i);

        for (j = 0; j < height; j++) {
          orc_memcpy (*dest, *src, row_length);
          cptr += c_stride;
          vptr += v_stride;
        }
      }
      gst_video_frame_unmap (&vframe);
      ret = TRUE;
      break;
    }
    case COLOR_TI_FormatYUV420PackedSemiPlanar:
    case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:{
      gint i, j, height;
      gint c_stride, v_stride;
      gint row_length;
      GstVideoFrame vframe;

      /* This should always be set */
      g_assert (cinfo->stride > 0 && cinfo->slice_height > 0);

      /* FIXME: This does not work for odd widths or heights
       * but might as well be a bug in the codec */
      gst_video_frame_map (&vframe, vinfo, vbuffer, GST_MAP_WRITE);
      for (i = 0; i < 2; i++) {
        if (i == 0) {
          c_stride = cinfo->stride;
          v_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
        } else {
          c_stride = GST_ROUND_UP_2 (cinfo->stride);
          v_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);
        }

        cptr = cbuffer->data + cbuffer_info->offset;
        if (i == 0) {
          row_length = cinfo->width;
        } else if (i == 1) {
          cptr += (cinfo->slice_height - cinfo->crop_top / 2) * cinfo->stride;
          row_length = GST_ROUND_UP_2 (cinfo->width);
        }

        vptr = GST_VIDEO_FRAME_COMP_DATA (&vframe, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, i);

        for (j = 0; j < height; j++) {
          orc_memcpy (*dest, *src, row_length);
          cptr += c_stride;
          vptr += v_stride;
        }
      }
      gst_video_frame_unmap (&vframe);
      ret = TRUE;
      break;
    }
    case COLOR_QCOM_FormatYUV420SemiPlanar:
    case COLOR_QCOM_FormatYVU420SemiPlanar32m:
    case COLOR_FormatYUV420SemiPlanar:{
      gint i, j, height;
      gint c_stride, v_stride;
      gint row_length;
      GstVideoFrame vframe;

      /* This should always be set */
      g_assert (cinfo->stride > 0 && cinfo->slice_height > 0);

      gst_video_frame_map (&vframe, vinfo, vbuffer, GST_MAP_WRITE);

      for (i = 0; i < 2; i++) {
        c_stride = cinfo->stride;
        v_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, i);

        cptr = cbuffer->data + cbuffer_info->offset;
        if (i == 0) {
          cptr += cinfo->crop_top * cinfo->stride;
          cptr += cinfo->crop_left;
          row_length = cinfo->width;
        } else if (i == 1) {
          cptr += cinfo->slice_height * cinfo->stride;
          cptr += cinfo->crop_top * cinfo->stride;
          cptr += cinfo->crop_left;
          row_length = cinfo->width;
        }

        vptr = GST_VIDEO_FRAME_COMP_DATA (&vframe, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, i);

        for (j = 0; j < height; j++) {
          orc_memcpy (*dest, *src, row_length);
          cptr += c_stride;
          vptr += v_stride;
        }
      }
      gst_video_frame_unmap (&vframe);
      ret = TRUE;
      break;
    }
      /* FIXME: This should be in libgstvideo as MT12 or similar, see v4l2 */
    case COLOR_QCOM_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:{
      GstVideoFrame vframe;
      gint width = cinfo->width;
      gint height = cinfo->height;
      gint v_luma_stride, v_chroma_stride;
      guint8 *cdata = cbuffer->data + cbuffer_info->offset;
      guint8 *v_luma, *v_chroma;
      gint y;
      const size_t tile_w = (width - 1) / TILE_WIDTH + 1;
      const size_t tile_w_align = (tile_w + 1) & ~1;
      const size_t tile_h_luma = (height - 1) / TILE_HEIGHT + 1;
      const size_t tile_h_chroma = (height / 2 - 1) / TILE_HEIGHT + 1;
      size_t luma_size = tile_w_align * tile_h_luma * TILE_SIZE;

      gst_video_frame_map (&vframe, vinfo, vbuffer, GST_MAP_WRITE);
      v_luma = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
      v_chroma = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 1);
      v_luma_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, 0);
      v_chroma_stride = GST_VIDEO_FRAME_COMP_STRIDE (&vframe, 1);

      if ((luma_size % TILE_GROUP_SIZE) != 0)
        luma_size = (((luma_size - 1) / TILE_GROUP_SIZE) + 1) * TILE_GROUP_SIZE;

      for (y = 0; y < tile_h_luma; y++) {
        size_t row_width = width;
        gint x;

        for (x = 0; x < tile_w; x++) {
          size_t tile_width = row_width;
          size_t tile_height = height;
          gint luma_idx;
          gint chroma_idx;
          /* luma source pointer for this tile */
          uint8_t *c_luma =
              cdata + tile_pos (x, y, tile_w_align, tile_h_luma) * TILE_SIZE;

          /* chroma source pointer for this tile */
          uint8_t *c_chroma =
              cdata + luma_size + tile_pos (x, y / 2, tile_w_align,
              tile_h_chroma) * TILE_SIZE;
          if (y & 1)
            c_chroma += TILE_SIZE / 2;

          /* account for right columns */
          if (tile_width > TILE_WIDTH)
            tile_width = TILE_WIDTH;

          /* account for bottom rows */
          if (tile_height > TILE_HEIGHT)
            tile_height = TILE_HEIGHT;

          /* vptr luma memory index for this tile */
          luma_idx = y * TILE_HEIGHT * v_luma_stride + x * TILE_WIDTH;

          /* vptr chroma memory index for this tile */
          /* XXX: remove divisions */
          chroma_idx = y * TILE_HEIGHT / 2 * v_chroma_stride + x * TILE_WIDTH;

          tile_height /= 2;     // we copy 2 luma lines at once
          while (tile_height--) {
            vptr = v_luma + luma_idx;
            cptr = c_luma;
            memcpy (*dest, *src, tile_width);
            c_luma += TILE_WIDTH;
            luma_idx += v_luma_stride;

            vptr = v_luma + luma_idx;
            cptr = c_luma;
            memcpy (*dest, *src, tile_width);
            c_luma += TILE_WIDTH;
            luma_idx += v_luma_stride;

            vptr = v_chroma + chroma_idx;
            cptr = c_chroma;
            memcpy (*dest, *src, tile_width);
            c_chroma += TILE_WIDTH;
            chroma_idx += v_chroma_stride;
          }
          row_width -= TILE_WIDTH;
        }
        height -= TILE_HEIGHT;
      }
      gst_video_frame_unmap (&vframe);
      ret = TRUE;
      break;

    }
    default:
      GST_ERROR ("Unsupported color format %d", cinfo->color_format);
      goto done;
      break;
  }

done:
  return ret;
}

static const struct
{
  gint id;
  const gchar *str;
  const gchar *alt_str;
} avc_profile_mapping_table[] = {
  {
  AVCProfileBaseline, "baseline", "constrained-baseline"}, {
  AVCProfileMain, "main", NULL}, {
  AVCProfileExtended, "extended", NULL}, {
  AVCProfileHigh, "high"}, {
  AVCProfileHigh10, "high-10", "high-10-intra"}, {
  AVCProfileHigh422, "high-4:2:2", "high-4:2:2-intra"}, {
  AVCProfileHigh444, "high-4:4:4", "high-4:4:4-intra"}
};

const gchar *
gst_amc_avc_profile_to_string (gint profile, const gchar ** alternative)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (avc_profile_mapping_table); i++) {
    if (avc_profile_mapping_table[i].id == profile) {
      *alternative = avc_profile_mapping_table[i].alt_str;
      return avc_profile_mapping_table[i].str;
    }
  }

  return NULL;
}

gint
gst_amc_avc_profile_from_string (const gchar * profile)
{
  gint i;

  g_return_val_if_fail (profile != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (avc_profile_mapping_table); i++) {
    if (strcmp (avc_profile_mapping_table[i].str, profile) == 0 ||
        (avc_profile_mapping_table[i].alt_str &&
            strcmp (avc_profile_mapping_table[i].alt_str, profile) == 0))
      return avc_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} avc_level_mapping_table[] = {
  {
  AVCLevel1, "1"}, {
  AVCLevel1b, "1b"}, {
  AVCLevel11, "1.1"}, {
  AVCLevel12, "1.2"}, {
  AVCLevel13, "1.3"}, {
  AVCLevel2, "2"}, {
  AVCLevel21, "2.1"}, {
  AVCLevel22, "2.2"}, {
  AVCLevel3, "3"}, {
  AVCLevel31, "3.1"}, {
  AVCLevel32, "3.2"}, {
  AVCLevel4, "4"}, {
  AVCLevel41, "4.1"}, {
  AVCLevel42, "4.2"}, {
  AVCLevel5, "5"}, {
  AVCLevel51, "5.1"}
};

const gchar *
gst_amc_avc_level_to_string (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (avc_level_mapping_table); i++) {
    if (avc_level_mapping_table[i].id == level)
      return avc_level_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_avc_level_from_string (const gchar * level)
{
  gint i;

  g_return_val_if_fail (level != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (avc_level_mapping_table); i++) {
    if (strcmp (avc_level_mapping_table[i].str, level) == 0)
      return avc_level_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  gint gst_id;
} h263_profile_mapping_table[] = {
  {
  H263ProfileBaseline, 0}, {
  H263ProfileH320Coding, 1}, {
  H263ProfileBackwardCompatible, 2}, {
  H263ProfileISWV2, 3}, {
  H263ProfileISWV3, 4}, {
  H263ProfileHighCompression, 5}, {
  H263ProfileInternet, 6}, {
  H263ProfileInterlace, 7}, {
  H263ProfileHighLatency, 8}
};

gint
gst_amc_h263_profile_to_gst_id (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_profile_mapping_table); i++) {
    if (h263_profile_mapping_table[i].id == profile)
      return h263_profile_mapping_table[i].gst_id;
  }

  return -1;
}

gint
gst_amc_h263_profile_from_gst_id (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_profile_mapping_table); i++) {
    if (h263_profile_mapping_table[i].gst_id == profile)
      return h263_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  gint gst_id;
} h263_level_mapping_table[] = {
  {
  H263Level10, 10}, {
  H263Level20, 20}, {
  H263Level30, 30}, {
  H263Level40, 40}, {
  H263Level50, 50}, {
  H263Level60, 60}, {
  H263Level70, 70}
};

gint
gst_amc_h263_level_to_gst_id (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_level_mapping_table); i++) {
    if (h263_level_mapping_table[i].id == level)
      return h263_level_mapping_table[i].gst_id;
  }

  return -1;
}

gint
gst_amc_h263_level_from_gst_id (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_level_mapping_table); i++) {
    if (h263_level_mapping_table[i].gst_id == level)
      return h263_level_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} mpeg4_profile_mapping_table[] = {
  {
  MPEG4ProfileSimple, "simple"}, {
  MPEG4ProfileSimpleScalable, "simple-scalable"}, {
  MPEG4ProfileCore, "core"}, {
  MPEG4ProfileMain, "main"}, {
  MPEG4ProfileNbit, "n-bit"}, {
  MPEG4ProfileScalableTexture, "scalable"}, {
  MPEG4ProfileSimpleFace, "simple-face"}, {
  MPEG4ProfileSimpleFBA, "simple-fba"}, {
  MPEG4ProfileBasicAnimated, "basic-animated-texture"}, {
  MPEG4ProfileHybrid, "hybrid"}, {
  MPEG4ProfileAdvancedRealTime, "advanced-real-time"}, {
  MPEG4ProfileCoreScalable, "core-scalable"}, {
  MPEG4ProfileAdvancedCoding, "advanced-coding-efficiency"}, {
  MPEG4ProfileAdvancedCore, "advanced-core"}, {
  MPEG4ProfileAdvancedScalable, "advanced-scalable-texture"}, {
  MPEG4ProfileAdvancedSimple, "advanced-simple"}
};

const gchar *
gst_amc_mpeg4_profile_to_string (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (mpeg4_profile_mapping_table); i++) {
    if (mpeg4_profile_mapping_table[i].id == profile)
      return mpeg4_profile_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_avc_mpeg4_profile_from_string (const gchar * profile)
{
  gint i;

  g_return_val_if_fail (profile != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (mpeg4_profile_mapping_table); i++) {
    if (strcmp (mpeg4_profile_mapping_table[i].str, profile) == 0)
      return mpeg4_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} mpeg4_level_mapping_table[] = {
  {
  MPEG4Level0, "0"}, {
  MPEG4Level0b, "0b"}, {
  MPEG4Level1, "1"}, {
  MPEG4Level2, "2"}, {
  MPEG4Level3, "3"}, {
  MPEG4Level4, "4"}, {
  MPEG4Level4a, "4a"}, {
MPEG4Level5, "5"},};

const gchar *
gst_amc_mpeg4_level_to_string (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (mpeg4_level_mapping_table); i++) {
    if (mpeg4_level_mapping_table[i].id == level)
      return mpeg4_level_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_mpeg4_level_from_string (const gchar * level)
{
  gint i;

  g_return_val_if_fail (level != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (mpeg4_level_mapping_table); i++) {
    if (strcmp (mpeg4_level_mapping_table[i].str, level) == 0)
      return mpeg4_level_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} aac_profile_mapping_table[] = {
  {
  AACObjectMain, "main"}, {
  AACObjectLC, "lc"}, {
  AACObjectSSR, "ssr"}, {
  AACObjectLTP, "ltp"}
};

const gchar *
gst_amc_aac_profile_to_string (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (aac_profile_mapping_table); i++) {
    if (aac_profile_mapping_table[i].id == profile)
      return aac_profile_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_aac_profile_from_string (const gchar * profile)
{
  gint i;

  g_return_val_if_fail (profile != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (aac_profile_mapping_table); i++) {
    if (strcmp (aac_profile_mapping_table[i].str, profile) == 0)
      return aac_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  guint32 mask;
  GstAudioChannelPosition pos;
} channel_mapping_table[] = {
  {
  CHANNEL_OUT_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT}, {
  CHANNEL_OUT_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT}, {
  CHANNEL_OUT_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER}, {
  CHANNEL_OUT_LOW_FREQUENCY, GST_AUDIO_CHANNEL_POSITION_LFE1}, {
  CHANNEL_OUT_BACK_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_LEFT}, {
  CHANNEL_OUT_BACK_RIGHT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT}, {
  CHANNEL_OUT_FRONT_LEFT_OF_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER}, {
  CHANNEL_OUT_FRONT_RIGHT_OF_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER}, {
  CHANNEL_OUT_BACK_CENTER, GST_AUDIO_CHANNEL_POSITION_REAR_CENTER}, {
  CHANNEL_OUT_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT}, {
  CHANNEL_OUT_SIDE_RIGHT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT}, {
  CHANNEL_OUT_TOP_CENTER, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_BACK_LEFT, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_BACK_CENTER, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_BACK_RIGHT, GST_AUDIO_CHANNEL_POSITION_INVALID}
};

gboolean
gst_amc_audio_channel_mask_to_positions (guint32 channel_mask, gint channels,
    GstAudioChannelPosition * pos)
{
  gint i, j;

  if (channel_mask == 0) {
    if (channels == 1) {
      pos[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
      return TRUE;
    }
    if (channels == 2) {
      pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
      pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
      return TRUE;
    }

    /* Now let the guesswork begin, these are the
     * AAC default channel assignments for these numbers
     * of channels */
    if (channels == 3) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER;
    } else if (channels == 4) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_CENTER;
    } else if (channels == 5) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_LEFT |
          CHANNEL_OUT_BACK_RIGHT;
    } else if (channels == 6) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_LEFT |
          CHANNEL_OUT_BACK_RIGHT | CHANNEL_OUT_LOW_FREQUENCY;
    } else if (channels == 8) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_LEFT |
          CHANNEL_OUT_BACK_RIGHT | CHANNEL_OUT_LOW_FREQUENCY |
          CHANNEL_OUT_FRONT_LEFT_OF_CENTER | CHANNEL_OUT_FRONT_RIGHT_OF_CENTER;
    }
  }

  for (i = 0, j = 0; i < G_N_ELEMENTS (channel_mapping_table); i++) {
    if ((channel_mask & channel_mapping_table[i].mask)) {
      pos[j++] = channel_mapping_table[i].pos;
      if (channel_mapping_table[i].pos == GST_AUDIO_CHANNEL_POSITION_INVALID) {
        memset (pos, 0, sizeof (GstAudioChannelPosition) * channels);
        GST_ERROR ("Unable to map channel mask 0x%08x",
            channel_mapping_table[i].mask);
        return FALSE;
      }
      if (j == channels)
        break;
    }
  }

  if (j != channels) {
    memset (pos, 0, sizeof (GstAudioChannelPosition) * channels);
    GST_ERROR ("Unable to map all channel positions in mask 0x%08x",
        channel_mask);
    return FALSE;
  }

  return TRUE;
}

guint32
gst_amc_audio_channel_mask_from_positions (GstAudioChannelPosition * positions,
    gint channels)
{
  gint i, j;
  guint32 channel_mask = 0;

  if (channels == 1 && !positions)
    return CHANNEL_OUT_FRONT_CENTER;
  if (channels == 2 && !positions)
    return CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT;

  for (i = 0; i < channels; i++) {
    if (positions[i] == GST_AUDIO_CHANNEL_POSITION_INVALID)
      return 0;

    for (j = 0; j < G_N_ELEMENTS (channel_mapping_table); j++) {
      if (channel_mapping_table[j].pos == positions[i]) {
        channel_mask |= channel_mapping_table[j].mask;
        break;
      }
    }

    if (j == G_N_ELEMENTS (channel_mapping_table)) {
      GST_ERROR ("Unable to map channel position %d", positions[i]);
      return 0;
    }
  }

  return channel_mask;
}

static gchar *
create_type_name (const gchar * parent_name, const gchar * codec_name)
{
  gchar *typified_name;
  gint i, k;
  gint parent_name_len = strlen (parent_name);
  gint codec_name_len = strlen (codec_name);
  gboolean upper = TRUE;

  typified_name = g_new0 (gchar, parent_name_len + 1 + strlen (codec_name) + 1);
  memcpy (typified_name, parent_name, parent_name_len);
  typified_name[parent_name_len] = '-';

  for (i = 0, k = 0; i < codec_name_len; i++) {
    if (g_ascii_isalnum (codec_name[i])) {
      if (upper)
        typified_name[parent_name_len + 1 + k++] =
            g_ascii_toupper (codec_name[i]);
      else
        typified_name[parent_name_len + 1 + k++] =
            g_ascii_tolower (codec_name[i]);

      upper = FALSE;
    } else {
      /* Skip all non-alnum chars and start a new upper case word */
      upper = TRUE;
    }
  }

  return typified_name;
}

static gchar *
create_element_name (gboolean video, gboolean encoder, const gchar * codec_name)
{
#define PREFIX_LEN 10
  static const gchar *prefixes[] = {
    "amcviddec-",
    "amcauddec-",
    "amcvidenc-",
    "amcaudenc-"
  };
  gchar *element_name;
  gint i, k;
  gint codec_name_len = strlen (codec_name);
  const gchar *prefix;

  if (video && !encoder)
    prefix = prefixes[0];
  else if (!video && !encoder)
    prefix = prefixes[1];
  else if (video && encoder)
    prefix = prefixes[2];
  else
    prefix = prefixes[3];

  element_name = g_new0 (gchar, PREFIX_LEN + strlen (codec_name) + 1);
  memcpy (element_name, prefix, PREFIX_LEN);

  for (i = 0, k = 0; i < codec_name_len; i++) {
    if (g_ascii_isalnum (codec_name[i])) {
      element_name[PREFIX_LEN + k++] = g_ascii_tolower (codec_name[i]);
    }
    /* Skip all non-alnum chars */
  }

  return element_name;
}

#undef PREFIX_LEN

static gboolean
register_codecs (GstPlugin * plugin)
{
  gboolean ret = TRUE;
  GList *l;

  GST_DEBUG ("Registering plugins");

  for (l = codec_infos; l; l = l->next) {
    GstAmcCodecInfo *codec_info = l->data;
    gboolean is_audio = FALSE;
    gboolean is_video = FALSE;
    gint i;
    gint n_types;

    GST_DEBUG ("Registering codec '%s'", codec_info->name);
    for (i = 0; i < codec_info->n_supported_types; i++) {
      GstAmcCodecType *codec_type = &codec_info->supported_types[i];

      if (g_str_has_prefix (codec_type->mime, "audio/"))
        is_audio = TRUE;
      else if (g_str_has_prefix (codec_type->mime, "video/"))
        is_video = TRUE;
    }

    n_types = 0;
    if (is_audio)
      n_types++;
    if (is_video)
      n_types++;

    for (i = 0; i < n_types; i++) {
      GTypeQuery type_query;
      GTypeInfo type_info = { 0, };
      GType type, subtype;
      gchar *type_name, *element_name;
      guint rank;

      if (is_video) {
        if (codec_info->is_encoder)
          type = gst_amc_video_enc_get_type ();
        else
          type = gst_amc_video_dec_get_type ();
      } else if (is_audio && !codec_info->is_encoder) {
        type = gst_amc_audio_dec_get_type ();
      } else {
        GST_DEBUG ("Skipping unsupported codec type");
        continue;
      }

      g_type_query (type, &type_query);
      memset (&type_info, 0, sizeof (type_info));
      type_info.class_size = type_query.class_size;
      type_info.instance_size = type_query.instance_size;
      type_name = create_type_name (type_query.type_name, codec_info->name);

      if (g_type_from_name (type_name) != G_TYPE_INVALID) {
        GST_ERROR ("Type '%s' already exists for codec '%s'", type_name,
            codec_info->name);
        g_free (type_name);
        continue;
      }

      subtype = g_type_register_static (type, type_name, &type_info, 0);
      g_free (type_name);

      g_type_set_qdata (subtype, gst_amc_codec_info_quark, codec_info);

      element_name =
          create_element_name (is_video, codec_info->is_encoder,
          codec_info->name);

      /* Give the Google software codec a secondary rank,
       * everything else is likely a hardware codec, except
       * OMX.SEC.*.sw.dec (as seen in Galaxy S4) */
      if (g_str_has_prefix (codec_info->name, "OMX.google") ||
          g_str_has_suffix (codec_info->name, ".sw.dec"))
        rank = GST_RANK_SECONDARY;
      else
        rank = GST_RANK_PRIMARY;

      ret |= gst_element_register (plugin, element_name, rank, subtype);
      g_free (element_name);

      is_video = FALSE;
    }
  }

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  const gchar *ignore;

  GST_DEBUG_CATEGORY_INIT (gst_amc_debug, "amc", 0, "android-media-codec");

  pthread_key_create (&current_jni_env, gst_amc_detach_current_thread);

  if (!initialize_java_vm ())
    return FALSE;

  gst_plugin_add_dependency_simple (plugin, NULL, "/etc", "media_codecs.xml",
      GST_PLUGIN_DEPENDENCY_FLAG_NONE);

  if (!get_java_classes ())
    return FALSE;

  /* Set this to TRUE to allow registering decoders that have
   * any unknown color formats, or encoders that only have
   * unknown color formats
   */
  ignore = g_getenv ("GST_AMC_IGNORE_UNKNOWN_COLOR_FORMATS");
  if (ignore && strcmp (ignore, "yes") == 0)
    ignore_unknown_color_formats = TRUE;

  if (!scan_codecs (plugin))
    return FALSE;

  gst_amc_codec_info_quark = g_quark_from_static_string ("gst-amc-codec-info");

  if (!register_codecs (plugin))
    return FALSE;

  return TRUE;
}

void
gst_amc_codec_info_to_caps (const GstAmcCodecInfo * codec_info,
    GstCaps ** sink_caps, GstCaps ** src_caps)
{
  GstCaps *raw_ret = NULL, *encoded_ret = NULL;
  gint i;

  if (codec_info->is_encoder) {
    if (sink_caps)
      *sink_caps = raw_ret = gst_caps_new_empty ();

    if (src_caps)
      *src_caps = encoded_ret = gst_caps_new_empty ();
  } else {
    if (sink_caps)
      *sink_caps = encoded_ret = gst_caps_new_empty ();

    if (src_caps)
      *src_caps = raw_ret = gst_caps_new_empty ();
  }

  for (i = 0; i < codec_info->n_supported_types; i++) {
    const GstAmcCodecType *type = &codec_info->supported_types[i];
    GstStructure *tmp, *tmp2, *tmp3;

    if (g_str_has_prefix (type->mime, "audio/")) {
      if (raw_ret) {
        tmp = gst_structure_new ("audio/x-raw",
            "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT,
            "format", G_TYPE_STRING, GST_AUDIO_NE (S16), NULL);

        raw_ret = gst_caps_merge_structure (raw_ret, tmp);
      }

      if (encoded_ret) {
        if (strcmp (type->mime, "audio/mpeg") == 0) {
          tmp = gst_structure_new ("audio/mpeg",
              "mpegversion", G_TYPE_INT, 1,
              "layer", G_TYPE_INT, 3,
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "parsed", G_TYPE_BOOLEAN, TRUE, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "audio/3gpp") == 0) {
          tmp = gst_structure_new ("audio/AMR",
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "audio/amr-wb") == 0) {
          tmp = gst_structure_new ("audio/AMR-WB",
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "audio/mp4a-latm") == 0) {
          gint j;
          gboolean have_profile = FALSE;
          GValue va = { 0, };
          GValue v = { 0, };

          g_value_init (&va, GST_TYPE_LIST);
          g_value_init (&v, G_TYPE_STRING);
          g_value_set_string (&v, "raw");
          gst_value_list_append_value (&va, &v);
          g_value_set_string (&v, "adts");
          gst_value_list_append_value (&va, &v);
          g_value_unset (&v);

          tmp = gst_structure_new ("audio/mpeg",
              "mpegversion", G_TYPE_INT, 4,
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "framed", G_TYPE_BOOLEAN, TRUE, NULL);
          gst_structure_set_value (tmp, "stream-format", &va);
          g_value_unset (&va);

          for (j = 0; j < type->n_profile_levels; j++) {
            const gchar *profile;

            profile =
                gst_amc_aac_profile_to_string (type->profile_levels[j].profile);

            if (!profile) {
              GST_ERROR ("Unable to map AAC profile 0x%08x",
                  type->profile_levels[j].profile);
              continue;
            }

            tmp2 = gst_structure_copy (tmp);
            gst_structure_set (tmp2, "profile", G_TYPE_STRING, profile, NULL);
            encoded_ret = gst_caps_merge_structure (encoded_ret, tmp2);

            have_profile = TRUE;
          }

          if (!have_profile) {
            encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
          } else {
            gst_structure_free (tmp);
          }
        } else if (strcmp (type->mime, "audio/g711-alaw") == 0) {
          tmp = gst_structure_new ("audio/x-alaw",
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "audio/g711-mlaw") == 0) {
          tmp = gst_structure_new ("audio/x-mulaw",
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "audio/vorbis") == 0) {
          tmp = gst_structure_new ("audio/x-vorbis",
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "audio/flac") == 0) {
          tmp = gst_structure_new ("audio/x-flac",
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "framed", G_TYPE_BOOLEAN, TRUE, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "audio/mpeg-L2") == 0) {
          tmp = gst_structure_new ("audio/mpeg",
              "mpegversion", G_TYPE_INT, 1,
              "layer", G_TYPE_INT, 2,
              "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "channels", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "parsed", G_TYPE_BOOLEAN, TRUE, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else {
          GST_WARNING ("Unsupported mimetype '%s'", type->mime);
        }
      }
    } else if (g_str_has_prefix (type->mime, "video/")) {
      if (raw_ret) {
        gint j;

        for (j = 0; j < type->n_color_formats; j++) {
          GstVideoFormat format;

          format =
              gst_amc_color_format_to_video_format (codec_info,
              type->mime, type->color_formats[j]);
          if (format == GST_VIDEO_FORMAT_UNKNOWN) {
            GST_WARNING ("Unknown color format 0x%08x", type->color_formats[j]);
            continue;
          }

          tmp = gst_structure_new ("video/x-raw",
              "format", G_TYPE_STRING, gst_video_format_to_string (format),
              "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
              "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

          raw_ret = gst_caps_merge_structure (raw_ret, tmp);
        }
      }

      if (encoded_ret) {
        if (strcmp (type->mime, "video/mp4v-es") == 0) {
          gint j;
          gboolean have_profile_level = FALSE;

          tmp = gst_structure_new ("video/mpeg",
              "width", GST_TYPE_INT_RANGE, 16, 4096,
              "height", GST_TYPE_INT_RANGE, 16, 4096,
              "framerate", GST_TYPE_FRACTION_RANGE,
              0, 1, G_MAXINT, 1,
              "mpegversion", G_TYPE_INT, 4,
              "systemstream", G_TYPE_BOOLEAN, FALSE,
              "parsed", G_TYPE_BOOLEAN, TRUE, NULL);

          if (type->n_profile_levels) {
            for (j = type->n_profile_levels - 1; j >= 0; j--) {
              const gchar *profile;

              profile =
                  gst_amc_mpeg4_profile_to_string (type->profile_levels[j].
                  profile);
              if (!profile) {
                GST_ERROR ("Unable to map MPEG4 profile 0x%08x",
                    type->profile_levels[j].profile);
                continue;
              }

              tmp2 = gst_structure_copy (tmp);
              gst_structure_set (tmp2, "profile", G_TYPE_STRING, profile, NULL);

              /* Don't put the level restrictions on the sinkpad caps for decoders,
               * see 2b94641a4 */
              if (codec_info->is_encoder) {
                const gchar *level;
                gint k;
                GValue va = { 0, };
                GValue v = { 0, };

                g_value_init (&va, GST_TYPE_LIST);
                g_value_init (&v, G_TYPE_STRING);

                for (k = 1; k <= type->profile_levels[j].level && k != 0;
                    k <<= 1) {
                  level = gst_amc_mpeg4_level_to_string (k);
                  if (!level)
                    continue;

                  g_value_set_string (&v, level);
                  gst_value_list_append_value (&va, &v);
                  g_value_reset (&v);
                }

                gst_structure_set_value (tmp2, "level", &va);
                g_value_unset (&va);
                g_value_unset (&v);
              }

              encoded_ret = gst_caps_merge_structure (encoded_ret, tmp2);
              have_profile_level = TRUE;
            }
          }

          if (!have_profile_level) {
            encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
          } else {
            gst_structure_free (tmp);
          }

          tmp = gst_structure_new ("video/x-divx",
              "width", GST_TYPE_INT_RANGE, 16, 4096,
              "height", GST_TYPE_INT_RANGE, 16, 4096,
              "framerate", GST_TYPE_FRACTION_RANGE,
              0, 1, G_MAXINT, 1,
              "divxversion", GST_TYPE_INT_RANGE, 3, 5,
              "parsed", G_TYPE_BOOLEAN, TRUE, NULL);
          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "video/3gpp") == 0) {
          gint j;
          gboolean have_profile_level = FALSE;

          tmp = gst_structure_new ("video/x-h263",
              "width", GST_TYPE_INT_RANGE, 16, 4096,
              "height", GST_TYPE_INT_RANGE, 16, 4096,
              "framerate", GST_TYPE_FRACTION_RANGE,
              0, 1, G_MAXINT, 1,
              "parsed", G_TYPE_BOOLEAN, TRUE,
              "variant", G_TYPE_STRING, "itu", NULL);

          if (type->n_profile_levels) {
            for (j = type->n_profile_levels - 1; j >= 0; j--) {
              gint profile;

              profile =
                  gst_amc_h263_profile_to_gst_id (type->profile_levels[j].
                  profile);

              if (profile == -1) {
                GST_ERROR ("Unable to map h263 profile 0x%08x",
                    type->profile_levels[j].profile);
                continue;
              }

              tmp2 = gst_structure_copy (tmp);
              gst_structure_set (tmp2, "profile", G_TYPE_UINT, profile, NULL);

              if (codec_info->is_encoder) {
                gint k;
                gint level;
                GValue va = { 0, };
                GValue v = { 0, };

                g_value_init (&va, GST_TYPE_LIST);
                g_value_init (&v, G_TYPE_UINT);

                for (k = 1; k <= type->profile_levels[j].level && k != 0;
                    k <<= 1) {
                  level = gst_amc_h263_level_to_gst_id (k);
                  if (level == -1)
                    continue;

                  g_value_set_uint (&v, level);
                  gst_value_list_append_value (&va, &v);
                  g_value_reset (&v);
                }

                gst_structure_set_value (tmp2, "level", &va);
                g_value_unset (&va);
                g_value_unset (&v);
              }

              encoded_ret = gst_caps_merge_structure (encoded_ret, tmp2);
              have_profile_level = TRUE;
            }
          }

          if (!have_profile_level) {
            encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
          } else {
            gst_structure_free (tmp);
          }
        } else if (strcmp (type->mime, "video/avc") == 0) {
          gint j;
          gboolean have_profile_level = FALSE;

          tmp = gst_structure_new ("video/x-h264",
              "width", GST_TYPE_INT_RANGE, 16, 4096,
              "height", GST_TYPE_INT_RANGE, 16, 4096,
              "framerate", GST_TYPE_FRACTION_RANGE,
              0, 1, G_MAXINT, 1,
              "parsed", G_TYPE_BOOLEAN, TRUE,
              "stream-format", G_TYPE_STRING, "byte-stream",
              "alignment", G_TYPE_STRING, "au", NULL);

          if (type->n_profile_levels) {
            for (j = type->n_profile_levels - 1; j >= 0; j--) {
              const gchar *profile, *alternative = NULL;

              profile =
                  gst_amc_avc_profile_to_string (type->profile_levels[j].
                  profile, &alternative);

              if (!profile) {
                GST_ERROR ("Unable to map H264 profile 0x%08x",
                    type->profile_levels[j].profile);
                continue;
              }

              tmp2 = gst_structure_copy (tmp);
              gst_structure_set (tmp2, "profile", G_TYPE_STRING, profile, NULL);

              if (alternative) {
                tmp3 = gst_structure_copy (tmp);
                gst_structure_set (tmp3, "profile", G_TYPE_STRING, alternative,
                    NULL);
              } else
                tmp3 = NULL;

              if (codec_info->is_encoder) {
                const gchar *level;
                gint k;
                GValue va = { 0, };
                GValue v = { 0, };

                g_value_init (&va, GST_TYPE_LIST);
                g_value_init (&v, G_TYPE_STRING);
                for (k = 1; k <= type->profile_levels[j].level && k != 0;
                    k <<= 1) {
                  level = gst_amc_avc_level_to_string (k);
                  if (!level)
                    continue;

                  g_value_set_string (&v, level);
                  gst_value_list_append_value (&va, &v);
                  g_value_reset (&v);
                }

                gst_structure_set_value (tmp2, "level", &va);
                if (tmp3)
                  gst_structure_set_value (tmp3, "level", &va);

                g_value_unset (&va);
                g_value_unset (&v);
              }

              encoded_ret = gst_caps_merge_structure (encoded_ret, tmp2);
              if (tmp3)
                encoded_ret = gst_caps_merge_structure (encoded_ret, tmp3);
              have_profile_level = TRUE;
            }
          }

          if (!have_profile_level) {
            encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
          } else {
            gst_structure_free (tmp);
          }
        } else if (strcmp (type->mime, "video/x-vnd.on2.vp8") == 0) {
          tmp = gst_structure_new ("video/x-vp8",
              "width", GST_TYPE_INT_RANGE, 16, 4096,
              "height", GST_TYPE_INT_RANGE, 16, 4096,
              "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else if (strcmp (type->mime, "video/mpeg2") == 0) {
          tmp = gst_structure_new ("video/mpeg",
              "width", GST_TYPE_INT_RANGE, 16, 4096,
              "height", GST_TYPE_INT_RANGE, 16, 4096,
              "framerate", GST_TYPE_FRACTION_RANGE,
              0, 1, G_MAXINT, 1,
              "mpegversion", GST_TYPE_INT_RANGE, 1, 2,
              "systemstream", G_TYPE_BOOLEAN, FALSE,
              "parsed", G_TYPE_BOOLEAN, TRUE, NULL);

          encoded_ret = gst_caps_merge_structure (encoded_ret, tmp);
        } else {
          GST_WARNING ("Unsupported mimetype '%s'", type->mime);
        }
      }
    }
  }
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    androidmedia,
    "Android Media plugin",
    plugin_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
