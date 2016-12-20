/*
 * Level 2 Unicode support requires being able to match substrings
 * as single chars (case folding, clusters, graphemes, etc).
 * We can do that, but ICU doesn't provide workable solutions.
 *
 * \X
 *   isn't a set match; it's a sub-expression
 * \q{ch}
 *   can't support without a set prefix-matching function
 * (?+i)(?-i){case-ignore}
 *   won't support without proper case folding
 * \N{name}
 *   no sense supporting this if we can't \N{MixCapsCaseAndSpace}
 *   we have \p{name=EXACT} and {name=EXACT}
 *
 * NOTE: Input should be normalized to NFD before matching.
 */

/* ********************************************************************** */
/* ********************************************************************** */

#include "icu-payne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#define Q(EX)  do{ rgx_error e_ = (EX); if (e_) return e_; }while(0)
#define QN(EX) do{ if ((EX) == NULL) return RGX_MEMORY; }while(0)

/* ********************************************************************** */
/* ********************************************************************** */

typedef enum rgx_error_e {
  RGX_OK = 0,
  RGX_MEMORY,          /*  1  malloc failed */
  RGX_TOO_LONG,        /*  2  regex would compile into too many opcodes */
  RGX_OVERFLOW,        /*  3  {n,m} integer overflow */
  RGX_BAD_REPEAT,      /*  4  {n,m} n > m */
  RGX_BAD_SET,         /*  5  malformed [] */
  RGX_BAD_DIRECTIVE,   /*  6  malformed {} */
  RGX_MISSING_BRACE,   /*  7  missing } after { */
  RGX_BAD_GROUP,       /*  8  unexpected after (? */
  RGX_MISSING_PAREN,   /*  9  missing ) after ( */
  RGX_BAD_ESCAPE,      /* 10  malformed \escape sequence */
  RGX_MISSING_BRACKET, /* 11  missing ] after [ */
  RGX_BAD_NAME,        /* 12  name too long or missing terminator */
  RGX_UNDEFINED,       /* 13  name is referenced but not defined */
  RGX_REDEFINED,       /* 14  procedure has multiple definitions */
  RGX_EXTRA_JUNK       /* 15  expr<rep><rep> */
} rgx_error;

#define FF(I) do{ fprintf(stdout, "%d\n", (I)); fflush(stdout); }while(0)

/* ********************************************************************** */
/* ********************************************************************** */

static char fmtbuf0[128];
static char fmtbuf1[128];
static char fmtbuf2[128];
static char *
ustr0(const UChar * s)
{
  UErrorCode uec = U_ZERO_ERROR;
  return u_strToUTF8(fmtbuf0, sizeof(fmtbuf0), NULL, s, -1, &uec);
}
static char *
ustr1(const UChar * s)
{
  UErrorCode uec = U_ZERO_ERROR;
  return u_strToUTF8(fmtbuf1, sizeof(fmtbuf1), NULL, s, -1, &uec);
}
static char *
ustr2(const UChar * s)
{
  UErrorCode uec = U_ZERO_ERROR;
  return u_strToUTF8(fmtbuf2, sizeof(fmtbuf2), NULL, s, -1, &uec);
}

/* ********************************************************************** */
/* ********************************************************************** */

/* Ranges are implemented as unrolled concatenation.
 *   That means the regex /((((a){255}){255}){255}){255}/
 *   compiles into 1*255*255*255*255 (0xFC05FC01) opcodes.
 *   Obviously, large allocations are possible and must be avoided.
 *   Let's impose some limits.
 *
 * RGX_LEN_MAX:
 *   AST node allocation is derived from the pattern's string length.
 * RGX_CODE_MAX:
 *   Compiled opcode buffer.
 */
#define RGX_REP_MAX   (65535)      /* big enough, but prevents integer overflow */
#define RGX_LEN_MAX   (64*1024)    /* larger than most text files (*ahem* Notepad) */
#define RGX_CODE_MAX  (1024*1024)  /* 1M * sizeof(rgx_code)B == much MB */

typedef enum rgx_tree_type_e {
  TREE_CHAR,    /* a */
  TREE_SET,     /* [] */
  TREE_ANY,     /* . {any} */
  TREE_NONE,    /*   {^any} */
  TREE_BOL,     /* ^ {line-start} */
  TREE_NBOL,    /*   {^line-start} */
  TREE_EOL,     /* $ {line-end} */
  TREE_NEOL,    /*   {^line-end} */
  TREE_BOT,     /* \A {input-start} */
  TREE_NBOT,    /*    {^input-start} */
  TREE_EOT,     /* \Z {input-end} */
  TREE_NEOT,    /*    {^input-end} */
  TREE_WBND,    /* \b {word-break} */
  TREE_NWBND,   /* \B {^word-break} */
  TREE_LOOKA,   /* (?=a) */
  TREE_NLOOKA,  /* (?!a) */
  TREE_LOOKB,   /* (?<=a) */
  TREE_NLOOKB,  /* (?<!a) */
  TREE_ALT,     /* a|b */
  TREE_CAT,     /* ab */
  TREE_GROUP,   /* (?name:a) */
  TREE_QUEST,   /* a? */
  TREE_PLUS,    /* a+ */
  TREE_STAR,    /* a* */
  TREE_REPEAT,  /* a{b,c} */
  TREE_BREF,    /* \kname; {ref name} */
  TREE_NBREF,   /* \Kname; {^ref name} */
  TREE_QREF,    /* \mname; {ref-braced name} */
  TREE_NQREF,   /* \Mname; {^ref-braced name} */
  TREE_PROC,    /* \gname; {call name} */
  TREE_NPROC,   /* \Gname; {^call name} */
  TREE_COND,    /* (??ctf) */
} rgx_tree_type;

#define index_t  int  /* avoid size_t, keep opcodes small */

typedef struct rgx_tree_s rgx_tree;
struct rgx_tree_s {
  rgx_tree_type type;
  rgx_tree * left;
  union {
    rgx_tree * xright;  /* TREE_ALT, TREE_CAT */
    index_t xindex;     /* TREE_GROUP, TREE_BREF, TREE_NBREF, TREE_PROC, TREE_NPROC, TREE_COND */
    UChar32 cvalue;     /* TREE_CHAR */
    USet * xset;        /* TREE_SET */
    struct {            /* TREE_REPEAT, TREE_QUEST, TREE_PLUS, TREE_STAR */
      int min;
      int max;
      bool greedy;
    } rep;
  } u;
};
#define right      u.xright
#define capindex   u.xindex
#define procindex  u.xindex
#define repmin     u.rep.min
#define repmax     u.rep.max
#define repgreedy  u.rep.greedy
#define chval      u.cvalue
#define chset      u.xset

/* ********************************************************************** */
/* ********************************************************************** */

struct tokenizer_s {
  /* the input pattern */
  uni_iter iter;
  UChar32 cur;

  /* the tree nodes */
  rgx_tree * nodes;
  size_t nodesidx;
  size_t nodeslen;

  /* sub-match and back-reference names */
  struct backref_s {
    UChar * name;
    bool defined; /* true if (?foo:) seen, false if only \kfoo; */
  } * refs;
  size_t refslen;

  /* named procedures */
  struct procref_s {
    UChar * name;
    rgx_tree * body; /* non-null if (?/foo:) seen, null if only \gfoo; */
    void * locfwd; /* rgx_code; compiled location */
    void * locrev; /* rgx_code; compiled location */
  } * procs;
  size_t procslen;
};

static rgx_error
lookup_group(struct tokenizer_s * tk, UChar * name, bool isdef, index_t * idx)
{
  size_t i;
  for (i = 0; i < tk->refslen; ++i) {
    if (!u_strCompare(tk->refs[i].name, -1, name, -1, true)) {
      tk->refs[i].defined |= isdef;
      if (idx) *idx = (index_t)i;
      return RGX_OK;
    }
  }
  QN(tk->refs = realloc(tk->refs, (tk->refslen + 1) * sizeof(struct backref_s)));
  QN(tk->refs[tk->refslen].name = u_strdup(name));
  tk->refs[tk->refslen].defined = isdef;
  if (idx) *idx = (index_t)tk->refslen;
  tk->refslen++;
  return RGX_OK;
}

static rgx_error
lookup_proc(struct tokenizer_s * tk, UChar * name, rgx_tree * body, index_t * idx)
{
  size_t i;
  for (i = 0; i < tk->procslen; ++i) {
    if (!u_strCompare(tk->procs[i].name, -1, name, -1, true)) {
      if (body) {
        if (tk->procs[i].body != NULL) return RGX_REDEFINED;
        tk->procs[i].body = body;
      }
      if (idx) *idx = (index_t)i;
      return RGX_OK;
    }
  }
  QN(tk->procs = realloc(tk->procs, (tk->procslen + 1) * sizeof(struct procref_s)));
  QN(tk->procs[tk->procslen].name = u_strdup(name));
  tk->procs[tk->procslen].body = body;
  if (idx) *idx = (index_t)tk->procslen;
  tk->procslen++;
  return RGX_OK;
}

