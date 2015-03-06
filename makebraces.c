/* Parse UnicodeData.txt and BidiBrackets.txt into C data
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAXPAIRS 2048

/* BidiBrackets:
 * (?comment: #[^\n]*\n )
 * (?char:[0-9A-F]+) \s*;\s* (?mapto:[0-9A-F]+) \s*;\s* (?side:[oc])
 *
 * UnicodeData:
 * (?char:[0-9A-F]+) ; (?name:[^;]*) ; (?cat:[^;]*) ;
 */

size_t
unique(void * buf, size_t nel, size_t width, int (*cmp)(const void *, const void *))
{
  char * dst = buf;
  char * src = dst + width;
  while (--nel) {
    if (cmp(dst, src) != 0) {
      dst += width;
      if (dst != src) memcpy(dst, src, width);
    }
    src += width;
  }
  dst += width;
  return (size_t)(dst - (char*)buf) / width;
}

/* ********************************************************************** */
/* Input Parsing */
/* ********************************************************************** */

static char line[4096];
static int curchar;
static int curmapto;
static int curleft;
static char * curname;

static int
readline_bidi(FILE * fp)
{
  char * p;
  for (;;) {
    if (!fgets(line, sizeof(line), fp)) return 0;
    if (isxdigit(line[0])) break;
  }
  p = line;
  curchar = (int)strtol(p, &p, 16);
  while (*p && !isxdigit(*p)) ++p;
  curmapto = (int)strtol(p, &p, 16);
  while (*p && !isalpha(*p)) ++p;
  curleft = (*p == 'o');
  return 1;
}

static int
readline_data(FILE * fp)
{
  char * p;
  for (;;) {
    if (!fgets(line, sizeof(line), fp)) return 0;
    p = line;
    while (*p != ';') ++p;
    ++p;
    while (*p != ';') ++p;
    ++p;
    if (*p == 'P' && strchr("seif", *(p + 1))) break;
  }
  p = line;
  curchar = (int)strtol(p, &p, 16);
  curname = ++p;
  while (*p != ';') ++p;
  *p = '\0';
  p += 2;
  curleft = (*p == 's' || *p == 'i');
  return 1;
}

/* ********************************************************************** */
/* Sets */
/* ********************************************************************** */

struct set_s {
  size_t len;
  int buf[MAXPAIRS];
};

static void
set_add(struct set_s * s, int val)
{
  s->buf[s->len] = val;
  if (++s->len >= MAXPAIRS) {
    fprintf(stderr, "MAXPAIRS too small\n"); exit(2);
  }
}

static int
set_sort_cmp(const void * va, const void * vb)
{
  const int * a = va;
  const int * b = vb;
  return *a - *b;
}

static void
set_sort(struct set_s * s)
{
  qsort(s->buf, s->len, sizeof(int), set_sort_cmp);
}

static void
set_unique(struct set_s * s)
{
  s->len = unique(s->buf, s->len, sizeof(int), set_sort_cmp);
}

static void
set_write(struct set_s * s, const char * name)
{
  size_t i;
  printf("static UChar32 uni_set_%s[%u] = {\n", name, (unsigned)s->len);
  for (i = 0; i < s->len; ++i) {
    printf("  0x%04X%s\n", s->buf[i], i + 1 >= s->len ? "" : ",");
  }
  printf("};\n");
  printf("#define uni_set_%s_length (%u)\n", name, (unsigned)s->len);
}

/* ********************************************************************** */
/* Maps */
/* ********************************************************************** */

struct pair_s {
  int from;
  int to;
};
struct map_s {
  size_t len;
  struct pair_s buf[MAXPAIRS];
};

static void
map_add(struct map_s * m, int from, int to)
{
  m->buf[m->len].from = from;
  m->buf[m->len].to = to;
  if (++m->len >= MAXPAIRS) {
    fprintf(stderr, "MAXPAIRS too small\n"); exit(2);
  }
}

