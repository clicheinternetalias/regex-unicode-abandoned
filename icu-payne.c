/* ICU is a pain.
 */
#include "icu-payne.h"
#include <stdlib.h>

/* ********************************************************************** */
/* ********************************************************************** */

/* ICU's UCharIterator is useless.
 * How can someone make a C iterator without providing a pointer member?
 *   UChar * uiter_pointer(UCharIterator * iter);
 * I'd write it, but I don't feel like digging around inside of a C++ object.
 * (Note: I've since discovered utf16.h, but it still lacks ptrs)
 */
void
uni_iter_init(uni_iter * iter, const UChar * s, size_t n)
{
  iter->startp = s;
  iter->curp = s;
  iter->endp = s + n;
}

UChar32
uni_iter_next(uni_iter * iter)
{
  UChar32 c;
  UChar32 t;
  if (iter->curp >= iter->endp) return EOF;
  c = *iter->curp++;
  if (U16_IS_LEAD((unsigned)c) && iter->curp < iter->endp) {
    t = *iter->curp++;
    if (U16_IS_TRAIL((unsigned)t)) {
      c = U16_GET_SUPPLEMENTARY(c, t);
    } else {
      iter->curp--;
    }
  }
  return c;
}

UChar32
uni_iter_prev(uni_iter * iter)
{
  UChar32 c;
  UChar32 t;
  if (iter->curp <= iter->startp) return EOF;
  c = *--iter->curp;
  if (U16_IS_TRAIL((unsigned)c) && iter->curp > iter->startp) {
    t = *--iter->curp;
    if (U16_IS_LEAD((unsigned)t)) {
      c = U16_GET_SUPPLEMENTARY(t, c);
    } else {
      iter->curp++;
    }
  }
  return c;
}

UChar32
uni_iter_peek(uni_iter * iter)
{
  UChar32 c;
  UChar32 t;
  if (iter->curp >= iter->endp) return EOF;
  c = *iter->curp;
  if (U16_IS_LEAD((unsigned)c) && iter->curp + 1 < iter->endp) {
    t = *(iter->curp + 1);
    if (U16_IS_TRAIL((unsigned)t)) c = U16_GET_SUPPLEMENTARY(c, t);
  }
  return c;
}

UChar32
uni_iter_rpeek(uni_iter * iter)
{
  UChar32 c;
  UChar32 t;
  if (iter->curp <= iter->startp) return EOF;
  c = *(iter->curp - 1);
  if (U16_IS_TRAIL((unsigned)c) && iter->curp - 2 >= iter->startp) {
    t = *(iter->curp - 2);
    if (U16_IS_LEAD((unsigned)t)) c = U16_GET_SUPPLEMENTARY(t, c);
  }
  return c;
}

/* ********************************************************************** */
/* ********************************************************************** */

/* ICU lacks strtol. It can be faked using u_sscanf, but scanf's interface is
 * questionable at best. (So is strtol's, but at least it's a familiar mess.)
 *
 * The following is borrowed from libiberty, and modified a lot.
 */

/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. [rescinded 22 July 1999]
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <errno.h>
#include <limits.h>

#define NEXT() do{ \
  lastp = s; ch = *s++; \
  if (U16_IS_LEAD((unsigned)ch) && U16_IS_TRAIL((unsigned)*s)) { \
    ch = U16_GET_SUPPLEMENTARY(ch, *s); \
    s++; \
  } \
}while(0)

intmax_t
uni_strtoimax(const UChar * strp, UChar ** endp, int base)
{
  const UChar * lastp;
  const UChar * s = strp;
  UChar32 ch;
  intmax_t val = 0;
  intmax_t cutoff;
  int cutlim;
  int i;
  bool neg = false;
  bool sawdigit = false;
  bool overflow = false;

  do { NEXT(); } while (u_isUWhiteSpace(ch));
  if (ch == '-' || ch == '+') {
    neg = (ch == '-');
    NEXT();
  }
  if ((base == 0 || base == 16) && ch == '0' && (*s == 'x' || *s == 'X')) {
    ++s;
    NEXT();
    base = 16;
  }
  if (base == 0) {
    base = (u_digit(ch, 10) == 0) ? 8 : 10;
  }

  if (neg) {
    cutoff =      (INTMAX_MIN / base);
    cutlim = (int)(INTMAX_MIN % base);
  } else {
    cutoff =      (INTMAX_MAX / base);
    cutlim = (int)(INTMAX_MAX % base);
  }

  while ((i = u_digit(ch, (int8_t)base)) >= 0) {
    if (overflow || (neg ? (val < cutoff || (val == cutoff && -i < cutlim))
                         : (val > cutoff || (val == cutoff &&  i > cutlim)))) {
      overflow = true;
    } else {
      sawdigit = true;
      val *= base;
      if (neg) val -= i; else val += i;
    }
    NEXT();
  }
  if (overflow) {
    errno = ERANGE;
    val = neg ? INTMAX_MIN : INTMAX_MAX;
  }
  if (!sawdigit) errno = EINVAL;
  if (endp != NULL) *endp = (UChar*)(sawdigit ? lastp : strp);
  return val;
}