#define tree_new(tk,type)        tree_new2((tk),(type),NULL,NULL)
#define tree_new1(tk,type,child) tree_new2((tk),(type),(child),NULL)
static rgx_tree *
tree_new2(struct tokenizer_s * tk, rgx_tree_type type,
          rgx_tree * rl, rgx_tree * rr)
{
  rgx_tree * re = tk->nodes + tk->nodesidx++;
  assert(tk->nodesidx <= tk->nodeslen);
  re->type = type;
  re->left = rl;
  re->right = rr;
  return re;
}

static bool ucat_init = false;
static USet * ucat_digit;
static USet * ucat_word;
static USet * ucat_space;
static USet * ucat_vspace;
static USet * ucat_hspace;
static USet * ucat_open;
static USet * ucat_close;

static rgx_error
init_charsets(void)
{
  UErrorCode uec = U_ZERO_ERROR;
  UChar buf[64];
#define MAKE_PAT(DST,PAT) do{ \
  u_charsToUChars((PAT), buf, sizeof(PAT)); \
  QN((DST) = uset_openPattern(buf, -1, &uec)); \
  if (U_FAILURE(uec)) return RGX_MEMORY; \
}while(0)
  MAKE_PAT(ucat_digit,  "\\p{nd}");
  MAKE_PAT(ucat_word,   "[\\p{alpha}\\p{m}\\p{n}\\p{pc}\\p{joinc}]");
  MAKE_PAT(ucat_space,  "\\p{whitespace}");
  MAKE_PAT(ucat_vspace, "[\\n\\v\\f\\r\\x85\\u2028\\u2029]");
  MAKE_PAT(ucat_hspace, "[\\t\\p{zs}]");
  QN(ucat_open = uni_set_open_left());
  QN(ucat_close = uni_set_open_right());
  ucat_init = true;
  return RGX_OK;
#undef MAKE_PAT
}

/* ********************************************************************** */
/* ********************************************************************** */

#define MORE   (tk->cur != EOF)
#define CUR    (tk->cur)
#define NEXT   (tk->cur = uni_iter_next(&tk->iter))
#define PEEK   (uni_iter_peek(&tk->iter))

static rgx_error parse_alt(struct tokenizer_s * tk, rgx_tree ** re);
static rgx_error parse_concat(struct tokenizer_s * tk, rgx_tree ** re);
static rgx_error parse_single(struct tokenizer_s * tk, rgx_tree ** re);

static bool
skip_spaces(struct tokenizer_s * tk)
{
  while (MORE && (CUR == '#' || uset_contains(ucat_space, CUR))) {
    if (CUR == '#') {
      do { NEXT; } while (MORE && !uset_contains(ucat_vspace, CUR));
    }
    NEXT;
  }
  return MORE;
}

static rgx_error
gather(struct tokenizer_s * tk, UChar * buf, size_t buflen, UChar32 endc, rgx_error err)
{
  size_t i = 0;
  while (CUR != endc) {
    buf[i++] = (UChar)CUR;
    if (i >= buflen) return err;
    NEXT; if (!MORE) return err;
  }
  NEXT; /* skip the end char */
  buf[i] = '\0';
  return RGX_OK;
}

static rgx_error
gather_word(struct tokenizer_s * tk, UChar * buf, size_t buflen, rgx_error err)
{
  size_t i = 0;
  if (!skip_spaces(tk)) return err;
  while (!uset_contains(ucat_space, CUR) && CUR != '}') {
    buf[i++] = (UChar)CUR;
    if (i >= buflen) return err;
    if (CUR == '$' || CUR == ':' || CUR == '=') { NEXT; break; }
    NEXT; if (!MORE) return err;
  }
  buf[i] = '\0';
  return RGX_OK;
}

static rgx_error
escape_hex(struct tokenizer_s * tk, rgx_tree * re)
{
  UChar * endp = (UChar *)tk->iter.curp;
  long int val;
  val = uni_strtol(tk->iter.curp, &endp, 16);
  if (!uni_is_valid(val) || endp == tk->iter.curp || *endp != ';') return RGX_BAD_ESCAPE;
  re->type = TREE_CHAR;
  re->chval = (UChar32)val;
  tk->iter.curp = endp + 1; NEXT;
  return RGX_OK;
}

static rgx_error
escape_property(struct tokenizer_s * tk, rgx_tree * re)
{
  UErrorCode uec = U_ZERO_ERROR;
  const UChar * start = tk->iter.curp - 2; /* include the backslash */
  while (CUR != '}') { NEXT; if (!MORE) return RGX_BAD_ESCAPE; }
  re->type = TREE_SET;
  re->chset = uset_openPattern(start, (int32_t)(tk->iter.curp - start), &uec);
  NEXT; /* '}' */
  return (U_FAILURE(uec)) ? RGX_BAD_ESCAPE : RGX_OK;
}

static rgx_error
escape_backref(struct tokenizer_s * tk, rgx_tree * re)
{
  UChar buf[128];
  re->type = (CUR == 'k') ? TREE_BREF
           : (CUR == 'm') ? TREE_QREF
           : (CUR == 'M') ? TREE_NQREF : TREE_NBREF;
  NEXT; if (!MORE) return RGX_BAD_ESCAPE;
  Q(gather(tk, buf, 128, ';', RGX_BAD_ESCAPE));
  return lookup_group(tk, buf, false, &re->capindex);
}

static rgx_error
escape_procref(struct tokenizer_s * tk, rgx_tree * re)
{
  UChar buf[128];
  re->type = (CUR == 'g') ? TREE_PROC : TREE_NPROC;
  NEXT; if (!MORE) return RGX_BAD_ESCAPE;
  Q(gather(tk, buf, 128, ';', RGX_BAD_ESCAPE));
  return lookup_proc(tk, buf, NULL, &re->procindex);
}

/* does not allocate return node, in case it's called from within a set */
static rgx_error
parse_escape(struct tokenizer_s * tk, rgx_tree * re)
{
#define ESC_CHAR(C) do{ re->type = TREE_CHAR; re->chval = (C); NEXT; }while(0)
#define ESC_SET(CAT) do{ re->type = TREE_SET; re->chset = (CAT); NEXT; }while(0)
  bool isneg = false;
  if (!MORE) return RGX_BAD_ESCAPE;
  switch (CUR) {
    case 'x': return escape_hex(tk, re);
    case 'K': case 'k': return escape_backref(tk, re);
    case 'M': case 'm': return escape_backref(tk, re);
    case 'G': case 'g': return escape_procref(tk, re);
    case 'P': case 'p': return escape_property(tk, re);
    case 'D': isneg = true; case 'd': ESC_SET(ucat_digit); break;
    case 'W': isneg = true; case 'w': ESC_SET(ucat_word); break;
    case 'S': isneg = true; case 's': ESC_SET(ucat_space); break;
    case 'V': isneg = true; case 'v': ESC_SET(ucat_vspace); break;
    case 'H': isneg = true; case 'h': ESC_SET(ucat_hspace); break;
    case 'O': isneg = true; case 'o': ESC_SET(ucat_open); break;
    case 'C': isneg = true; case 'c': ESC_SET(ucat_close); break;
    case 'r': ESC_CHAR('\r'); break;
    case 'n': ESC_CHAR('\n'); break;
    case 't': ESC_CHAR('\t'); break;
    case 'a': re->type = TREE_BOT;   NEXT; break;
    case 'A': re->type = TREE_NBOT;  NEXT; break;
    case 'z': re->type = TREE_EOT;   NEXT; break;
    case 'Z': re->type = TREE_NEOT;  NEXT; break;
    case 'b': re->type = TREE_WBND;  NEXT; break;
    case 'B': re->type = TREE_NWBND; NEXT; break;
    default:  ESC_CHAR(CUR); break;
  }
  if (isneg) {
    QN(re->chset = uset_clone(re->chset));
    uset_complement(re->chset);
  }
  return RGX_OK;
#undef ESC_CHAR
#undef ESC_SET
}

#define DIRECTIVE(V,S,E) UNI_STRING_DECL(V,S);
#include "directives.inc"

static void
init_directives(void)
{
  static bool dir_init = false;
  if (!dir_init) {
#define DIRECTIVE(V,S,E) UNI_STRING_INIT(V,S);
#include "directives.inc"
    dir_init = true;
  }
}

static UChar * directives[] = {
#define DIRECTIVE(V,S,E) V,
#include "directives.inc"
};
#define directives_length  (sizeof(directives) / sizeof(directives[0]))

typedef enum rgx_directive_type_e {
#define DIRECTIVE(V,S,E) E,
#include "directives.inc"
  D_NONE_
} rgx_directive_type;

static rgx_directive_type
find_directive(const UChar * buf)
{
  int lo = 0;
  int hi = directives_length - 1;
  while (lo <= hi) {
    int mid = lo + ((hi - lo) / 2);
    int c = u_strcmp(directives[mid], buf);
    if      (c > 0) hi = mid - 1;
    else if (c < 0) lo = mid + 1;
    else return (rgx_directive_type)mid;
  }
  return D_NONE_;
}

