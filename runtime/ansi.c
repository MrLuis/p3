/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/*
 * Ansistrings: reference-counted strings of any length, with Free Pascal's
 * layout. A value is NULL (the empty string) or a pointer to NUL-terminated
 * characters preceded by a header. String constants are static data with a
 * negative count, never counted or freed.
 */
#include "pascalrt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct AnsiHeader {
  /* Placed first so refs (-16) and length (-8) keep their exact offsets for
   * static string literals, which only emit { refs = -1, length } and never
   * have capacity accessed (capacity is touched only when refs == 1). */
  int64_t capacity;
  int64_t refs;
  int64_t length;
} AnsiHeader;

_Static_assert(sizeof(AnsiHeader) == 24, "AnsiHeader layout");

#define HEADER(s) ((AnsiHeader *)(void *)((s) - sizeof(AnsiHeader)))
#define REFS_PTR(s) ((int64_t *)(void *)((s) - 16))
#define LEN(s) ((s) ? *(const int64_t *)(void *)((s) - 8) : 0)

static size_t str_alloc_size(int64_t len) {
  /* Reject negative lengths, size_t wrap, and > 2 GiB strings that would
   * silently truncate in i32-based APIs (__pas_astr_length, pos, copy). */
  if (len < 0 || len > INT32_MAX ||
      (uint64_t)len > (uint64_t)(SIZE_MAX - sizeof(AnsiHeader) - 1))
    __pas_runtime_error("out of memory in a string");
  return sizeof(AnsiHeader) + (size_t)len + 1;
}

static int64_t str_capacity(int64_t current, int64_t needed) {
  if (current < 0)
    current = 0;
  int64_t doubled = current >= INT32_MAX / 2 ? INT32_MAX : current * 2;
  int64_t cap = doubled > 15 ? doubled : 15;
  if (needed > cap)
    cap = needed;
  return cap > INT32_MAX ? INT32_MAX : cap;
}

#define ASTR_POOL_CAP 32
/* External linkage so libpascalrt.ll (available_externally) and libpascalrt.a
 * share a single pool instance instead of duplicating static globals. */
AnsiHeader *__pas_astr_pool15[ASTR_POOL_CAP] = {0};
int32_t __pas_astr_pool15_n = 0;
AnsiHeader *__pas_astr_pool39[ASTR_POOL_CAP] = {0};
int32_t __pas_astr_pool39_n = 0;

static inline void free_str_header(AnsiHeader *h) {
  if (!__pas_threads_started) {
    int64_t cap = h->capacity;
    if (cap == 15 && __pas_astr_pool15_n < ASTR_POOL_CAP) {
      __pas_astr_pool15[__pas_astr_pool15_n++] = h;
      return;
    }
    if (cap == 39 && __pas_astr_pool39_n < ASTR_POOL_CAP) {
      __pas_astr_pool39[__pas_astr_pool39_n++] = h;
      return;
    }
  }
  free(h);
}

static char *alloc_str_cap(int64_t len, int64_t cap) {
  if (cap < len)
    cap = len;
  AnsiHeader *h = NULL;
  if (!__pas_threads_started) {
    if (cap <= 15) {
      cap = 15;
      if (__pas_astr_pool15_n > 0)
        h = __pas_astr_pool15[--__pas_astr_pool15_n];
    } else if (cap <= 39) {
      cap = 39;
      if (__pas_astr_pool39_n > 0)
        h = __pas_astr_pool39[--__pas_astr_pool39_n];
    }
  }
  if (!h) {
    h = malloc(str_alloc_size(cap));
    if (!h)
      __pas_runtime_error("out of memory in a string");
    h->capacity = cap;
  }
  h->refs = 1;
  h->length = len;
  char *s = (char *)(h + 1);
  s[len] = '\0';
  return s;
}

static char *alloc_str(int64_t len) {
  return alloc_str_cap(len, len);
}

static char *from_chars_cap(const char *p, int64_t len, int64_t cap) {
  if (len <= 0)
    return NULL;
  char *s = alloc_str_cap(len, cap);
  memcpy(s, p, (size_t)len);
  return s;
}