long int
uni_strtol(const UChar * strp, UChar ** endp, int base)
{
  intmax_t i = uni_strtoimax(strp, endp, base);
#if LONG_MAX < INTMAX_MAX
  if (i < LONG_MIN || i > LONG_MAX) {
    errno = ERANGE;
    return i < LONG_MIN ? LONG_MIN : LONG_MAX;
  }
#endif
  return (long int)i;
}

/* ********************************************************************** */
/* ********************************************************************** */

UChar *
u_strdup(const UChar * s)
{
  int32_t n = u_strlen(s);
  UChar * rv = malloc((size_t)(n + 1) * sizeof(UChar));
  if (rv) { u_memcpy(rv, s, n); rv[n] = '\0'; }
  return rv;
}

/* ********************************************************************** */
/* ********************************************************************** */

/* Unicode doesn't have a concept of mapping quotes/braces to
 * each other (besides some bidi stuff).
 *
 * Unfortunately, providing a (uni_map_get_match(map, val)) function
 * is harder than it sounds, since the mapping can be one-to-many.
 */

typedef struct uni_map_s {
  struct uni_map_entry_s {
    UChar32 from;
    UChar32 to;
  } * buf;
  size_t len;
} uni_map;

#include "braces.c.inc"

static USet *
uni_set_open(UChar32 * s, size_t n)
{
  USet * rv = uset_openEmpty();
  if (rv) {
    size_t i;
    for (i = 0; i < n; ++i) uset_add(rv, s[i]);
  }
  return rv;
}

USet *
uni_set_open_left(void)
{
  return uni_set_open(uni_set_left, uni_set_left_length);
}

USet *
uni_set_open_right(void)
{
  return uni_set_open(uni_set_right, uni_set_right_length);
}

static bool
uni_set_contains(const UChar32 * m, size_t mlen, UChar32 val)
{
  size_t lo = 0;
  size_t hi = mlen - 1;
  while (lo <= hi) {
    size_t mid = lo + ((hi - lo) / 2);
    if      (m[mid] > val) hi = mid - 1;
    else if (m[mid] < val) lo = mid + 1;
    else return true;
  }
  return false;
}

static bool
uni_map_contains(const uni_map * m, UChar32 val)
{
  size_t lo = 0;
  size_t hi = m->len - 1;
  while (lo <= hi) {
    size_t mid = lo + ((hi - lo) / 2);
    if      (m->buf[mid] > val) hi = mid - 1;
    else if (m->buf[mid] < val) lo = mid + 1;
    else return true;
  }
  return false;
}

static bool
uni_map_ismapped(const uni_map * m, UChar32 from, UChar32 to)
{
  size_t lo = 0;
  size_t hi = m->len - 1;
  while (lo <= hi) {
    size_t mid = lo + ((hi - lo) / 2);
    if      (from < m->buf[mid].from) hi = mid - 1;
    else if (from > m->buf[mid].from) lo = mid + 1;
    else if (to   < m->buf[mid].to  ) hi = mid - 1;
    else if (to   > m->buf[mid].to  ) lo = mid + 1;
    else return true;
  }
  return false;
}

bool
uni_isopen(UChar32 c)
{
  return uni_set_contains(uni_set_left, uni_set_left_length, c);
}

bool
uni_isclose(UChar32 c)
{
  return uni_set_contains(uni_set_right, uni_set_right_length, c);
}

bool
uni_ismatch(UChar32 a, UChar32 b)
{
  return uni_map_ismapped(&uni_map_braces, a, b);
}

bool
uni_quote_equal(const UChar * a, const UChar * b, size_t n)
{
  size_t i;
  for (i = 0; i < n; ++i) {
    if (a[i] == b[i] ? uni_map_contains(&uni_map_braces, a[i]) /* "("   "("  */
                     : !uni_ismatch(a[i], b[i]))               /* "(" [^")"] */
      return false;
  }
  return true;
}

/* ********************************************************************** */
/* ********************************************************************** */
