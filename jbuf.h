// SPDX-License-Identifier: GPL-3.0-only
/* jbuf.h - bounded appends for the JSON builders.
 *
 * The builders write into fixed buffers at a running position n. With plain
 * `n += snprintf(buf + n, size - n, ...)`, a text that does not fit still adds
 * its full length to n; the next call then gets `size - n` below zero, which
 * as a size_t is a huge number, and writes past the end of the buffer.
 *
 * jcat() and jclamp() keep n at most size - 1: a text that does not fit is cut
 * (the answer is then incomplete JSON, which the page treats as a failed read)
 * and every later append gets 1 byte of room, so nothing is written past the
 * buffer and no length larger than the buffer is ever sent.
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/* n limited to 0 .. size - 1 */
static inline int
jclamp(long long n, size_t size) {
  if(size == 0 || n < 0) return 0;
  return (unsigned long long)n >= size ? (int)(size - 1) : (int)n;
}

/* Appends like snprintf(buf + n, size - n, fmt, ...); returns the new n. */
static inline int jcat(char *buf, size_t size, long long n, const char *fmt, ...)
  __attribute__((format(printf, 4, 5)));
static inline int
jcat(char *buf, size_t size, long long n, const char *fmt, ...) {
  int at = jclamp(n, size);
  if(size == 0) return 0;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(buf + at, size - (size_t)at, fmt, ap);
  va_end(ap);
  return w < 0 ? at : jclamp((long long)at + w, size);
}