static char *from_chars(const char *p, int64_t len) {
  return from_chars_cap(p, len, len);
}

int32_t __pas_threads_started = 0;

/* Counts change atomically once threads have started: values are then
   shared between threads (tasks); plainly before. On the single-thread
   fast path, check __pas_threads_started once instead of performing atomic
   loads or writing refs = 0 right before free(). */
void __pas_astr_addref(char *s) {
  if (!s)
    return;
  int64_t *rp = REFS_PTR(s);
  if (!__pas_threads_started) {
    int64_t r = *rp;
    if (r > 0)
      *rp = r + 1;
    return;
  }
  if (__atomic_load_n(rp, __ATOMIC_RELAXED) > 0)
    __atomic_fetch_add(rp, 1, __ATOMIC_RELAXED);
}

static void release(char *s) {
  if (!s)
    return;
  int64_t *rp = REFS_PTR(s);
  if (!__pas_threads_started) {
    int64_t r = *rp;
    if (r == 1)
      free_str_header(HEADER(s));
    else if (r > 1)
      *rp = r - 1;
    return;
  }
  if (__atomic_load_n(rp, __ATOMIC_RELAXED) > 0 &&
      __atomic_sub_fetch(rp, 1, __ATOMIC_ACQ_REL) == 0)
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
  int64_t la = LEN(a), lb = LEN(b);
  if (la < 0 || lb < 0 || la > INT32_MAX - lb)
    __pas_runtime_error("out of memory in a string");
  char *s = alloc_str(la + lb);
  memcpy(s, a, (size_t)la);
  memcpy(s + la, b, (size_t)lb);
  return s;
}

