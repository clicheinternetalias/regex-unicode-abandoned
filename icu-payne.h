/* ICU is a pain. I'm sure it's fine, if you're a Java or C++ programmer,
 * but the C API is lacking a C API.
 */
#ifndef UNI_PAYNE_H_
#define UNI_PAYNE_H_ 1

#include <unicode/uset.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/ustdio.h>
#include <stdint.h>

#ifndef BOOL_DEFINED
#define BOOL_DEFINED 1
typedef enum bool_e {
  false = 0,
  true = 1
} bool;
#endif

/* ********************************************************************** */
/* ********************************************************************** */

#define uni_is_valid(C) ((C) >= UCHAR_MIN_VALUE && (C) <= UCHAR_MAX_VALUE)

/* We're declaring string literals. Why is length a separate arg?
 */
#define UNI_STRING_DECL(V,S)  U_STRING_DECL(V, S, (sizeof(S)-1))
#define UNI_STRING_INIT(V,S)  U_STRING_INIT(V, S, (sizeof(S)-1))

/* ********************************************************************** */
/* ********************************************************************** */

typedef struct uni_iter_s {
  const UChar * startp;
  const UChar * curp;
  const UChar * endp;
} uni_iter;

extern void    uni_iter_init(uni_iter * iter, const UChar * s, size_t n);
extern UChar32 uni_iter_next(uni_iter * iter);
extern UChar32 uni_iter_prev(uni_iter * iter);
extern UChar32 uni_iter_peek(uni_iter * iter);
extern UChar32 uni_iter_rpeek(uni_iter * iter);

/* ********************************************************************** */
/* ********************************************************************** */

extern intmax_t uni_strtoimax(const UChar * strp, UChar ** endp, int base);
extern long int uni_strtol(const UChar * strp, UChar ** endp, int base);

extern UChar * u_strdup(const UChar * s);

/* ********************************************************************** */
/* ********************************************************************** */

extern USet * uni_set_open_left(void);
extern USet * uni_set_open_right(void);

extern bool uni_isopen(UChar32 c);
extern bool uni_isclose(UChar32 c);
extern bool uni_ismatch(UChar32 a, UChar32 b);
extern bool uni_quote_equal(const UChar * a, const UChar * b, size_t n);

/* ********************************************************************** */
/* ********************************************************************** */

#endif /* UNI_PAYNE_H_ */