static rgx_error
parse_directive(struct tokenizer_s * tk, rgx_tree * re)
{
#define ESC_SET(CAT) do{ \
  re->type = TREE_SET; \
  re->chset = (CAT); \
  if (isneg) { \
    QN(re->chset = uset_clone(re->chset)); \
    uset_complement(re->chset); \
  } \
}while(0)
#define ESC_TYPE(T) do{ \
  re->type = (T); \
}while(0)
  bool isneg = false;
  rgx_directive_type d;
  UChar buf[128];
  const UChar * savp;
  UChar32 savc;

  init_directives();

  if (!MORE) return RGX_BAD_DIRECTIVE;
  if (CUR == '^') { NEXT; isneg = true; }

  savp = tk->iter.curp;
  savc = tk->cur;
  Q(gather_word(tk, buf, 128, RGX_BAD_DIRECTIVE));
  d = find_directive(buf);
  if (d == D_NONE_) { d = D_PROP; tk->iter.curp = savp; tk->cur = savc; }

  switch (d) {
    case D_NONE_:       return RGX_BAD_DIRECTIVE;
    case D_LINE_START:  ESC_TYPE(isneg ? TREE_NBOL : TREE_BOL); break;
    case D_LINE_END:    ESC_TYPE(isneg ? TREE_NEOL : TREE_EOL); break;
    case D_INPUT_START: ESC_TYPE(isneg ? TREE_NBOT : TREE_BOT); break;
    case D_INPUT_END:   ESC_TYPE(isneg ? TREE_NEOT : TREE_EOT); break;
    case D_WORD_BREAK:  ESC_TYPE(isneg ? TREE_NWBND : TREE_WBND); break;
    case D_ANY:         ESC_TYPE(isneg ? TREE_NONE : TREE_ANY); break;
    case D_DIGIT:       ESC_SET(ucat_digit);  break;
    case D_WORD:        ESC_SET(ucat_word);   break;
    case D_SPACE:       ESC_SET(ucat_space);  break;
    case D_VSPACE:      ESC_SET(ucat_vspace); break;
    case D_HSPACE:      ESC_SET(ucat_hspace); break;
    case D_OPEN_BRACE:  ESC_SET(ucat_open);   break;
    case D_CLOSE_BRACE: ESC_SET(ucat_close);  break;
    case D_EQUAL:
    case D_REF: {
      ESC_TYPE(isneg ? TREE_NBREF : TREE_BREF);
      Q(gather_word(tk, buf, 128, RGX_BAD_DIRECTIVE));
      Q(lookup_group(tk, buf, false, &re->capindex));
      break;
    }
    case D_COLON:
    case D_REF_BRACED: {
      ESC_TYPE(isneg ? TREE_NQREF : TREE_QREF);
      Q(gather_word(tk, buf, 128, RGX_BAD_DIRECTIVE));
      Q(lookup_group(tk, buf, false, &re->capindex));
      break;
    }
    case D_SLASH:
    case D_CALL: {
      ESC_TYPE(isneg ? TREE_NPROC : TREE_PROC);
      Q(gather_word(tk, buf, 128, RGX_BAD_DIRECTIVE));
      Q(lookup_proc(tk, buf, false, &re->procindex));
      break;
    }
    case D_PROP: {
      UErrorCode uec = U_ZERO_ERROR;
      int i = 0;
      buf[i++] = '\\';
      buf[i++] = isneg ? 'P' : 'p';
      buf[i++] = '{';
      for (;;) {
        buf[i] = (UChar)CUR;
        if (++i >= 128) return RGX_BAD_DIRECTIVE;
        if (CUR == '}') break;
        NEXT; if (!MORE) return RGX_BAD_DIRECTIVE;
      }
      buf[i] = '\0';
      ESC_TYPE(TREE_SET);
      re->chset = uset_openPattern(buf, i, &uec);
      if (U_FAILURE(uec)) return RGX_BAD_DIRECTIVE;
      break;
    }
  }
  if (skip_spaces(tk) && CUR == '}') { NEXT; } else return RGX_MISSING_BRACE;
  return RGX_OK;
#undef ESC_SET
#undef ESC_TYPE
}

static rgx_error
parse_setchar(struct tokenizer_s * tk, rgx_tree * re, bool * found)
{
  *found = false;
  if (skip_spaces(tk)) {
    if (CUR == '\\') { NEXT;
      *found = true;
      return parse_escape(tk, re);
    }
    if (CUR == '{') { NEXT;
      *found = true;
      return parse_directive(tk, re);
    }
    if (!CUR || CUR >= 0x80 || !strchr("[]-&~", CUR)) {
      *found = true;
      re->type = TREE_CHAR;
      re->chval = CUR;
      NEXT;
    }
  }
  return RGX_OK;
}

static rgx_error
parse_bracket(struct tokenizer_s * tk, rgx_tree ** re)
{
  rgx_tree * rl = tree_new(tk, TREE_SET);
  rgx_tree tmp;
  rgx_tree * rt = &tmp; /* rt might be moved to another node; no worries */
  bool neg = false;
  bool found = false;
  UChar32 min = EOF;
  UChar32 op = EOF;

  QN(rl->chset = uset_openEmpty());

  if (skip_spaces(tk) && CUR == '^') { NEXT; neg = true; }

  for (;;) {
    Q(parse_setchar(tk, rt, &found));
    if (found) {
      if (rt->type == TREE_CHAR) {
        uset_add(rl->chset, rt->chval);
        min = rt->chval;
        continue;
      }
      if (rt->type == TREE_SET) {
        uset_addAll(rl->chset, rt->chset);
        min = EOF;
        continue;
      }
      return RGX_BAD_SET;
    }
    if (!MORE || CUR == ']') break;

    /* here: (CUR == '&' || CUR == '~' || CUR == '-' || CUR == '[') */

    if (CUR == '[') { op = '|'; found = false; }
    else            { op = CUR; NEXT; Q(parse_setchar(tk, rt, &found)); }
    if (!found) {
      if (MORE && CUR == '[') { NEXT; Q(parse_bracket(tk, &rt)); }
      else return RGX_BAD_SET;
    }

    if (rt->type == TREE_CHAR) { /* [a-b] */
      if (op == '-' && min != EOF) uset_addRange(rl->chset, min, rt->chval);
      else return RGX_BAD_SET;
    } else  if (rt->type == TREE_SET) { /* [a-\d] || [a-[b]] */
      switch (op) {
        case '-': uset_removeAll(rl->chset, rt->chset); break;
        case '~': {
          USet * ins = uset_clone(rl->chset); /* ins = rl && rt */
          uset_retainAll(ins, rt->chset);
          uset_addAll(rl->chset, rt->chset);
          uset_removeAll(rl->chset, ins);     /* rl = (rl || rt) - ins */
          break;
        }
        case '&': uset_retainAll(rl->chset, rt->chset); break;
        case '|': uset_addAll(rl->chset, rt->chset); break;
      }
    } else return RGX_BAD_SET;
    min = EOF;
  }

  if (neg) uset_complement(rl->chset);
  if (skip_spaces(tk) && CUR == ']') { NEXT; } else return RGX_MISSING_BRACKET;
  *re = rl;
  return RGX_OK;
}

static rgx_error
parse_paren(struct tokenizer_s * tk, rgx_tree ** re)
{
  rgx_tree * rl = NULL;

  if (!MORE) return RGX_BAD_GROUP;

  if (CUR != '?') { /* (non-capture group) */
    Q(parse_alt(tk, &rl));
    goto close_paren;
  }
  NEXT; if (!MORE) return RGX_BAD_GROUP;

  if (CUR == '=') { NEXT; /* (?=look-ahead) */
    Q(parse_alt(tk, &rl));
    rl = tree_new1(tk, TREE_LOOKA, rl);
    goto close_paren;

  } else if (CUR == '!') { NEXT; /* (?!negative look-ahead) */
    Q(parse_alt(tk, &rl));
    rl = tree_new1(tk, TREE_NLOOKA, rl);
    goto close_paren;

  } else if (CUR == '<') { NEXT; if (!MORE) return RGX_BAD_GROUP;

    if (CUR == '=') { NEXT; /* (?<=look-behind) */
      Q(parse_alt(tk, &rl));
      rl = tree_new1(tk, TREE_LOOKB, rl);
      goto close_paren;

    } else if (CUR == '!') { NEXT; /* (?<!negative look-behind) */
      Q(parse_alt(tk, &rl));
      rl = tree_new1(tk, TREE_NLOOKB, rl);
      goto close_paren;
    }

  } else if (CUR == '/') { /* (?/procname:expr) */
    UChar buf[128];
    NEXT; if (!MORE) return RGX_BAD_GROUP;
    Q(gather(tk, buf, 128, ':', RGX_BAD_NAME));
    Q(parse_alt(tk, &rl));
    Q(lookup_proc(tk, buf, rl, NULL));
    if (skip_spaces(tk) && CUR == ')') { NEXT; } else return RGX_MISSING_PAREN;
    return parse_single(tk, re);

  } else if (CUR == '?') { /* (??ctf) (??!ctf) */
    rgx_tree * rb;
    index_t idx;
    bool isneg = false;
    UChar buf[128];
    NEXT; if (!MORE) return RGX_BAD_GROUP;
    if (CUR == '!') { isneg = true; NEXT; if (!MORE) return RGX_BAD_GROUP; }
    Q(parse_single(tk, &rb));
    u_snprintf(buf, 128, ";%u", (unsigned)tk->procslen); /* ';' prevents \g;23; */
    Q(lookup_proc(tk, buf, rb, &idx));
    Q(parse_concat(tk, &rb)); /* left=true, right=false */
    if (isneg) { rgx_tree * tmp = rb->left; rb->left = rb->right; rb->right = tmp; }
    rl = tree_new1(tk, TREE_COND, rb);
    rl->procindex = idx;
    goto close_paren;

  } else { /* (?name:expr) */
    UChar buf[128];
    Q(gather(tk, buf, 128, ':', RGX_BAD_NAME));
    Q(parse_alt(tk, &rl));
    rl = tree_new1(tk, TREE_GROUP, rl);
    Q(lookup_group(tk, buf, true, &rl->capindex));
  }

close_paren:
  if (skip_spaces(tk) && CUR == ')') { NEXT; } else return RGX_MISSING_PAREN;
  *re = rl;
  return RGX_OK;
}