static int
map_sort_cmp(const void * va, const void * vb)
{
  const struct pair_s * a = va;
  const struct pair_s * b = vb;
  return a->from - b->from ? a->from - b->from : a->to - b->to;
}

static void
map_sort(struct map_s * m)
{
  qsort(m->buf, m->len, sizeof(struct pair_s), map_sort_cmp);
}

static void
map_unique(struct map_s * m)
{
  m->len = unique(m->buf, m->len, sizeof(struct pair_s), map_sort_cmp);
}

static void
map_write(struct map_s * m, const char * name)
{
  size_t i;
  printf("static struct uni_map_entry_s uni_map_%s_data[%u] = {\n", name, (unsigned)m->len);
  for (i = 0; i < m->len; ++i) {
    printf("  { 0x%04X, 0x%04X }%s\n",
           m->buf[i].from, m->buf[i].to,
           i + 1 >= m->len ? "" : ",");
  }
  printf("};\n");
  printf("uni_map uni_map_%s = { uni_map_%s_data, %u };\n", name, name, (unsigned)m->len);
}

/* ********************************************************************** */
/* ********************************************************************** */

static struct set_s set_left;
static struct set_s set_right;
static struct map_s map_braces;

/* ********************************************************************** */
/* Custom Pairs */
/* ********************************************************************** */

/* Some stuff that isn't generated programmatically:
 */
static struct pair_s patch_ltor[] = {
  { 0x003C, 0x003E }, /* LESS-THAN SIGN */
  { 0x301E, 0x301D }, /* DOUBLE PRIME QUOTATION MARK (the mistake) */
  { 0x301F, 0x301D }, /* LOW DOUBLE PRIME QUOTATION MARK */
  { 0, 0 }
};
static struct pair_s patch_rtol[] = {
  { 0x003E, 0x003C }, /* GREATER-THAN SIGN */
  { 0x301D, 0x301F }, /* REVERSED DOUBLE PRIME QUOTATION MARK */
  { 0x301D, 0x301E },
  { 0, 0 }
};

static void
parse_patch(void)
{
  size_t i;
  for (i = 0; patch_ltor[i].from; ++i) {
    set_add(&set_left, patch_ltor[i].from);
    map_add(&map_braces, patch_ltor[i].from, patch_ltor[i].to);
  }
  for (i = 0; patch_rtol[i].from; ++i) {
    set_add(&set_right, patch_rtol[i].from);
    map_add(&map_braces, patch_rtol[i].from, patch_rtol[i].to);
  }
}

/* ********************************************************************** */
/* Name Matching */
/* ********************************************************************** */

struct name_pattern {
  char * name;
  size_t len;
  int primary;
  int right;
};
#define ENTRY(S,P,R)  { S, sizeof(S) - 1, P, R }
static struct name_pattern patterns[] = {
  ENTRY("RIGHT ", 1, 1),
  ENTRY("LEFT ", 1, 0),
  ENTRY("LOW-9 ", 0, 0),
  ENTRY("HIGH-REVERSED-9 ", 0, 0),
  { NULL, 0, 0, 0 }
};
#undef ENTRY

static char *
xstrdup(const char * s)
{
  size_t n = strlen(s);
  char * r = malloc((n + 1) * sizeof(char));
  if (r) strcpy(r, s);
  return r;
}

static int
xstrremove(char * src, const char * k)
{
  size_t kn;
  size_t pn;
  char * p = strstr(src, k);
  if (!p) return 0;
  kn = strlen(k);
  pn = strlen(p) + 1;
  memmove(p, p + kn, (pn - kn) * sizeof(char));
  return 1;
}

/* 'v' must be equal to or shorter than 'k' */
static int
xstrreplace(char * src, const char * k, const char * v)
{
  size_t vn;
  size_t kn;
  size_t pn;
  char * p = strstr(src, k);
  if (!p) return 0;
  vn = strlen(v);
  kn = strlen(k);
  pn = strlen(p) + 1;
  memcpy(p, v, vn * sizeof(char));
  if (vn != kn) memmove(p + vn, p + kn, (pn - kn) * sizeof(char));
  return 1;
}

