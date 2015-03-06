/* ********************************************************************** */
/* Basic Markup Language Tokenizer */
/* (not quite XML, but similar) */
/* ********************************************************************** */

#ifndef BML_H_
#define BML_H_ 1

#include <stdio.h>

/* Only tags and attributes are returned.
 * Content is handled by a separate function.
 * FIXME: No entity support for attribute values.
 */
typedef enum bml_token_e {
  BML_ERROR = -1,
  BML_EOF = 0,
  BML_OPEN,     /* <foo */
  BML_ATTR,     /* name */
  BML_VALUE,    /* ='value' ="value" =value */
  BML_CHILDREN, /* > */
  BML_EMPTY,    /* /> */
  BML_CLOSE,    /* </foo> */
  BML_CONTENT   /* ... */
} bml_token;

extern bml_token bml_next(FILE * fp, char * tokbuf, size_t toklen);
extern bml_token bml_content(FILE * fp, char * tokbuf, size_t toklen);

/* ********************************************************************** */
/* ********************************************************************** */

#endif /* BML_H_ */