char *__pas_astr_join(const char *a, const char *p, int64_t n, const char *b) {
  int64_t la = a ? LEN(a) : 0, lb = b ? LEN(b) : 0;
  if (n < 0)
    n = 0;
  if (la < 0 || lb < 0 || n > INT32_MAX || la > INT32_MAX - n ||
      lb > INT32_MAX - (la + n))
    __pas_runtime_error("out of memory in a string");
  int64_t total = la + n + lb;
  if (total == 0)
    return NULL;
  char *s = alloc_str(total);
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

int32_t __pas_astr_eq(const char *a, const char *b) {
  if (a == b)
    return 1;
  int64_t la = LEN(a), lb = LEN(b);
  if (la != lb)
    return 0;
  return la == 0 || memcmp(a, b, (size_t)la) == 0;
}

int32_t __pas_astr_slice_compare(const char *a, int32_t la, const char *b,
                                 int32_t lb) {
  int32_t n = la < lb ? la : lb;
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
  if (s && PAS_RC_LOAD_ACQUIRE(REFS_PTR(s)) != 1) {
    char *copy = from_chars(s, LEN(s));
    release(s);
    *p = s = copy;
  }
  return s;
}

int32_t __pas_astr_bad_index(const char *s, int64_t index) {
  return index < 1 || index > LEN(s);
}

static void astr_setlength(char **p, int32_t len, int zero_fill) {
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
  if (s && PAS_RC_LOAD_ACQUIRE(REFS_PTR(s)) == 1) {
    AnsiHeader *h = HEADER(s);
    if (len <= h->capacity && (len > old || len >= (h->capacity >> 1))) {
      if (zero_fill && len > old)
        memset(s + old, 0, (size_t)(len - old));
      s[len] = '\0';
      h->length = len;
      return;
    }
    int64_t cap = len > h->capacity ? str_capacity(h->capacity, len) : len;
    AnsiHeader *nh = realloc(h, str_alloc_size(cap));
    if (!nh)
      __pas_runtime_error("out of memory in a string");
    nh->capacity = cap;
    nh->length = len;
    char *n = (char *)(nh + 1);
    if (zero_fill && len > old)
      memset(n + old, 0, (size_t)(len - old));
    n[len] = '\0';
    *p = n;
    return;
  }
  char *n = alloc_str(len);
  int64_t keep = old < len ? old : len;
  if (keep > 0)
    memcpy(n, s, (size_t)keep);
  if (zero_fill && len > keep)
    memset(n + keep, 0, (size_t)(len - keep));
  release(s);
  *p = n;
}

void __pas_astr_setlength(char **p, int32_t len) {
  astr_setlength(p, len, 1);
}

/* SetLength for a string the caller is about to fill whole: what grows past
 * the old contents is not zeroed (setlength above zeroes it, and a builder
 * that writes every byte pays for writing it twice). */
void __pas_astr_setlength_raw(char **p, int32_t len) {
  astr_setlength(p, len, 0);
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
  if (ls < 0 || n < 0 || ls > INT32_MAX - n)
    __pas_runtime_error("out of memory in a string");
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

/* Ensures *p is a writable buffer of length (*out_n + m) with terminating NUL,
 * reusing/growing *p in place when it has a single reference and does not alias
 * any input. When a fresh buffer is allocated instead, the previous *p is
 * returned in *out_old so the caller can read inputs before releasing it. */
static inline char *astr_prepare_append(char **p, int64_t m, const char *g1,
                                        const char *g2, int extra_alias,
                                        int64_t *out_n, char **out_old) {
  if (m < 0 || m > INT32_MAX)
    __pas_runtime_error("out of memory in a string");
  char *s = *p;
  if (!s) {
    char *r = alloc_str_cap(m, str_capacity(0, m));
    *p = r;
    *out_n = 0;
    *out_old = NULL;
    return r;
  }
  int64_t n = LEN(s);
  if (n < 0 || n > INT32_MAX - m)
    __pas_runtime_error("out of memory in a string");
  int64_t total = n + m;
  if (!extra_alias && s != g1 && s != g2 &&
      PAS_RC_LOAD_ACQUIRE(REFS_PTR(s)) == 1) {
    AnsiHeader *h = HEADER(s);
    if (total <= h->capacity) {
      s[total] = '\0';
      h->length = total;
      *out_n = n;
      *out_old = NULL;
      return s;
    }
    int64_t cap = str_capacity(h->capacity, total);
    AnsiHeader *nh = realloc(h, str_alloc_size(cap));
    if (!nh)
      __pas_runtime_error("out of memory in a string");
    nh->capacity = cap;
    nh->length = total;
    char *r = (char *)(nh + 1);
    r[total] = '\0';
    *p = r;
    *out_n = n;
    *out_old = NULL;
    return r;
  }
  char *r = alloc_str_cap(total, str_capacity(n, total));
  if (n > 0)
    memcpy(r, s, (size_t)n);
  *p = r;
  *out_n = n;
  *out_old = s;
  return r;
}

static inline void astr_append_bytes(char **p, const char *x, int64_t m) {
  if (m <= 0)
    return;
  int64_t n = 0;
  char *old = NULL;
  char *r = astr_prepare_append(p, m, x, NULL, 0, &n, &old);
  memcpy(r + n, x, (size_t)m);
  release(old);
}

/* s := s + x: appends to s in place when s is the only reference to its
 * characters, and as a copy otherwise. x may be s (or share its data). */
void __pas_astr_append(char **p, char *x) {
  astr_append_bytes(p, x, LEN(x));
}

void __pas_astr_append_char(char **p, char c) {
  astr_append_bytes(p, &c, 1);
}

void __pas_astr_append2(char **p, char *a, char *b) {
  int64_t la = LEN(a), lb = LEN(b);
  if (la < 0 || lb < 0 || la > INT32_MAX - lb)
    __pas_runtime_error("out of memory in a string");
  int64_t m = la + lb;
  if (m == 0)
    return;
  int64_t n = 0;
  char *old = NULL;
  char *r = astr_prepare_append(p, m, a, b, 0, &n, &old);
  if (la > 0)
    memcpy(r + n, a, (size_t)la);
  if (lb > 0)
    memcpy(r + n + la, b, (size_t)lb);
  release(old);
}

void __pas_astr_append_sc(char **p, char *x, char c) {
  int64_t lx = LEN(x);
  if (lx < 0 || lx > INT32_MAX - 1)
    __pas_runtime_error("out of memory in a string");
  int64_t n = 0;
  char *old = NULL;
  char *r = astr_prepare_append(p, lx + 1, x, NULL, 0, &n, &old);
  if (lx > 0)
    memcpy(r + n, x, (size_t)lx);
  r[n + lx] = c;
  release(old);
}

void __pas_astr_append_cs(char **p, char c, char *x) {
  int64_t lx = LEN(x);
  if (lx < 0 || lx > INT32_MAX - 1)
    __pas_runtime_error("out of memory in a string");
  int64_t n = 0;
  char *old = NULL;
  char *r = astr_prepare_append(p, 1 + lx, x, NULL, 0, &n, &old);
  r[n] = c;
  if (lx > 0)
    memcpy(r + n + 1, x, (size_t)lx);
  release(old);
}

void __pas_astr_append_n(char **p, const char *const *ptrs, const int64_t *lens,
                         int64_t count) {
  int64_t m = 0;
  char *s = *p;
  int aliased = 0;
  for (int64_t i = 0; i < count; ++i) {
    int64_t l = lens[i] < 0 ? LEN(ptrs[i]) : lens[i];
    if (l < 0 || m > INT32_MAX - l)
      __pas_runtime_error("out of memory in a string");
    m += l;
    if (s && ptrs[i] == s)
      aliased = 1;
  }
  if (m == 0)
    return;
  int64_t n = 0;
  char *old = NULL;
  char *r = astr_prepare_append(p, m, NULL, NULL, aliased, &n, &old);
  int64_t off = n;
  for (int64_t i = 0; i < count; ++i) {
    int64_t l = lens[i] < 0 ? LEN(ptrs[i]) : lens[i];
    if (l > 0) {
      memcpy(r + off, ptrs[i], (size_t)l);
      off += l;
    }
  }
  release(old);
}

static inline void astr_prepend_bytes(char **p, const char *x, int64_t m) {
  if (m <= 0)
    return;
  char *s = *p;
  if (!s) {
    *p = from_chars_cap(x, m, str_capacity(0, m));
    return;
  }
  int64_t n = LEN(s);
  if (n < 0 || n > INT32_MAX - m)
    __pas_runtime_error("out of memory in a string");
  int64_t total = n + m;
  if (s != x && PAS_RC_LOAD_ACQUIRE(REFS_PTR(s)) == 1) {
    AnsiHeader *h = HEADER(s);
    if (total <= h->capacity) {
      memmove(s + m, s, (size_t)n + 1);
      memcpy(s, x, (size_t)m);
      h->length = total;
      return;
    }
    int64_t cap = str_capacity(h->capacity, total);
    char *r = alloc_str_cap(total, cap);
    memcpy(r, x, (size_t)m);
    if (n > 0)
      memcpy(r + m, s, (size_t)n);
    free_str_header(h);
    *p = r;
    return;
  }
  char *r = alloc_str_cap(total, str_capacity(n, total));
  memcpy(r, x, (size_t)m);
  if (n > 0)
    memcpy(r + m, s, (size_t)n);
  release(s);
  *p = r;
}

/* s := x + s: prepends to s in place when s is the only reference to its
 * characters and has spare capacity, and as a copy otherwise. */
void __pas_astr_prepend(char **p, char *x) {
  astr_prepend_bytes(p, x, LEN(x));
}

void __pas_astr_prepend_char(char **p, char c) {
  astr_prepend_bytes(p, &c, 1);
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
  /* The room there is, at least the length: growth by SetLength only
   * realoccasions when it is exceeded, like a list that keeps spare
   * room. Invisible to programs (length is the language's only bound),
   * and never read past length. */
  int64_t capacity;
} DynHeader;

#define DYN_HEADER(a) ((DynHeader *)(void *)((char *)(a) - sizeof(DynHeader)))

/* The room a SetLength that has to grow asks for: doubles capacity (with a
 * small minimum of 8 for typical element sizes) to avoid quadratic copies. */
static int64_t dyn_capacity(const DynHeader *h, int64_t count) {
  int64_t room = h->capacity > INT64_MAX / 2 ? INT64_MAX : h->capacity * 2;
  if (h->info->elem_size > 0 && h->info->elem_size <= 64 && room < 8)
    room = 8;
  return count > room ? count : room;
}

static size_t dyn_size(const PasDynInfo *info, int64_t count) {
  if (count < 0 || !info || info->elem_size < 0)
    __pas_runtime_error("invalid length of a dynamic array");
  size_t esz = (size_t)info->elem_size;
  if (esz > 0 &&
      ((uint64_t)count > (uint64_t)(SIZE_MAX - sizeof(DynHeader)) / esz ||
       count > INT64_MAX / info->elem_size))
    __pas_runtime_error("out of memory in a dynamic array");
  return sizeof(DynHeader) + (size_t)count * esz;
}

#define DYN_POOL_BUCKETS 5
#define DYN_POOL_CAP 32
static const size_t dyn_bucket_sizes[DYN_POOL_BUCKETS] = {64, 128, 256, 512, 1024};
DynHeader *__pas_dyn_pool[DYN_POOL_BUCKETS][DYN_POOL_CAP] = {{0}};
int32_t __pas_dyn_pool_n[DYN_POOL_BUCKETS] = {0};

static inline void dyn_free_hdr(DynHeader *h) {
  if (!__pas_threads_started && h->info && h->info->elem_size > 0) {
    size_t bytes = (size_t)h->capacity * (size_t)h->info->elem_size;
    int b = -1;
    if (bytes == 64) b = 0;
    else if (bytes == 128) b = 1;
    else if (bytes == 256) b = 2;
    else if (bytes == 512) b = 3;
    else if (bytes == 1024) b = 4;
    if (b >= 0 && __pas_dyn_pool_n[b] < DYN_POOL_CAP) {
      __pas_dyn_pool[b][__pas_dyn_pool_n[b]++] = h;
      return;
    }
  }
  free(h);
}

static DynHeader *dyn_alloc_hdr(const PasDynInfo *info, int64_t count, int zero) {
  size_t total = dyn_size(info, count);
  size_t esz = (size_t)info->elem_size;
  size_t payload = total - sizeof(DynHeader);
  int64_t cap = count;
  DynHeader *h = NULL;
  if (!__pas_threads_started && esz > 0 && payload <= 1024) {
    int b = -1;
    if (payload <= 64) b = 0;
    else if (payload <= 128) b = 1;
    else if (payload <= 256) b = 2;
    else if (payload <= 512) b = 3;
    else b = 4;
    size_t bbytes = dyn_bucket_sizes[b];
    if (bbytes % esz == 0) {
      cap = (int64_t)(bbytes / esz);
      total = sizeof(DynHeader) + bbytes;
      if (__pas_dyn_pool_n[b] > 0)
        h = __pas_dyn_pool[b][--__pas_dyn_pool_n[b]];
    }
  }
  if (!h) {
    h = (zero && total > sizeof(DynHeader) + 1024) ? calloc(1, total) : malloc(total);
    if (!h)
      __pas_runtime_error("out of memory in a dynamic array");
    if (zero && total <= sizeof(DynHeader) + 1024 && payload > 0)
      memset(h + 1, 0, payload);
  } else if (zero && payload > 0) {
    memset(h + 1, 0, payload);
  }
  h->refs = 1;
  h->length = count;
  h->capacity = cap;
  h->info = info;
  return h;
}

void *__pas_dyn_new(const PasDynInfo *info, int64_t count) {
  if (count < 0)
    __pas_runtime_error("invalid length of a dynamic array");
  if (count == 0)
    return NULL;
  return dyn_alloc_hdr(info, count, 1) + 1;
}

int64_t __pas_dyn_length(const void *a) {
  return a ? DYN_HEADER(a)->length : 0;
}

void __pas_dyn_addref(void *a) {
  if (!a)
    return;
  DynHeader *h = DYN_HEADER(a);
  if (!__pas_threads_started) {
    h->refs += 1;
    return;
  }
  PAS_RC_INC(&h->refs);
}

static void dyn_fini(const PasDynInfo *info, char *elems, int64_t from, int64_t to) {
  if (info->fini)
    for (int64_t i = from; i < to; ++i)
      info->fini(elems + i * info->elem_size);
}

static inline void dyn_release_hdr(DynHeader *h, int pod) {
  if (!__pas_threads_started) {
    if (--h->refs != 0)
      return;
  } else {
    if (PAS_RC_DEC(&h->refs) != 0)
      return;
  }
  if (!pod)
    dyn_fini(h->info, (char *)(h + 1), 0, h->length);
  dyn_free_hdr(h);
}

static void dyn_release(void *a) {
  if (a)
    dyn_release_hdr(DYN_HEADER(a), 0);
}

void __pas_dyn_clear(void **a) {
  void *old = *a;
  if (!old)
    return;
  *a = NULL;
  dyn_release_hdr(DYN_HEADER(old), 0);
}

void __pas_dyn_clear_pod(void **a) {
  void *old = *a;
  if (!old)
    return;
  *a = NULL;
  dyn_release_hdr(DYN_HEADER(old), 1);
}

void __pas_dyn_assign(void **dst, void *src) {
  void *old = *dst;
  if (old == src)
    return;
  __pas_dyn_addref(src);
  *dst = src;
  if (old)
    dyn_release_hdr(DYN_HEADER(old), 0);
}

void __pas_dyn_assign_pod(void **dst, void *src) {
  void *old = *dst;
  if (old == src)
    return;
  __pas_dyn_addref(src);
  *dst = src;
  if (old)
    dyn_release_hdr(DYN_HEADER(old), 1);
}

void __pas_dyn_take(void **dst, void *src) {
  void *old = *dst;
  *dst = src;
  if (old)
    dyn_release_hdr(DYN_HEADER(old), 0);
}

void __pas_dyn_take_pod(void **dst, void *src) {
  void *old = *dst;
  *dst = src;
  if (old)
    dyn_release_hdr(DYN_HEADER(old), 1);
}

/* A new array with the first count elements of a (which has at least
 * that many), each counted once more. */
static void *dyn_duplicate(const void *a, int64_t from, int64_t count, int64_t size,
                           const PasDynInfo *info) {
  if (size <= 0)
    return NULL;
  DynHeader *h = dyn_alloc_hdr(info, size, 0);
  char *copy = (char *)(h + 1);
  if (count > 0)
    memcpy(copy, (const char *)a + from * info->elem_size,
           (size_t)count * (size_t)info->elem_size);
  if (size > count)
    memset(copy + (size_t)count * (size_t)info->elem_size, 0,
           (size_t)(size - count) * (size_t)info->elem_size);
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
  int64_t refs = !__pas_threads_started ? h->refs : PAS_RC_LOAD_ACQUIRE(&h->refs);
  if (refs > 1) {
    /* shared: this variable gets its own */
    *a = dyn_duplicate(old, 0, count < h->length ? count : h->length, count, info);
    dyn_release(old);
    return;
  }
  int64_t length = h->length;
  if (count == length)
    return;
  if (count > length && count <= h->capacity) {
    /* there is room: the elements take it, no reallocation (the bytes
     * between length and count were never read) */
    memset((char *)(h + 1) + length * info->elem_size, 0,
           (size_t)(count - length) * (size_t)info->elem_size);
    h->length = count;
    return;
  }
  int64_t keep = count < length ? count : length;
  dyn_fini(info, old, keep, length);
  /* A real growth past the room takes the headroom rule's room; a
   * truncation reallocs to what it keeps, as before. */
  int64_t cap = count < length ? count : dyn_capacity(h, count);
  h = realloc(h, dyn_size(info, cap));
  if (!h)
    __pas_runtime_error("out of memory in a dynamic array");
  h->length = count;
  h->capacity = cap;
  if (count > length)
    memset((char *)(h + 1) + length * info->elem_size, 0,
           (size_t)(count - length) * (size_t)info->elem_size);
  *a = h + 1;
}

void *__pas_dyn_copy(const void *a, int64_t start, int64_t count) {
  if (!a || count <= 0)
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