static rgx_error
parse_single(struct tokenizer_s * tk, rgx_tree ** re)
{
  rgx_tree * rl = NULL;

  if (skip_spaces(tk)) {
    if (CUR == '(') { NEXT; return parse_paren(tk, re); }
    if (CUR == '[') { NEXT; return parse_bracket(tk, re); }
    if (CUR == '.') { NEXT; *re = tree_new(tk, TREE_ANY); return RGX_OK; }
    if (CUR == '^') { NEXT; *re = tree_new(tk, TREE_BOL); return RGX_OK; }
    if (CUR == '$') { NEXT; *re = tree_new(tk, TREE_EOL); return RGX_OK; }
    if (CUR == '{') { NEXT;
      *re = tree_new(tk, TREE_CHAR);
      return parse_directive(tk, *re);
    }
    if (CUR == '\\') { NEXT;
      *re = tree_new(tk, TREE_CHAR);
      return parse_escape(tk, *re);
    }
    /* we obviously don't need a unicode version of strchr */
    if (!CUR || CUR >= 0x80 || !strchr("*+?|()[]{}", CUR)) {
      rl = tree_new(tk, TREE_CHAR);
      rl->chval = CUR;
      NEXT;
    }
  }
  *re = rl;
  return RGX_OK;
}

#define MAYBE_INT(DST) do{ \
  UChar * endp_ = (UChar *)tk->iter.curp; \
  long int v_ = uni_strtol(tk->iter.curp, &endp_, 10); \
  if (v_ < 0 || v_ > RGX_REP_MAX) return RGX_BAD_REPEAT; \
  tk->iter.curp = endp_; NEXT; \
  (DST) = (int)v_; \
}while(0)

static rgx_error
parse_repeat(struct tokenizer_s * tk, rgx_tree ** re)
{
  rgx_tree * rl = NULL;
  UChar32 c;

  Q(parse_single(tk, &rl));
  if (rl && skip_spaces(tk)) {
    if      (CUR == '*') { NEXT; rl = tree_new1(tk, TREE_STAR, rl); }
    else if (CUR == '+') { NEXT; rl = tree_new1(tk, TREE_PLUS, rl); }
    else if (CUR == '?') { NEXT; rl = tree_new1(tk, TREE_QUEST, rl); }
    else if (CUR == '{' && ((c = PEEK) == ',' || c == '}' ||
                            uset_contains(ucat_digit, c) ||
                            uset_contains(ucat_space, c))) {
      int min = 0;
      int max = 0;
      MAYBE_INT(min);
      if (skip_spaces(tk) && CUR == ',') { MAYBE_INT(max); } else { max = min; }
      if (skip_spaces(tk) && CUR == '}') { NEXT; } else return RGX_MISSING_BRACE;
      if (max && min > max) return RGX_BAD_REPEAT;
      rl = tree_new1(tk, TREE_REPEAT, rl);
      rl->repmin = min;
      rl->repmax = max;
    } else goto done;

    rl->repgreedy = true;
    if (MORE && CUR == '?') { NEXT; rl->repgreedy = false; }
  }
done:
  *re = rl;
  return RGX_OK;
}

static rgx_error
parse_concat(struct tokenizer_s * tk, rgx_tree ** re)
{
  rgx_tree * rl = NULL;
  rgx_tree * rr = NULL;

  Q(parse_repeat(tk, &rl));
  if (rl) {
    while (MORE) {
      Q(parse_repeat(tk, &rr));
      if (!rr) break;
      rl = tree_new2(tk, TREE_CAT, rl, rr);
    }
  }
  *re = rl;
  return RGX_OK;
}

static rgx_error
parse_alt(struct tokenizer_s * tk, rgx_tree ** re)
{
  rgx_tree * rl = NULL;
  rgx_tree * rr = NULL;

  Q(parse_concat(tk, &rl));
  while (skip_spaces(tk) && CUR == '|') { NEXT;
    Q(parse_concat(tk, &rr));
    rl = tree_new2(tk, TREE_ALT, rl, rr);
  }
  *re = rl;
  return RGX_OK;
}

static rgx_error
parse_full(struct tokenizer_s * tk, rgx_tree ** re)
{
  rgx_tree * rl = NULL;
  NEXT;
  Q(parse_alt(tk, &rl));
  if (skip_spaces(tk)) return RGX_EXTRA_JUNK;
  *re = rl;
  return RGX_OK;
}

/* ********************************************************************** */
/* ********************************************************************** */

typedef enum rgx_code_type_e {
  OP_CHAR, OP_SET, OP_ANY, OP_NONE,
  OP_BOL, OP_NBOL, OP_EOL, OP_NEOL,
  OP_BOT, OP_NBOT, OP_EOT, OP_NEOT,
  OP_WBND, OP_NWBND,
  OP_LOOK, OP_NLOOK, OP_LOOKR, OP_NLOOKR,
  OP_BREF, OP_NBREF, OP_QREF, OP_NQREF, OP_PROC, OP_NPROC,
  OP_COND,
  OP_JUMP, OP_SPLITLO, OP_SPLITHI,
  OP_SAVE,
  OP_MATCH,
} rgx_code_type;

typedef struct rgx_code_s rgx_code;
struct rgx_code_s {
  rgx_code_type opcode;
  unsigned int generation;
  union {
    rgx_code * xaddr;   /* OP_SPLIT*, OP_JUMP, OP_*LOOK*, OP_PROC, OP_BREF, OP_QREF, OP_COND */
    USet * xset;        /* OP_SET */
    UChar32 literalc;   /* OP_CHAR */
    struct {            /* OP_SAVE, OP_PROC, OP_BREF, OP_QREF, OP_COND */
      index_t xsubidx;
      bool rev;
    } proc;
  } u;
};
#define addr      u.xaddr
#define subidx    u.proc.xsubidx
#define reversed  u.proc.rev
#define valc      u.literalc
#define cset      u.xset

typedef struct rgx_prog_s rgx_prog;
struct rgx_prog_s {
  size_t nameslen;
  UChar ** names;
  size_t len;
  rgx_code * start;
};

#define EMIT(X)     (pc = emit(pc, (X), forward))
#define EMITFWD(X)  (pc = emit(pc, (X), true))
#define EMITREV(X)  (pc = emit(pc, (X), false))

static rgx_code * emit(rgx_code * pc, rgx_tree * re, bool forward);

static rgx_code *
emit_quest(rgx_code * pc, rgx_tree * re, int n, bool forward)
{
  /* 0. split 1,4
   * 1. expr
   * 2. split 3,4
   * 3. expr
   */
  rgx_code * split;
  split = pc++; split->opcode = OP_SPLITLO;
  EMIT(re->left);
  if (--n) pc = emit_quest(pc, re, n, forward);
  split->addr = pc;
  if (!re->repgreedy) split->opcode = OP_SPLITHI;
  return pc;
}

