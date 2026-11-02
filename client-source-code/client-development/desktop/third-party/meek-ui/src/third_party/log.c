/*
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "log.h"
#include "../host_services.h"
#include "../clib/stdlib.h"

#define MAX_CALLBACKS 32

typedef struct {
  log_LogFn fn;
  void *udata;
  int level;
} Callback;

static struct {
  void *udata;
  log_LockFn lock;
  int level;
  bool quiet;
  Callback callbacks[MAX_CALLBACKS];
} L;


static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};
#endif


/*
 * Android: route every log call through __android_log_print so lines
 * reach logcat. Our `fprintf(stderr, ...)` was going to /dev/null on
 * Android (stderr isn't attached to anything for NativeActivity apps),
 * so none of log_info/log_warn/log_error was visible. With this, the
 * lines appear under the tag baked in by APPNAME ("guidemo" by default,
 * set from the build-time -include header) and can be filtered in
 * Android Studio's Logcat via `tag:guidemo` or `package:mine`.
 */
#if MEEK_UI_HOST_SERVICES_FALLBACK
// the stdio sinks below exist only in a build that keeps the C runtime
// fallback; with MEEK_UI_HOST_SERVICES_FALLBACK=0 the host's write_log is
// the only sink, and fprintf, fputc, fflush and strftime leave the object

#if defined(__ANDROID__)
#include <android/log.h>
#ifndef APPNAME
#define APPNAME "guiapp"
#endif
static void stdout_callback(log_Event *ev) {
  int prio;
  switch (ev->level) {
    case LOG_TRACE: prio = ANDROID_LOG_VERBOSE; break;
    case LOG_DEBUG: prio = ANDROID_LOG_DEBUG;   break;
    case LOG_INFO:  prio = ANDROID_LOG_INFO;    break;
    case LOG_WARN:  prio = ANDROID_LOG_WARN;    break;
    case LOG_ERROR: prio = ANDROID_LOG_ERROR;   break;
    case LOG_FATAL: prio = ANDROID_LOG_FATAL;   break;
    default:        prio = ANDROID_LOG_INFO;
  }
  char body[1024];
  vsnprintf(body, sizeof(body), ev->fmt, ev->ap);
  /* strip directory prefix from ev->file to keep the logcat line short */
  const char *basename = ev->file;
  for (const char *p = ev->file; *p; p++) {
    if (*p == '/' || *p == '\\') basename = p + 1;
  }
  __android_log_print(prio, APPNAME, "%s:%d: %s", basename, ev->line, body);
}
#else
static void stdout_callback(log_Event *ev) {
  char buf[16];
  buf[strftime(buf, sizeof(buf), "%H:%M:%S", ev->time)] = '\0';
#ifdef LOG_USE_COLOR
  fprintf(
    ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
    buf, level_colors[ev->level], level_strings[ev->level],
    ev->file, ev->line);
#else
  fprintf(
    ev->udata, "%s %-5s %s:%d: ",
    buf, level_strings[ev->level], ev->file, ev->line);
#endif
  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}
#endif


static void file_callback(log_Event *ev) {
  char buf[64];
  buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
  fprintf(
    ev->udata, "%s %-5s %s:%d: ",
    buf, level_strings[ev->level], ev->file, ev->line);
  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}


#endif // MEEK_UI_HOST_SERVICES_FALLBACK

static void lock(void)   {
  if (L.lock) { L.lock(true, L.udata); }
}


static void unlock(void) {
  if (L.lock) { L.lock(false, L.udata); }
}


const char* log_level_string(int level) {
  return level_strings[level];
}


void log_set_lock(log_LockFn fn, void *udata) {
  L.lock = fn;
  L.udata = udata;
}


void log_set_level(int level) {
  L.level = level;
}


void log_set_quiet(bool enable) {
  L.quiet = enable;
}


int log_add_callback(log_LogFn fn, void *udata, int level) {
  for (int i = 0; i < MAX_CALLBACKS; i++) {
    if (!L.callbacks[i].fn) {
      L.callbacks[i] = (Callback) { fn, udata, level };
      return 0;
    }
  }
  return -1;
}


int log_add_fp(FILE *fp, int level) {
#if MEEK_UI_HOST_SERVICES_FALLBACK
  return log_add_callback(file_callback, fp, level);
#else
  // no stdio sink in a freestanding build: the host's write_log is it
  (void)fp;
  (void)level;
  return -1;
#endif
}


static void init_event(log_Event *ev, void *udata) {
#if MEEK_UI_HOST_SERVICES_FALLBACK
  if (!ev->time) {
    time_t t = time(NULL);
    ev->time = localtime(&t);
  }
#endif
  ev->udata = udata;
}


void log_log(int level, const char *file, int line, const char *fmt, ...) {
  log_Event ev = {
    .fmt   = fmt,
    .file  = file,
    .line  = line,
    .level = level,
  };

  lock();

  // A host that installed meek_host_services_t owns the log: the line is
  // formatted here and handed over, and no stdio is touched. This is what
  // lets a static build link without a C runtime.
  {
    const meek_host_services_t *services = host_services__get();

    if (services->write_log != NULL_POINTER) {
      char message[1024];
      va_list args;

      message[0] = 0;
      va_start(args, fmt);
      stdlib__vformat(message, (int64)sizeof(message), fmt, args);
      va_end(args);
      services->write_log((meek_log_level_e)level, file, line, message);
      unlock();
      return;
    }
  }

#if MEEK_UI_HOST_SERVICES_FALLBACK
  if (!L.quiet && level >= L.level) {
    init_event(&ev, stderr);
    va_start(ev.ap, fmt);
    stdout_callback(&ev);
    va_end(ev.ap);
  }
#endif

#if MEEK_UI_HOST_SERVICES_FALLBACK
  for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++) {
    Callback *cb = &L.callbacks[i];
    if (level >= cb->level) {
      init_event(&ev, cb->udata);
      va_start(ev.ap, fmt);
      cb->fn(&ev);
      va_end(ev.ap);
    }
  }
#endif

  unlock();
}
