
#ifndef DIRECTIVE
#define DIRECTIVE(V,S,E) /**/
#endif

DIRECTIVE(dir_slash,       "$",           D_SLASH)       /* \g\G */
DIRECTIVE(dir_colon,       ":",           D_COLON)       /* \m\M */
DIRECTIVE(dir_equal,       "=",           D_EQUAL)       /* \k\K */
DIRECTIVE(dir_any,         "any",         D_ANY)         /* . */
DIRECTIVE(dir_call,        "call",        D_CALL)        /* \g\G */
DIRECTIVE(dir_close_brace, "close-brace", D_CLOSE_BRACE) /* \c\C */
DIRECTIVE(dir_digit,       "digit",       D_DIGIT)       /* \d\D */
DIRECTIVE(dir_hspace,      "hspace",      D_HSPACE)      /* \h\H */
DIRECTIVE(dir_input_end,   "input-end",   D_INPUT_END)   /* \Z */
DIRECTIVE(dir_input_start, "input-start", D_INPUT_START) /* \A */
DIRECTIVE(dir_line_end,    "line-end",    D_LINE_END)    /* $ */
DIRECTIVE(dir_line_start,  "line-start",  D_LINE_START)  /* ^ */
DIRECTIVE(dir_open_brace,  "open-brace",  D_OPEN_BRACE)  /* \o\O */
DIRECTIVE(dir_prop,        "prop",        D_PROP)        /* \p\P */
DIRECTIVE(dir_ref,         "ref",         D_REF)         /* \k\K */
DIRECTIVE(dir_ref_braced,  "ref-braced",  D_REF_BRACED)  /* \m\M */
DIRECTIVE(dir_space,       "space",       D_SPACE)       /* \s\S */
DIRECTIVE(dir_vspace,      "vspace",      D_VSPACE)      /* \v\V */
DIRECTIVE(dir_word,        "word",        D_WORD)        /* \w\W */
DIRECTIVE(dir_word_break,  "word-break",  D_WORD_BREAK)  /* \b\B */

#undef DIRECTIVE
