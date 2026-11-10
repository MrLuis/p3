/*
 * Ansistrings: reference-counted strings of any length, with Free Pascal's
 * layout. A value is NULL (the empty string) or a pointer to NUL-terminated
 * characters preceded by a header. String constants are static data with a
 * negative count, never counted or freed.
 */
#include "pascalrt.h"

#include <stdlib.h>
#include <string.h>

typedef struct AnsiHeader {
  int64_t refs;
  int64_t length;
} AnsiHeader;

#define HEADER(s) ((AnsiHeader *)(void *)((s) - sizeof(AnsiHeader)))
#define LEN(s) ((s) ? HEADER(s)->length : 0)

static char *alloc_str(int64_t len) {
  AnsiHeader *h = malloc(sizeof(AnsiHeader) + (size_t)len + 1);
  if (!h)
    __pas_runtime_error("out of memory in a string");
  h->refs = 1;
  h->length = len;
  char *s = (char *)(h + 1);
  s[len] = '\0';
  return s;
}

static char *from_chars(const char *p, int64_t len) {
  if (len <= 0)
    return NULL;
  char *s = alloc_str(len);
  memcpy(s, p, (size_t)len);
  return s;
}

void __pas_astr_addref(char *s) {
  if (s && HEADER(s)->refs > 0)
    HEADER(s)->refs++;
}

static void release(char *s) {
  if (s && HEADER(s)->refs > 0 && --HEADER(s)->refs == 0)
    free(HEADER(s));
}

void __pas_astr_assign(char **dst, char *src) {
  __pas_astr_addref(src);
  char *old = *dst;
  *dst = src;
  release(old);
}

void __pas_astr_clear(char **p) {
  char *old = *p;
  *p = NULL;
  release(old);
}

int32_t __pas_astr_length(const char *s) { return (int32_t)LEN(s); }

char *__pas_astr_from_chars(const char *p, int64_t len) {
  return from_chars(p, len);
}

char *__pas_astr_from_pstr(const unsigned char *p) {
  return from_chars((const char *)p + 1, p[0]);
}

char *__pas_astr_from_char(char c) { return from_chars(&c, 1); }

char *__pas_astr_from_cstr(const char *p) {
  return p ? from_chars(p, (int64_t)strlen(p)) : NULL;
}

void __pas_astr_to_pstr(unsigned char *dst, int32_t maxlen, const char *s) {
  int64_t n = LEN(s);
  if (n > maxlen)
    n = maxlen;
  if (n > 0)
    memcpy(dst + 1, s, (size_t)n);
  dst[0] = (unsigned char)n;
}

const char *__pas_astr_pchar(const char *s) { return s ? s : ""; }

char *__pas_astr_concat(char *a, char *b) {
  if (!a || !b) {
    char *only = a ? a : b;
    __pas_astr_addref(only);
    return only;
  }
  char *s = alloc_str(LEN(a) + LEN(b));
  memcpy(s, a, (size_t)LEN(a));
  memcpy(s + LEN(a), b, (size_t)LEN(b));
  return s;
}

int32_t __pas_astr_compare(const char *a, const char *b) {
  int64_t la = LEN(a), lb = LEN(b), n = la < lb ? la : lb;
  int c = n > 0 ? memcmp(a, b, (size_t)n) : 0;
  if (c != 0)
    return c < 0 ? -1 : 1;
  return la < lb ? -1 : la > lb;
}

int32_t __pas_astr_pos(const char *sub, const char *s) {
  int64_t ls = LEN(sub), n = LEN(s);
  if (ls == 0)
    return 0;
  for (int64_t i = 0; i + ls <= n; i++)
    if (s[i] == sub[0] && memcmp(s + i, sub, (size_t)ls) == 0)
      return (int32_t)(i + 1);
  return 0;
}

char *__pas_astr_copy(char *s, int32_t index, int32_t count) {
  int64_t n = LEN(s);
  if (index < 1)
    index = 1;
  if (count <= 0 || index > n)
    return NULL;
  int64_t c = count;
  if (c > n - index + 1)
    c = n - index + 1;
  if (index == 1 && c == n) {
    __pas_astr_addref(s);
    return s;
  }
  return from_chars(s + index - 1, c);
}

/* Makes *p the only reference to its characters; returns them. */
char *__pas_astr_unique(char **p) {
  char *s = *p;
  if (s && HEADER(s)->refs != 1) {
    char *copy = from_chars(s, LEN(s));
    release(s);
    *p = s = copy;
  }
  return s;
}

int32_t __pas_astr_bad_index(const char *s, int64_t index) {
  return index < 1 || index > LEN(s);
}

void __pas_astr_setlength(char **p, int32_t len) {
  char *s = *p;
  int64_t old = LEN(s);
  if (len <= 0) {
    __pas_astr_clear(p);
    return;
  }
  if (len == old) {
    __pas_astr_unique(p);
    return;
  }
  char *n = alloc_str(len);
  int64_t keep = old < len ? old : len;
  if (keep > 0)
    memcpy(n, s, (size_t)keep);
  if (len > keep)
    memset(n + keep, 0, (size_t)(len - keep));
  release(s);
  *p = n;
}

void __pas_astr_delete(char **p, int32_t index, int32_t count) {
  int64_t n = LEN(*p);
  if (index < 1 || index > n || count <= 0)
    return;
  int64_t c = count;
  if (c > n - index + 1)
    c = n - index + 1;
  char *r = c == n ? NULL : alloc_str(n - c);
  if (r) {
    memcpy(r, *p, (size_t)index - 1);
    memcpy(r + index - 1, *p + index - 1 + c, (size_t)(n - index + 1 - c));
  }
  release(*p);
  *p = r;
}

void __pas_astr_insert(char *src, char **p, int32_t index) {
  int64_t n = LEN(*p), ls = LEN(src);
  if (ls == 0)
    return;
  if (index < 1)
    index = 1;
  if (index > n + 1)
    index = (int32_t)n + 1;
  char *r = alloc_str(n + ls);
  if (index > 1)
    memcpy(r, *p, (size_t)index - 1);
  memcpy(r + index - 1, src, (size_t)ls);
  if (index <= n)
    memcpy(r + index - 1 + ls, *p + index - 1, (size_t)(n - index + 1));
  release(*p);
  *p = r;
}

/* setlength of a short string. */
void __pas_str_setlength(unsigned char *s, int32_t maxlen, int32_t len) {
  if (len < 0)
    len = 0;
  if (len > maxlen)
    len = maxlen;
  if (len > s[0])
    memset(s + 1 + s[0], 0, (size_t)(len - s[0]));
  s[0] = (unsigned char)len;
}