static rgx_code *
emit(rgx_code * pc, rgx_tree * re, bool forward)
{
  rgx_code * split = NULL;
  rgx_code * jump = NULL;

  if (!re) return pc;
  switch (re->type) {
    case TREE_ALT: { /* 0. split 1,3; 1. expr1; 2. jump 4; 3. expr2 */
      split = pc++; split->opcode = OP_SPLITLO;
      EMIT(re->left);
      jump = pc++; jump->opcode = OP_JUMP;
      split->addr = pc;
      EMIT(re->right);
      jump->addr = pc;
      break;
    }
    case TREE_CAT: { /* 0. expr1; 1. expr2 */
      if (forward) { EMIT(re->left); EMIT(re->right); }
      else         { EMIT(re->right); EMIT(re->left); }
      break;
    }
    case TREE_GROUP: { /* 0. save (start); 1. expr; 2. save (end) */
      pc->opcode = OP_SAVE; pc->subidx = (index_t)(2 * re->capindex + (forward ? 0 : 1));
      pc++;
      EMIT(re->left);
      pc->opcode = OP_SAVE; pc->subidx = (index_t)(2 * re->capindex + (forward ? 1 : 0));
      pc++;
      break;
    }
    case TREE_PROC: { /* 0. proc P0 */
      pc->opcode = OP_PROC; pc->subidx = re->procindex; pc->reversed = !forward;
      pc++;
      break;
    }
    case TREE_NPROC: { /* 0. negproc P0 */
      pc->opcode = OP_NPROC; pc->subidx = re->procindex; pc->reversed = !forward;
      pc++;
      break;
    }
    case TREE_BREF: { /* 0. backref (start) */
      pc->opcode = OP_BREF; pc->subidx = (index_t)(2 * re->capindex);
      pc++;
      break;
    }
    case TREE_NBREF: { /* 0. negbackref (start) */
      pc->opcode = OP_NBREF; pc->subidx = (index_t)(2 * re->capindex);
      pc++;
      break;
    }
    case TREE_QREF: { /* 0. quotebackref (start) */
      pc->opcode = OP_QREF; pc->subidx = (index_t)(2 * re->capindex);
      pc++;
      break;
    }
    case TREE_NQREF: { /* 0. negquotebackref (start) */
      pc->opcode = OP_NQREF; pc->subidx = (index_t)(2 * re->capindex);
      pc++;
      break;
    }
    case TREE_LOOKA: { /* 0. lookahead 3; 1. expr; 2. match */
      jump = pc++; jump->opcode = forward ? OP_LOOK : OP_LOOKR;
      EMITFWD(re->left);
      pc->opcode = OP_MATCH;
      pc++; jump->addr = pc;
      break;
    }
    case TREE_NLOOKA: { /* 0. lookahead 3; 1. expr; 2. match */
      jump = pc++; jump->opcode = forward ? OP_NLOOK : OP_NLOOKR;
      EMITFWD(re->left);
      pc->opcode = OP_MATCH;
      pc++; jump->addr = pc;
      break;
    }
    case TREE_LOOKB: { /* 0. lookbehind 3; 1. expr; 2. match */
      jump = pc++; jump->opcode = forward ? OP_LOOKR : OP_LOOK;
      EMITREV(re->left);
      pc->opcode = OP_MATCH;
      pc++; jump->addr = pc;
      break;
    }
    case TREE_NLOOKB: { /* 0. lookbehind 3; 1. expr; 2. match */
      jump = pc++; jump->opcode = forward ? OP_NLOOKR : OP_NLOOK;
      EMITREV(re->left);
      pc->opcode = OP_MATCH;
      pc++; jump->addr = pc;
      break;
    }
    case TREE_COND: {
      /* 0. cond P0; 1. jump 4; 2. texpr; 3. jump 5; 4. fexpr */
      pc->opcode = OP_COND; pc->subidx = re->procindex; pc->reversed = !forward;
      pc++; jump = pc++; jump->opcode = OP_JUMP;
      EMIT(re->left->left);
      split = pc++; split->opcode = OP_JUMP;
      jump->addr = pc;
      EMIT(re->left->right);
      split->addr = pc;
      break;
    }
    case TREE_STAR: { /* 0. split 1,3; 1. expr; 2. jump 0 */
      split = pc++; split->opcode = OP_SPLITLO;
      EMIT(re->left);
      pc->opcode = OP_JUMP; pc->addr = split;
      pc++; split->addr = pc;
      if (!re->repgreedy) split->opcode = OP_SPLITHI;
      break;
    }
    case TREE_QUEST: { pc = emit_quest(pc, re, 1, forward); break; }
    case_plus:
    case TREE_PLUS: { /* 0. expr; 1. split 0,2 */
      jump = pc;
      EMIT(re->left);
      split = pc++; split->opcode = OP_SPLITHI; split->addr = jump;
      if (!re->repgreedy) split->opcode = OP_SPLITLO;
      break;
    }
    case TREE_REPEAT: {
      if (re->repmin > 0 && re->repmax == 0) { /* x{n,0} -> xx+ */
        int m = re->repmin;
        while (--m) { EMIT(re->left); }
        goto case_plus;
      } else {
        int m = re->repmin;
        int n = re->repmax - re->repmin;
        while (m--) { EMIT(re->left); }             /* x{m,n} -> xx{0,<n-m>} */
        if (n) pc = emit_quest(pc, re, n, forward); /* x{0,n} -> (x(x)?)? */
      }
      break;
    }
    case TREE_CHAR:  { pc->opcode = OP_CHAR; pc->valc = re->chval; pc++; break; }
    case TREE_SET:   { pc->opcode = OP_SET;  pc->cset = re->chset; pc++; break; }
    case TREE_ANY:   { pc->opcode = OP_ANY;  pc++; break; }
    case TREE_NONE:  { pc->opcode = OP_NONE; pc++; break; }
    case TREE_BOL:   { pc->opcode = forward ? OP_BOL  : OP_EOL;  pc++; break; }
    case TREE_NBOL:  { pc->opcode = forward ? OP_NBOL : OP_NEOL; pc++; break; }
    case TREE_EOL:   { pc->opcode = forward ? OP_EOL  : OP_BOL;  pc++; break; }
    case TREE_NEOL:  { pc->opcode = forward ? OP_NEOL : OP_NBOL; pc++; break; }
    case TREE_BOT:   { pc->opcode = forward ? OP_BOT  : OP_EOT;  pc++; break; }
    case TREE_NBOT:  { pc->opcode = forward ? OP_NBOT : OP_NEOT; pc++; break; }
    case TREE_EOT:   { pc->opcode = forward ? OP_EOT  : OP_BOT;  pc++; break; }
    case TREE_NEOT:  { pc->opcode = forward ? OP_NEOT : OP_NBOT; pc++; break; }
    case TREE_WBND:  { pc->opcode = OP_WBND;  pc++; break; }
    case TREE_NWBND: { pc->opcode = OP_NWBND; pc++; break; }
  }
  return pc;
}

static rgx_error
count(rgx_tree * re, size_t * n)
{
  size_t a = 1; /* note: we assume one opcode */
  size_t b = 0;
  size_t c = 0;
  if (!re) return RGX_OK;
  switch (re->type) {
    case TREE_CHAR:   break;
    case TREE_ANY:    break;
    case TREE_NONE:   break;
    case TREE_BOL:    break;
    case TREE_NBOL:   break;
    case TREE_EOL:    break;
    case TREE_NEOL:   break;
    case TREE_BOT:    break;
    case TREE_NBOT:   break;
    case TREE_EOT:    break;
    case TREE_NEOT:   break;
    case TREE_WBND:   break;
    case TREE_NWBND:  break;
    case TREE_BREF:   break;
    case TREE_NBREF:  break;
    case TREE_QREF:   break;
    case TREE_NQREF:  break;
    case TREE_PROC:   break;
    case TREE_NPROC:  break;
    case TREE_LOOKA:  a = 2; Q(count(re->left, &b)); break;
    case TREE_NLOOKA: a = 2; Q(count(re->left, &b)); break;
    case TREE_LOOKB:  a = 2; Q(count(re->left, &b)); break;
    case TREE_NLOOKB: a = 2; Q(count(re->left, &b)); break;
    case TREE_COND:   a = 3; Q(count(re->left->left, &b)); Q(count(re->left->right, &c)); break;
    case TREE_SET:    break;
    case TREE_ALT:    a = 2; Q(count(re->left, &b)); Q(count(re->right, &c)); break;
    case TREE_CAT:    a = 0; Q(count(re->left, &b)); Q(count(re->right, &c)); break;
    case TREE_GROUP:  a = 2; Q(count(re->left, &b)); break;
    case TREE_QUEST:         Q(count(re->left, &b)); break;
    case TREE_PLUS:          Q(count(re->left, &b)); break;
    case TREE_STAR:   a = 2; Q(count(re->left, &b)); break;
    case TREE_REPEAT: {
      /* We're not too careful with overflow checking, since
       * (RGX_CODE_MAX * RGX_REP_MAX) is less than SIZE_MAX.
       */
      Q(count(re->left, &b));
      if (re->repmax) { /* repeat (max) times, with (max-min) '?' branches */
        a = (size_t)(re->repmax - re->repmin);
        b *= (size_t)re->repmax;
      } else { /* repeat (min) times, with a final '+' branch */
        b *= (size_t)re->repmin;
      }
      break;
    }
  }
  if (a + b + c >= RGX_CODE_MAX - *n) return RGX_TOO_LONG;
  *n += a + b + c;
  return RGX_OK;
}

static UChar *
rgx_strecpy(UChar * dst, const UChar * src)
{
  while (*src) *dst++ = *src++;
  *dst = '\0';
  return dst;
}

