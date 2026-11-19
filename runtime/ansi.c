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

int32_t __pas_threads_started = 0;

/* Counts change atomically once threads have started: values are then
   shared between threads (tasks); plainly before (PAS_RC_INC...,
   pascalrt.h). */
void __pas_astr_addref(char *s) {
  if (s && __atomic_load_n(&HEADER(s)->refs, __ATOMIC_RELAXED) > 0)
    PAS_RC_INC(&HEADER(s)->refs);
}

static void release(char *s) {
  if (s && __atomic_load_n(&HEADER(s)->refs, __ATOMIC_RELAXED) > 0 &&
      PAS_RC_DEC(&HEADER(s)->refs) == 0)
    free(HEADER(s));
}

void __pas_astr_assign(char **dst, char *src) {
  __pas_astr_addref(src);
  char *old = *dst;
  *dst = src;
  release(old);
}

/* Stores src without counting it, and releases what dst held: the reference
 * in src moves to dst (a function result, or a static string constant). */
void __pas_astr_take(char **dst, char *src) {
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

char *__pas_astr_join(const char *a, const char *p, int64_t n, const char *b) {
  int64_t la = LEN(a), lb = LEN(b);
  if (la + n + lb <= 0)
    return NULL;
  char *s = alloc_str(la + n + lb);
  if (la > 0)
    memcpy(s, a, (size_t)la);
  if (n > 0)
    memcpy(s + la, p, (size_t)n);
  if (lb > 0)
    memcpy(s + la + n, b, (size_t)lb);
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
  if (s && PAS_RC_LOAD_ACQUIRE(&HEADER(s)->refs) != 1) {
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

/* s := s + x: appends to s in place when s is the only reference to its
 * characters, and as a copy otherwise. x may be s (or share its data). */
void __pas_astr_append(char **p, char *x) {
  int64_t m = LEN(x);
  if (m <= 0)
    return;
  char *s = *p;
  if (s == x) { /* s + s: copy the characters before the old string goes */
    int64_t n = LEN(s);
    char *r = alloc_str(n + m);
    if (n > 0)
      memcpy(r, s, (size_t)n);
    memcpy(r + n, x, (size_t)m);
    release(s);
    *p = r;
    return;
  }
  if (!s) {
    *p = from_chars(x, m);
    return;
  }
  s = __pas_astr_unique(p);
  int64_t n = LEN(s);
  AnsiHeader *h =
      realloc(HEADER(s), sizeof(AnsiHeader) + (size_t)n + (size_t)m + 1);
  if (!h)
    __pas_runtime_error("out of memory in a string");
  h->length = n + m;
  char *r = (char *)(h + 1);
  memcpy(r + n, x, (size_t)m);
  r[n + m] = '\0';
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

/*===- Dynamic arrays -------------------------------------------------------===*/

typedef struct DynHeader {
  int64_t refs;
  int64_t length;
  const PasDynInfo *info;
  int64_t pad; /* keeps the elements 16-byte aligned, as malloc's block */
} DynHeader;

#define DYN_HEADER(a) ((DynHeader *)(void *)((char *)(a) - sizeof(DynHeader)))

static size_t dyn_size(const PasDynInfo *info, int64_t count) {
  return sizeof(DynHeader) + (size_t)count * (size_t)info->elem_size;
}

void *__pas_dyn_new(const PasDynInfo *info, int64_t count) {
  if (count == 0)
    return NULL;
  DynHeader *h = calloc(1, dyn_size(info, count));
  if (!h)
    __pas_runtime_error("out of memory in a dynamic array");
  h->refs = 1;
  h->length = count;
  h->info = info;
  return h + 1;
}

int64_t __pas_dyn_length(const void *a) {
  return a ? DYN_HEADER(a)->length : 0;
}

void __pas_dyn_addref(void *a) {
  if (a)
    PAS_RC_INC(&DYN_HEADER(a)->refs);
}

static void dyn_fini(const PasDynInfo *info, char *elems, int64_t from, int64_t to) {
  if (info->fini)
    for (int64_t i = from; i < to; ++i)
      info->fini(elems + i * info->elem_size);
}

static void dyn_release(void *a) {
  if (!a)
    return;
  DynHeader *h = DYN_HEADER(a);
  if (PAS_RC_DEC(&h->refs) != 0)
    return;
  dyn_fini(h->info, a, 0, h->length);
  free(h);
}

void __pas_dyn_clear(void **a) {
  void *old = *a;
  *a = NULL;
  dyn_release(old);
}

void __pas_dyn_assign(void **dst, void *src) {
  __pas_dyn_addref(src);
  void *old = *dst;
  *dst = src;
  dyn_release(old);
}

/* A new array with the first count elements of a (which has at least
 * that many), each counted once more. */
static void *dyn_duplicate(const void *a, int64_t from, int64_t count, int64_t size,
                           const PasDynInfo *info) {
  char *copy = __pas_dyn_new(info, size);
  if (count > 0)
    memcpy(copy, (const char *)a + from * info->elem_size,
           (size_t)count * (size_t)info->elem_size);
  if (info->addref)
    for (int64_t i = 0; i < count; ++i)
      info->addref(copy + i * info->elem_size);
  return copy;
}

void __pas_dyn_setlength(void **a, const PasDynInfo *info, int64_t count) {
  if (count < 0)
    __pas_runtime_error("invalid length of a dynamic array");
  void *old = *a;
  if (count == 0) {
    __pas_dyn_clear(a);
    return;
  }
  if (!old) {
    *a = __pas_dyn_new(info, count);
    return;
  }
  DynHeader *h = DYN_HEADER(old);
  if (PAS_RC_LOAD_ACQUIRE(&h->refs) > 1) {
    /* shared: this variable gets its own */
    *a = dyn_duplicate(old, 0, count < h->length ? count : h->length, count, info);
    dyn_release(old);
    return;
  }
  int64_t length = h->length;
  if (count == length)
    return;
  dyn_fini(info, old, count, length);
  h = realloc(h, dyn_size(info, count));
  if (!h)
    __pas_runtime_error("out of memory in a dynamic array");
  h->length = count;
  if (count > length)
    memset((char *)(h + 1) + length * info->elem_size, 0,
           (size_t)(count - length) * (size_t)info->elem_size);
  *a = h + 1;
}

void *__pas_dyn_copy(const void *a, int64_t start, int64_t count) {
  if (!a)
    return NULL;
  const DynHeader *h = DYN_HEADER(a);
  if (start < 0) { /* as in Free Pascal: the count starts at the given index */
    count += start;
    start = 0;
  }
  if (start >= h->length || count <= 0)
    return NULL;
  if (count > h->length - start)
    count = h->length - start;
  return dyn_duplicate(a, start, count, count, h->info);
}

int32_t __pas_dyn_bad_index(const void *a, int64_t index) {
  return index < 0 || index >= __pas_dyn_length(a);
}
