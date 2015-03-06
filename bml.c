/* Basic Markup Language Tokenizer
 * (not quite XML, but similar)
 */
#include "bml.h"
#include <string.h>

#define ISDELIM(c_)  ((c_) <= ' ' || strchr("<=>/\"'", (c_)))
#define ISSPACE(c_)  ((c_) <= ' ')

#define MORE  (ch != EOF)
#define CUR   (ch)
#define NEXT  (ch = getc(fp))
#define PREV  (ungetc(ch, fp))

#define READ_TOKEN(STOP) do{ \
  size_t i_ = 0; \
  do { \
    if (STOP) break; \
    tokbuf[i_++] = (char)CUR; \
    if (i_ >= toklen) return BML_ERROR; \
    NEXT; \
  } while (MORE); \
  tokbuf[i_] = '\0'; \
}while(0)

bml_token
bml_content(FILE * fp, char * tokbuf, size_t toklen)
{
  int ch;
  NEXT;
  if (MORE) {
    if (tokbuf) { READ_TOKEN(CUR == '<'); }
    else { while (MORE) { if (CUR == '<') break; NEXT; } }
    if (MORE) { PREV; return BML_CONTENT; }
  }
  return BML_EOF;
}

bml_token
bml_next(FILE * fp, char * tokbuf, size_t toklen)
{
  int ch;
  NEXT;
  while (MORE) {
    if (ISSPACE(CUR)) { NEXT; continue; }
    if (CUR == '<') { NEXT; if (!MORE) return BML_ERROR;

      /* <!--...--> <!...> */
      if (CUR == '!') { NEXT;
        if (MORE && CUR == '-') { NEXT;
          if (MORE && CUR == '-') { /* <!--...--> */
            char buf[4] = "###"; NEXT;
            while (MORE) {
              memmove(buf, buf + 1, (4 - 1) * sizeof(char));
              buf[2] = (char)CUR;
              if (!memcmp(buf, "-->", 4 * sizeof(char))) break;
              NEXT;
            }
          }
        }
        while (MORE && CUR != '>') { NEXT; } /* <!...> */
        if (MORE) { NEXT; continue; } else return BML_ERROR;
      }
      /* <?...?> */
      if (CUR == '?') {
        int t = '#'; NEXT;
        while (MORE && (t != '?' || CUR != '>')) { t = CUR; NEXT; }
        if (MORE) { NEXT; continue; } else return BML_ERROR;
      }
      /* </name...> */
      if (CUR == '/') { NEXT;
        READ_TOKEN(ISDELIM(CUR));
        while (MORE && ISSPACE(CUR)) { NEXT; }
        return (MORE && CUR == '>') ? BML_CLOSE : BML_ERROR;
      }
      /* <name */
      READ_TOKEN(ISDELIM(CUR)); PREV;
      return BML_OPEN;
    }
    if (CUR == '>') { return BML_CHILDREN; } /* > */
    if (CUR == '/') { NEXT;
      return (MORE && CUR == '>') ? BML_EMPTY : BML_ERROR; /* /> */
    }
    if (CUR == '=') {
      do { NEXT; } while (MORE && ISSPACE(CUR));
      if (CUR == '\'' || CUR == '\"') { /* ='string' ="string" */
        int qu = CUR; NEXT;
        READ_TOKEN(CUR == qu);
      } else {
        READ_TOKEN(ISDELIM(CUR)); PREV; /* =word */
      }
      return BML_VALUE;
    }
    if (CUR == '\'' || CUR == '\"') { return BML_ERROR; }
    READ_TOKEN(ISDELIM(CUR)); PREV; /* attr */
    return BML_ATTR;
  }
  return BML_EOF;
}

/* ********************************************************************** */
/* ********************************************************************** */