rgx_error
rgx_compile(rgx_prog ** program, const UChar * pattern, size_t patlen)
{
  rgx_prog * prog;
  rgx_tree * rtree = NULL;
  struct tokenizer_s tk;

  if (patlen >= RGX_LEN_MAX) return RGX_TOO_LONG;

  if (!ucat_init) Q(init_charsets());

  uni_iter_init(&tk.iter, pattern, patlen);
  tk.cur = EOF;
  /* tree node buffer */
  tk.nodesidx = 0;
  tk.nodeslen = patlen * 2 + 4;
  QN(tk.nodes = malloc(tk.nodeslen * sizeof(rgx_tree))); /* upper-bound on tree nodes */
  /* named capture groups */
  tk.refs = NULL;
  tk.refslen = 0;
  { UChar nil = 0; Q(lookup_group(&tk, &nil, true, NULL)); } /* entire match */
  /* procedures */
  tk.procs = NULL;
  tk.procslen = 0;

  Q(parse_full(&tk, &rtree));

  { /* integrity checking */
    size_t i;
    for (i = 0; i < tk.refslen; ++i) {
      if (tk.refs[i].defined == false) return RGX_UNDEFINED;
    }
    for (i = 0; i < tk.procslen; ++i) {
      if (tk.procs[i].body == NULL) {
        printf("UNDEF: %s\n", ustr0(tk.procs[i].name));
        return RGX_UNDEFINED;
      }
    }
  }

  { /* .*?(regex) */
    rgx_tree * cap = tree_new1(&tk, TREE_GROUP, rtree);
    rgx_tree * rep = tree_new1(&tk, TREE_STAR, tree_new(&tk, TREE_ANY));
    cap->capindex = 0;
    rep->repgreedy = false;
    rtree = tree_new2(&tk, TREE_CAT, rep, cap);
  }
  /* Go easy on our GC; put everything in the same alloc */
  {
    size_t opcnt = 1 + (tk.procslen * 6);     /* match + [save...save match]*2 */
    size_t nlen = tk.refslen * sizeof(UChar); /* \0 terminators */
    size_t i;
    for (i = 0; i < tk.refslen; ++i)
      nlen += (size_t)u_strlen(tk.refs[i].name) * sizeof(UChar);
    Q(count(rtree, &opcnt));
    for (i = 0; i < tk.procslen; ++i) {
      size_t n = 0;
      Q(count(tk.procs[i].body, &n));
      opcnt += n + n; /* forward and backward */
      if (opcnt >= RGX_CODE_MAX) return RGX_TOO_LONG;
    }
    QN(prog = malloc(sizeof(rgx_prog)                 /* root struct */
                     + opcnt * sizeof(rgx_code)       /* compiled program */
                     + tk.refslen * sizeof(UChar*)    /* pointers to name data */
                     + nlen));                        /* name data */
  }
  prog->start = (rgx_code*)(prog + 1);
  { /* 0. jump 3; 1. proc; 2. match */
    rgx_code * pc = prog->start;
    pc = emit(pc, rtree, true);
    pc->opcode = OP_MATCH;
    pc++;
    /* emit procedures, then patch addresses for procedure calls */
    if (tk.procslen) {
      rgx_code * patch = prog->start;
      rgx_tree cap;
      size_t i;
      cap.type = TREE_GROUP;
      cap.capindex = 0;
      for (i = 0; i < tk.procslen; ++i) {
        tk.procs[i].locfwd = pc;
        cap.left = tk.procs[i].body;
        pc = emit(pc, &cap, true);
        pc->opcode = OP_MATCH;
        pc++;
        tk.procs[i].locrev = pc;
        pc = emit(pc, &cap, false);
        pc->opcode = OP_MATCH;
        pc++;
      }
      while (patch < pc) {
        if (patch->opcode == OP_PROC || patch->opcode == OP_NPROC ||
            patch->opcode == OP_COND)
          patch->addr = patch->reversed ? tk.procs[patch->subidx].locrev
                                        : tk.procs[patch->subidx].locfwd;
        patch++;
      }
    }
    prog->len = (size_t)(pc - prog->start);
    prog->names = (UChar **)pc;
  }
  prog->nameslen = tk.refslen;
  {
    UChar * p = (UChar *)(prog->names + tk.refslen);
    size_t i;
    for (i = 0; i < tk.refslen; ++i) {
      prog->names[i] = p;
      p = rgx_strecpy(p, tk.refs[i].name) + 1;
    }
  }
  *program = prog;
  return RGX_OK;
}

UChar **
rgx_group_names(rgx_prog * prog)
{
  return prog->names;
}

size_t
rgx_group_count(rgx_prog * prog)
{
  return prog->nameslen;
}

static void
charset_print(USet * s)
{
  UErrorCode uec = U_ZERO_ERROR;
  UChar buf[1024*16];
  uset_toPattern(s, buf, 1024*16, true, &uec);
  printf("%s", ustr0(buf));
}

void
rgx_print_prog(rgx_prog * prog)
{
  rgx_code * start = prog->start;
  rgx_code * end = start + prog->len;
  rgx_code * pc = start;
  size_t i;

  printf("groups: %u [ ", (unsigned)(prog->nameslen));
  for (i = 0; i < prog->nameslen; ++i) printf("'%s' ", ustr0(prog->names[i]));
  printf("]\n");
  for (; pc < end; pc++) {
    printf("%2u. ", (unsigned)(pc - start));
    switch (pc->opcode) {
      case OP_MATCH:  printf("match"); break;
      case OP_CHAR:   printf("char '%c'", pc->valc); break;
      case OP_SET:    printf("set "); charset_print(pc->cset); break;
      case OP_ANY:    printf("char any"); break;
      case OP_NONE:   printf("char none"); break;
      case OP_BOL:    printf("line begin"); break;
      case OP_NBOL:   printf("not line begin"); break;
      case OP_EOL:    printf("line end"); break;
      case OP_NEOL:   printf("not line end"); break;
      case OP_BOT:    printf("text begin"); break;
      case OP_NBOT:   printf("not text begin"); break;
      case OP_EOT:    printf("text end"); break;
      case OP_NEOT:   printf("not text end"); break;
      case OP_WBND:   printf("word boundary"); break;
      case OP_NWBND:  printf("not word boundary"); break;
      case OP_BREF:   printf("backref (%u)", (unsigned)pc->subidx); break;
      case OP_NBREF:  printf("negative backref (%u)", (unsigned)pc->subidx); break;
      case OP_QREF:   printf("quotebackref (%u)", (unsigned)pc->subidx); break;
      case OP_NQREF:  printf("negative quotebackref (%u)", (unsigned)pc->subidx); break;
      case OP_PROC:   printf("proc %lu", (unsigned long)(pc->addr - start)); break;
      case OP_NPROC:  printf("negative proc %lu", (unsigned long)(pc->addr - start)); break;
      case OP_LOOK:   printf("look-ahead %lu", (unsigned long)(pc->addr - start)); break;
      case OP_NLOOK:  printf("negative look-ahead %lu", (unsigned long)(pc->addr - start)); break;
      case OP_LOOKR:  printf("look-behind %lu", (unsigned long)(pc->addr - start)); break;
      case OP_NLOOKR: printf("negative look-behind %lu", (unsigned long)(pc->addr - start)); break;
      case OP_COND:   printf("cond %lu", (unsigned long)(pc->addr - start)); break;
      case OP_JUMP:   printf("jump %lu", (unsigned long)(pc->addr - start)); break;
      case OP_SPLITLO:printf("split lo %lu", (unsigned long)(pc->addr - start)); break;
      case OP_SPLITHI:printf("split hi %lu", (unsigned long)(pc->addr - start)); break;
      case OP_SAVE:   printf("save (%u)", (unsigned)pc->subidx); break;
    }
    printf("\n");
  }
}

/* ********************************************************************** */
/* ********************************************************************** */

typedef struct rgx_submatch_s rgx_submatch;
struct rgx_submatch_s {
  int ref;
  UChar * ptrs[1];
};

struct matcher_s {
  rgx_prog * prog;
  unsigned int generation;
  uni_iter iter;
  UChar32 cur;
  bool reverse;
  size_t nsubs;
  rgx_submatch * freesub;
};

static rgx_submatch *
sub_new(struct matcher_s * mm)
{
  rgx_submatch * s = mm->freesub;
  if (s != NULL) mm->freesub = (rgx_submatch*)s->ptrs[0];
  else s = malloc(sizeof(rgx_submatch) + mm->nsubs * sizeof(UChar*));
  s->ref = 1;
  return s;
}

static void
sub_dec(struct matcher_s * mm, rgx_submatch * s)
{
  if (--s->ref == 0) {
    s->ptrs[0] = (UChar*)mm->freesub;
    mm->freesub = s;
  }
}

#define sub_inc(MM,S)  ( (S)->ref++, (S) )

static rgx_submatch *
sub_update(struct matcher_s * mm, rgx_submatch * s, size_t i)
{
  if (s->ref > 1) {
    rgx_submatch * s1 = sub_new(mm);
    memcpy(s1->ptrs, s->ptrs, mm->nsubs * sizeof(UChar*));
    s->ref--;
    s = s1;
  }
  s->ptrs[i] = (UChar*)mm->iter.curp;
  return s;
}