static char *
clean_leftright(const char * src, struct name_pattern ** patp)
{
  struct name_pattern * pat;
  char * dst;

  for (pat = patterns; pat->name; ++pat) {
    if (strstr(src, pat->name)) break;
  }
  if (!pat->name) return NULL;

  dst = xstrdup(src);
  xstrremove(dst, pat->name);
  xstrreplace(dst, "BRAKCET", "BRACKET"); /* U+FE18 */
  xstrreplace(dst, "GREATER-THAN", "-THAN"); /* for "RIGHT ARC GREATER-THAN BRACKET" etc */
  xstrreplace(dst, "LESS-THAN", "-THAN");
  *patp = pat;
  return dst;
}

/* left/right names for later processing */
struct candidate_s {
  char * name; /* after removing "LEFT|RIGHT" */
  int right; /* true if "RIGHT" */
  int primary; /* true if "LEFT", false if synonym */
  int value;
};
struct candidate_cache_s {
  size_t len;
  struct candidate_s buf[MAXPAIRS];
};
static struct candidate_cache_s namecache;

static void
name_add_to_cache(int val, const char * name)
{
  struct candidate_s * bc;
  struct name_pattern * pat = NULL;
  char * np = clean_leftright(name, &pat);
  if (!np) return; /* not a quote/brace */

  bc = &namecache.buf[namecache.len];
  bc->right = pat->right;
  bc->primary = pat->primary;
  bc->value = val;
  bc->name = np;
  if (++namecache.len >= MAXPAIRS) {
    fprintf(stderr, "MAXPAIRS too small in name matching\n");
    exit(2);
  }
}

static int
name_finish_sort(const void * va, const void * vb)
{
  /* sort by name,right,primary,value */
  const struct candidate_s * a = va;
  const struct candidate_s * b = vb;
  int c = strcmp(a->name, b->name);
  if (!c) c = b->right - a->right;
  if (!c) c = b->primary - a->primary;
  if (!c) c = a->value - b->value;
  return c;
}

static void
finish_name_mapping(void)
{
  struct candidate_s * p;
  struct candidate_s * r = NULL;
  size_t i;

  qsort(namecache.buf, namecache.len, sizeof(struct candidate_s), name_finish_sort);

  /* sorted by name, we have [right, primary-left, other-lefts] */
  for (i = 0; i < namecache.len; ++i) {
    p = &namecache.buf[i];
/*    printf("%s %04X %s\n", p->right ? "R" : "L", p->value, p->name);*/
    if (p->right) { r = p; continue; } /* don't add RIGHT without a LEFT */
    if (!r) { fprintf(stderr, "no right brace\n"); exit(2); }
    set_add(&set_left, p->value);
    set_add(&set_right, r->value);
    map_add(&map_braces, p->value, r->value);
    map_add(&map_braces, r->value, p->value);
  }
}

/* ********************************************************************** */
/* ********************************************************************** */

int
main(void)
{
  FILE * fp;
  
  if (!(fp = fopen("BidiBrackets.txt", "r"))) {
    fprintf(stderr, "could not open BidiBrackets.txt\n"); exit(2);
  }
  while (readline_bidi(fp)) {
    set_add(curleft ? &set_left : &set_right, curchar);
    map_add(&map_braces, curchar, curmapto);
  }
  fclose(fp);

  if (!(fp = fopen("UnicodeData.txt", "r"))) {
    fprintf(stderr, "could not open UnicodeData.txt\n"); exit(2);
  }
  while (readline_data(fp)) {
    name_add_to_cache(curchar, curname);
  }
  fclose(fp);
  finish_name_mapping();

  parse_patch();

  set_sort(&set_left); set_unique(&set_left);
  set_sort(&set_right); set_unique(&set_right);
  map_sort(&map_braces); map_unique(&map_braces);
  set_write(&set_left, "left");
  set_write(&set_right, "right");
  map_write(&map_braces, "braces");
  return 0;
}