/* ********************************************************************** */
/* ********************************************************************** */

typedef struct rgx_thread_s rgx_thread;
struct rgx_thread_s {
  rgx_code * pc;
  const UChar * resume;
  rgx_submatch * sub;
};

#define thread_new(PC,SUB)            ((rgx_thread){ (PC), NULL,     (SUB) })
#define thread_paused(PC,RESUME,SUB)  ((rgx_thread){ (PC), (RESUME), (SUB) })

typedef struct rgx_threadlist_s rgx_threadlist;
struct rgx_threadlist_s {
  size_t len;
  rgx_thread * threads;
};

#define MATCH(EX)  do{ if (EX) goto keep_thread; else goto drop_thread; }while(0)
#define NMATCH(EX) do{ if (EX) goto drop_thread; else goto keep_thread; }while(0)
#define MATCHJ(EX) do{ if (EX) goto jump_thread; else goto drop_thread; }while(0)
#undef MORE
#undef CUR
#undef NEXT
#undef PEEK
#define MORE  (mm->cur != EOF)
#define CUR   (mm->cur)
#define NEXT  (mm->cur = mm->reverse ? uni_iter_prev(&mm->iter) : uni_iter_next(&mm->iter))
#define PEEK  (mm->reverse ? uni_iter_rpeek(&mm->iter) : uni_iter_peek(&mm->iter))

static bool rgx_exec1(struct matcher_s * mm, rgx_code * pc, rgx_submatch ** sub);

static bool
match_backref(struct matcher_s * mm, bool quoted, rgx_thread t, const UChar ** resume)
{
  const UChar * ref = t.sub->ptrs[t.pc->subidx];
  const UChar * end = t.sub->ptrs[t.pc->subidx + 1];
  size_t n;
  if (!ref || !end) return false; /* failed submatch */
  n = (size_t)(end - ref);
  if (mm->reverse) {
    if ((size_t)(mm->iter.curp - mm->iter.startp) < n) return false;
    if (quoted ? !uni_quote_equal(mm->iter.curp - n, ref, n)
               : u_memcmp(mm->iter.curp - n, ref, (int32_t)n)) return false;
    if (resume) *resume = mm->iter.curp - n;
  } else {
    if ((size_t)(mm->iter.endp - mm->iter.curp) < n) return false;
    if (quoted ? !uni_quote_equal(mm->iter.curp, ref, n)
               : u_memcmp(mm->iter.curp, ref, (int32_t)n)) return false;
    if (resume) *resume = mm->iter.curp + n;
  }
  return true;
}

#define RECURSE(DST,REV) do{ \
  struct matcher_s mmtmp_ = *mm; \
  mm->reverse = (REV); \
  (DST) = rgx_exec1(mm, t.pc + 1, &t.sub); \
  *mm = mmtmp_; \
}while(0)

static void
addthread(struct matcher_s * mm, rgx_threadlist * tlist, rgx_thread t)
{
  bool b;
  UChar32 c;
  if (t.pc->generation == mm->generation) goto drop_thread; /* already in list */
  t.pc->generation = mm->generation;

  switch (t.pc->opcode) {
    jump_thread:
    case OP_JUMP: {
      addthread(mm, tlist, thread_new(t.pc->addr, t.sub));
      break;
    }
    case OP_SPLITLO: {
      addthread(mm, tlist, thread_new(t.pc + 1, sub_inc(mm, t.sub)));
      addthread(mm, tlist, thread_new(t.pc->addr, t.sub));
      break;
    }
    case OP_SPLITHI: {
      addthread(mm, tlist, thread_new(t.pc->addr, sub_inc(mm, t.sub)));
      addthread(mm, tlist, thread_new(t.pc + 1, t.sub));
      break;
    }
    case OP_SAVE: {
      addthread(mm, tlist, thread_new(t.pc + 1, sub_update(mm, t.sub, (size_t)t.pc->subidx)));
      break;
    }
    case OP_BOL:   MATCH(!MORE || uset_contains(ucat_vspace, CUR));
    case OP_NBOL: NMATCH(!MORE || uset_contains(ucat_vspace, CUR));
    case OP_EOL:   MATCH((c = PEEK) == EOF || uset_contains(ucat_vspace, c));
    case OP_NEOL: NMATCH((c = PEEK) == EOF || uset_contains(ucat_vspace, c));
    case OP_BOT:   MATCH(!MORE);
    case OP_NBOT: NMATCH(!MORE);
    case OP_EOT:   MATCH(PEEK == EOF);
    case OP_NEOT: NMATCH(PEEK == EOF);
    case OP_WBND: {
      bool iswA = MORE && uset_contains(ucat_word, CUR);
      bool iswB = (c = PEEK) != EOF && uset_contains(ucat_word, c);
      MATCH(iswA != iswB);
    }
    case OP_NWBND: {
      bool iswA = MORE && uset_contains(ucat_word, CUR);
      bool iswB = (c = PEEK) != EOF && uset_contains(ucat_word, c);
      NMATCH(iswA != iswB);
    }
    case OP_LOOK:   RECURSE(b,  mm->reverse); MATCHJ( b);
    case OP_NLOOK:  RECURSE(b,  mm->reverse); MATCHJ(!b);
    case OP_LOOKR:  RECURSE(b, !mm->reverse); MATCHJ( b);
    case OP_NLOOKR: RECURSE(b, !mm->reverse); MATCHJ(!b);

    case OP_BREF: { /* handled here because we need curp to be useful */
      const UChar * resume = NULL;
      b = match_backref(mm, false, t, &resume);
      if (!b) goto drop_thread;
      tlist->threads[tlist->len++] = thread_paused(t.pc, resume, t.sub);
      break;
    }
    case OP_NBREF: { /* zero-width assertion; matches only if backref doesn't */
      MATCH(!match_backref(mm, false, t, NULL));
    }

    case OP_QREF: {
      const UChar * resume = NULL;
      b = match_backref(mm, true, t, &resume);
      if (!b) goto drop_thread;
      tlist->threads[tlist->len++] = thread_paused(t.pc, resume, t.sub);
      break;
    }
    case OP_NQREF: {
      MATCH(!match_backref(mm, true, t, NULL));
    }

    case OP_PROC: {
      const UChar * resume = NULL;
      UChar * sav[2] = { t.sub->ptrs[0], t.sub->ptrs[1] };
      rgx_submatch * freetmp = mm->freesub;
      b = rgx_exec1(mm, t.pc->addr, &t.sub);
      if (b) resume = mm->reverse ? t.sub->ptrs[0] : t.sub->ptrs[1];
      {
        struct matcher_s mmtmp = *mm;
        unsigned int gentmp = mm->generation;
        *mm = mmtmp;
        mm->generation = gentmp;
        mm->freesub = freetmp;
      }
      t.sub->ptrs[0] = sav[0]; t.sub->ptrs[1] = sav[1];
      if (!b) goto drop_thread;
      tlist->threads[tlist->len++] = thread_paused(t.pc, resume, t.sub);
      break;
    }
    case OP_NPROC: { /* zero-width assertion; matches only if proc doesn't */
      UChar * sav[2] = { t.sub->ptrs[0], t.sub->ptrs[1] };
      struct matcher_s mmtmp = *mm;
      b = rgx_exec1(mm, t.pc->addr, &t.sub);
      {
        struct matcher_s mmtmp = *mm;
        unsigned int gentmp = mm->generation;
        *mm = mmtmp;
        mm->generation = gentmp;
        mm->freesub = freetmp;
      }
      t.sub->ptrs[0] = sav[0]; t.sub->ptrs[1] = sav[1];
      MATCH(!b);
    }

    case OP_COND: {
      UChar * sav[2] = { t.sub->ptrs[0], t.sub->ptrs[1] };
      struct matcher_s mmtmp = *mm;
      b = rgx_exec1(mm, t.pc->addr, &t.sub);
      {
        struct matcher_s mmtmp = *mm;
        unsigned int gentmp = mm->generation;
        *mm = mmtmp;
        mm->generation = gentmp;
        mm->freesub = freetmp;
      }
      t.sub->ptrs[0] = sav[0]; t.sub->ptrs[1] = sav[1];
      addthread(mm, tlist, thread_new(t.pc + (b ? 2 : 1), t.sub));
      break;
    }

    drop_thread:
    case OP_NONE: {
      sub_dec(mm, t.sub);
      break;
    }
    keep_thread: {
      addthread(mm, tlist, thread_new(t.pc + 1, t.sub));
      break;
    }
    default: {
      tlist->threads[tlist->len++] = t;
      break;
    }
  }
}

static bool
rgx_exec1(struct matcher_s * mm, rgx_code * pc, rgx_submatch ** subp)
{
  rgx_threadlist tlc;
  rgx_threadlist tln;
  rgx_threadlist * tlcurr = &tlc;
  rgx_threadlist * tlnext = &tln;
  rgx_submatch * curmatches = NULL;
  rgx_submatch * sub;
  size_t i;

  tlcurr->threads = malloc(mm->prog->len * sizeof(rgx_thread)); tlcurr->len = 0;
  tlnext->threads = malloc(mm->prog->len * sizeof(rgx_thread)); tlnext->len = 0;

  addthread(mm, tlcurr, thread_new(pc, sub_inc(mm, *subp)));

  while (tlcurr->len > 0) {
    NEXT;
    mm->generation++;
    for (i = 0; i < tlcurr->len; ++i) {
      pc = tlcurr->threads[i].pc;
      sub = tlcurr->threads[i].sub;
      switch (pc->opcode) {
        case OP_MATCH: {
          if (curmatches) sub_dec(mm, curmatches);
          curmatches = sub;
          while (++i < tlcurr->len) sub_dec(mm, tlcurr->threads[i].sub);
          break;
        }
        case OP_SET:  MATCH(MORE && uset_contains(pc->cset, CUR));
        case OP_CHAR: MATCH(MORE && CUR == pc->valc);
        case OP_ANY:  MATCH(MORE);

        case OP_BREF: /* if seen here, match already happened */
        case OP_QREF:
        case OP_PROC: {
          const UChar * resume = tlcurr->threads[i].resume;
          if (mm->reverse ? mm->iter.curp <= resume : mm->iter.curp >= resume) goto keep_thread;
          tlnext->threads[tlnext->len++] = tlcurr->threads[i];
          break;
        }

        keep_thread: {
          addthread(mm, tlnext, thread_new(pc + 1, sub));
          break;
        }
        drop_thread: {
          sub_dec(mm, sub);
          break;
        }
        default: break; /* silence compiler warnings */
      }
    }
    { rgx_threadlist * tmp = tlcurr; tlcurr = tlnext; tlnext = tmp; }
    tlnext->len = 0;
    if (!MORE) break;
  }
  free(tlcurr->threads);
  free(tlnext->threads);
  if (curmatches) { *subp = curmatches; return true; }
  return false;
}

bool
rgx_exec(rgx_prog * prog, const UChar * input, size_t inputlen, UChar ** subp, size_t nsubp)
{
  struct matcher_s matcher;
  struct matcher_s * mm = &matcher;
  rgx_submatch * sub;
  size_t i;

  uni_iter_init(&mm->iter, input, inputlen);
  mm->prog = prog;
  mm->generation = 1;
  mm->cur = EOF;
  mm->reverse = false;
  mm->nsubs = nsubp;
  mm->freesub = NULL;

  for (i = 0; i < prog->len; ++i) prog->start[i].generation = 0;
  sub = sub_new(mm);
  memset(sub->ptrs, 0, mm->nsubs * sizeof(UChar*));

  if (rgx_exec1(mm, prog->start, &sub)) {
    memcpy(subp, sub->ptrs, mm->nsubs * sizeof(UChar*));
    sub_dec(mm, sub);
    return true;
  }
  return false;
}

/* ********************************************************************** */
/* ********************************************************************** */

#include <time.h>
#include "bml.h"
#if 1
#define REPS 5000
#else
#define REPS 1
#endif
#define MAXSUB 20

#if 0
#define LOG_COMPILE(X) do{ (X); fflush(stdout); }while(0)
#else
#define LOG_COMPILE(X)
#endif

#include <execinfo.h>
#include <signal.h>
void
print_trace(int f)
{
  void * array[50];
  int size = backtrace(array, 50);
  printf("Stack:\n"); fflush(stdout);
  backtrace_symbols_fd(array, size, 1);
  f = f;
  abort();
}


#define BUFMAX 128
static UChar pattern[BUFMAX];
static UChar input[BUFMAX];
static UChar matches[MAXSUB * 2][BUFMAX]; /* name,value pairs */
static size_t matcheslen;
static rgx_prog * program;

/* <test rgx="foo" str="foo" ="foo" subname="foo" /> */
int
readline(void)
{
  UErrorCode uec = U_ZERO_ERROR;
  char buf[128];
  bml_token tk = bml_next(stdin, buf, 128);
  if (tk != BML_OPEN || strcmp(buf, "test")) return 0;

  matches[0][0] = 0xFFFF;
  matches[0][1] = '\0';
  matcheslen = 0;
  while ((tk = bml_next(stdin, buf, 128)) != BML_EOF) {
    if (tk == BML_EMPTY) break;
    if (tk == BML_VALUE) {
      matches[0][0] = '\0';
      u_strFromUTF8(matches[1], BUFMAX, NULL, buf, -1, &uec);
      if (!matcheslen) matcheslen = 1;
      continue;
    }
    if (tk == BML_ATTR) {
      if (!strcmp(buf, "rgx")) {
        bml_next(stdin, buf, BUFMAX);
        u_strFromUTF8(pattern, BUFMAX, NULL, buf, -1, &uec);
      } else if (!strcmp(buf, "str")) {
        bml_next(stdin, buf, BUFMAX);
        u_strFromUTF8(input, BUFMAX, NULL, buf, -1, &uec);
      } else {
        if (!matcheslen) matcheslen = 1;
        u_strFromUTF8(matches[matcheslen * 2], BUFMAX, NULL, buf, -1, &uec);
        bml_next(stdin, buf, BUFMAX);
        u_strFromUTF8(matches[matcheslen * 2 + 1], BUFMAX, NULL, buf, -1, &uec);
        matcheslen++;
      }
    }
  }
  return 1;
}

void
timing(void)
{
  UChar * subs[MAXSUB * 2];
  int i;
  clock_t start, finish;
  start = clock();
  for (i = 0; i < REPS; ++i)
    rgx_exec(program, input, (size_t)u_strlen(input), subs, rgx_group_count(program) * 2);
  finish = clock();
  printf("time: %3d '%s'\n", (int)((finish - start) / REPS), ustr0(pattern));
  fflush(stdout);
}

bool
maybe_report(UChar ** subs)
{
  /* only report if any; report all if any */
  UChar ** names = rgx_group_names(program);
  size_t nlen = rgx_group_count(program);
  size_t i, j;
  bool report = false;
start:;
  if (report) printf("XXX: wrong match '%s', '%s'\n", ustr0(pattern), ustr1(input));

  for (j = 0; j < nlen; ++j) {
    UChar * a = subs[j * 2];
    size_t an = (size_t)(subs[j * 2 + 1] - a);
    for (i = 0; i < matcheslen; ++i) {
      if (!u_strcmp(matches[i * 2], names[j])) {
        UChar * b = matches[i * 2 + 1];
        size_t bn = (size_t)u_strlen(b);
        if (!a || an != bn || u_memcmp(a, b, (int32_t)an)) {
          if (!report) { report = true; goto start; }
          printf("  bad match '%s': '%.*s' should be '%s'\n", ustr0(names[j]), (int)an, ustr1(a), ustr2(b));
        } else if (report) {
          printf("  good match '%s': '%.*s' is '%s'\n", ustr0(names[j]), (int)an, ustr1(a), ustr2(b));
        }
        break;
      }
    }
    if (i >= matcheslen && subs[j * 2] != NULL) {
      if (!report) { report = true; goto start; }
      printf("  bad match '%s': '%.*s' should have failed\n", ustr0(names[j]), (int)an, ustr1(a));
    }
  }
  return report;
}

int
main(void)
{
  UChar * subs[MAXSUB * 2];
  bool m;

  signal(SIGSEGV, print_trace);

  /*printf("%u\n", (unsigned)sizeof(rgx_code));*/ /* 16 */
  while (readline()) {
    LOG_COMPILE(printf("compiling: '%s'\n", ustr0(pattern)));
    {
      rgx_error err = rgx_compile(&program, pattern, (size_t)u_strlen(pattern));
      if (err) { printf("compile error %d for '%s'\n", err, ustr0(pattern)); exit(1); }
    }
    LOG_COMPILE(printf("compiled\n"));
    LOG_COMPILE(rgx_print_prog(program));

    if (rgx_group_count(program) >= MAXSUB) {
      printf("too many sub groups %u in '%s'\n", (unsigned)rgx_group_count(program), ustr0(pattern));
      continue;
    }
    memset(subs, 0, sizeof(subs));
    m = rgx_exec(program, input, (size_t)u_strlen(input), subs, rgx_group_count(program) * 2);

    if (!m && matcheslen > 0) {
      printf("XXX: failed match '%s', '%s'\n", ustr0(pattern), ustr1(input));
      goto error;
    }
    if (m && matcheslen == 0) {
      printf("XXX: unexpected match '%s', '%s', '%.*s'\n", ustr0(pattern), ustr1(input),
             (int)(subs[1] - subs[0]), ustr2(subs[0]));
      goto error;
    }
    if (maybe_report(subs)) { goto error; }

    timing(); continue;
    error: rgx_print_prog(program);
  }
  printf("done\n");
  return 0;
}

/* ********************************************************************** */
/* ********************************************************************** */
/* Portions of this program are derived from re1 by Russ Cox.
 * http://code.google.com/p/re1/
 */
// Copyright (c) 2007-2009 Russ Cox, Google Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
// * Neither the name of Google, Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
