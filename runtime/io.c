/*===- io.c - Files, text I/O and heap for Pascal programs -------------------===
 *
 * ISO 7185 file model: each file has a mode (undefined / inspection /
 * generation) and a buffer variable f^ holding the current component.
 * Inspection is lazy: the next component is fetched only when the program
 * looks at f^, eof or eoln, so prompts and interactive input interleave
 * correctly.
 *
 * Text files: a line end is presented as f^ = ' ' with eoln(f) = true. A
 * final line without a terminating newline gets an implicit one.
 *
 *===------------------------------------------------------------------------===*/

#include "pascalrt.h"

#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#ifdef _WIN32
/* Windows has no _longjmp, and its _setjmp takes a second argument and a
 * 16-byte aligned buffer: the runtime has its own pair (sys.c), which
 * compiled code links to under the usual names. */
void _longjmp(jmp_buf env, int value) __attribute__((noreturn));
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODE_UNDEFINED, MODE_INSPECT, MODE_GENERATE };

struct PasFile {
  FILE *fp;
  const char *name;      /* external name; NULL for internal files */
  int mode;
  int is_text;
  int is_std;            /* input / output: never closed */
  int64_t elem_size;
  unsigned char *buf;    /* buffer variable f^ */
  int buf_full;          /* inspection: buf holds the next component */
  int at_eof;
  int eoln;              /* text: buf is a line end */
  int line_has_chars;    /* text: chars read since the last line end */
  struct PasFile *next;  /* all files, for flushing at exit */
};

/* The runtime's variables are not static: its code is also linked into
 * optimized programs as LLVM IR (libpascalrt.ll) and inlined there, and
 * must then use the same variables as the library's copy. */
struct PasFile __pas_std_input, __pas_std_output;
PasFile *__pas_input = &__pas_std_input;
PasFile *__pas_output = &__pas_std_output;
struct PasFile *__pas_all_files;
/* Set by main() in --mode=tp: field widths never truncate, and booleans
 * print as TRUE/FALSE (Turbo/FPC conventions). */
int32_t __pas_tp_mode;
int __pas_prog_argc;
char **__pas_prog_argv;

void __pas_runtime_error(const char *msg) {
  fflush(stdout);
  fprintf(stderr, "Runtime error: %s\n", msg);
  exit(2);
}

static void file_error(const PasFile *f, const char *what) {
  char msg[512];
  snprintf(msg, sizeof msg, "%s (file '%s')", what,
           f->name             ? f->name
           : f == &__pas_std_input   ? "input"
           : f == &__pas_std_output  ? "output"
                               : "<internal>");
  __pas_runtime_error(msg);
}

unsigned char __pas_std_buf_in, __pas_std_buf_out;

void __pas_init(int32_t argc, char **argv) {
  __pas_prog_argc = argc;
  __pas_prog_argv = argv;
  __pas_std_input = (struct PasFile){.fp = stdin, .mode = MODE_INSPECT, .is_text = 1,
                               .is_std = 1, .elem_size = 1, .buf = &__pas_std_buf_in};
  __pas_std_output = (struct PasFile){.fp = stdout, .mode = MODE_GENERATE,
                                .is_text = 1, .is_std = 1, .elem_size = 1,
                                .buf = &__pas_std_buf_out};
}

void __pas_fini(void) {
  for (struct PasFile *f = __pas_all_files; f; f = f->next)
    if (f->fp && f->mode == MODE_GENERATE)
      fflush(f->fp);
  if (fflush(stdout) != 0 || ferror(stdout)) {
    fputs("Runtime error: error writing to output\n", stderr);
    exit(1);
  }
}

/*===- File variables -------------------------------------------------------===*/

PasFile *__pas_file_new(int64_t elem_size, int32_t is_text, const char *name) {
  struct PasFile *f = calloc(1, sizeof *f);
  if (!f)
    __pas_runtime_error("out of memory creating a file variable");
  f->elem_size = elem_size > 0 ? elem_size : 1;
  f->is_text = is_text;
  f->name = name;
  f->buf = calloc(1, (size_t)f->elem_size);
  f->next = __pas_all_files;
  __pas_all_files = f;
  return f;
}

void __pas_file_free(PasFile *f) {
  if (!f || f->is_std)
    return;
  for (struct PasFile **p = &__pas_all_files; *p; p = &(*p)->next)
    if (*p == f) {
      *p = f->next;
      break;
    }
  if (f->fp)
    fclose(f->fp);
  free(f->buf);
  free(f);
}

const char *__pas_program_param(int32_t index, const char *name) {
  return index < __pas_prog_argc ? __pas_prog_argv[index] : name;
}

void __pas_reset(PasFile *f) {
  if (f->is_std) {
    if (f != &__pas_std_input)
      file_error(f, "'reset' of the standard output file");
    return;
  }
  if (f->name) {
    if (f->fp)
      fclose(f->fp);
    f->fp = fopen(f->name, "rb");
    if (!f->fp)
      file_error(f, "cannot open file for reading");
  } else {
    if (!f->fp)
      file_error(f, "'reset' of a file that was never written");
    fflush(f->fp);
    rewind(f->fp);
  }
  f->mode = MODE_INSPECT;
  f->buf_full = f->at_eof = f->eoln = f->line_has_chars = 0;
}

void __pas_rewrite(PasFile *f) {
  if (f->is_std) {
    if (f != &__pas_std_output)
      file_error(f, "'rewrite' of the standard input file");
    return;
  }
  if (f->fp)
    fclose(f->fp);
  f->fp = f->name ? fopen(f->name, "wb") : tmpfile();
  if (!f->fp)
    file_error(f, "cannot open file for writing");
  f->mode = MODE_GENERATE;
  f->at_eof = 1; /* ISO: eof is true while generating */
  f->buf_full = f->eoln = f->line_has_chars = 0;
}

static void need_inspect(PasFile *f) {
  if (f->mode != MODE_INSPECT)
    file_error(f, "file is not open for reading (use 'reset')");
}

static void need_generate(PasFile *f) {
  if (f->mode != MODE_GENERATE)
    file_error(f, "file is not open for writing (use 'rewrite')");
}

/* Fills the buffer variable with the next component, if not done yet. */
static void fill(PasFile *f) {
  if (f->buf_full || f->at_eof)
    return;
  if (!f->is_text) {
    if (fread(f->buf, (size_t)f->elem_size, 1, f->fp) != 1)
      f->at_eof = 1;
    f->buf_full = 1;
    return;
  }
  int c = getc(f->fp);
  if (c == '\r') {
    int d = getc(f->fp);
    if (d == '\n')
      c = '\n';
    else if (d != EOF)
      ungetc(d, f->fp);
  }
  if (c == EOF) {
    if (f->line_has_chars) {
      /* Supply the missing final line end. */
      f->line_has_chars = 0;
      f->eoln = 1;
      f->buf[0] = ' ';
      f->buf_full = 1;
      return;
    }
    f->at_eof = 1;
    f->eoln = 0;
    f->buf[0] = ' ';
    f->buf_full = 1;
    return;
  }
  f->eoln = c == '\n';
  f->line_has_chars = !f->eoln;
  f->buf[0] = f->eoln ? ' ' : (unsigned char)c;
  f->buf_full = 1;
}

void *__pas_buffer(PasFile *f) {
  if (f->mode == MODE_INSPECT)
    fill(f);
  return f->buf;
}

int32_t __pas_eof(PasFile *f) {
  if (f->mode == MODE_GENERATE)
    return 1;
  need_inspect(f);
  fill(f);
  return f->at_eof;
}

int32_t __pas_eoln(PasFile *f) {
  need_inspect(f);
  if (!f->is_text)
    file_error(f, "'eoln' of a non-text file");
  fill(f);
  if (f->at_eof)
    file_error(f, "'eoln' at end of file");
  return f->eoln;
}

void __pas_get(PasFile *f) {
  need_inspect(f);
  fill(f);
  if (f->at_eof)
    file_error(f, "'get' past the end of the file");
  f->buf_full = 0;
}

void __pas_put(PasFile *f) {
  need_generate(f);
  if (fwrite(f->buf, (size_t)f->elem_size, 1, f->fp) != 1)
    file_error(f, "error writing file");
}

void __pas_page(PasFile *f) {
  need_generate(f);
  putc('\f', f->fp);
}

void __pas_read_elem(PasFile *f, void *dst) {
  need_inspect(f);
  fill(f);
  if (f->at_eof)
    file_error(f, "'read' past the end of the file");
  memcpy(dst, f->buf, (size_t)f->elem_size);
  f->buf_full = 0;
}

void __pas_write_elem(PasFile *f, const void *src) {
  need_generate(f);
  memcpy(f->buf, src, (size_t)f->elem_size);
  __pas_put(f);
}

/*===- Text output ----------------------------------------------------------===*/

static void pad(PasFile *f, int32_t n) {
  for (; n > 0; --n)
    putc(' ', f->fp);
}

void __pas_write_str(PasFile *f, const char *s, int64_t len, int32_t width) {
  need_generate(f);
  if (width > 0) {
    if (len > width) {
      if (!__pas_tp_mode)
        len = width; /* ISO 7185: truncate */
    } else {
      pad(f, width - (int32_t)len);
    }
  }
  fwrite(s, 1, (size_t)len, f->fp);
}

void __pas_write_char(PasFile *f, char c, int32_t width) {
  need_generate(f);
  pad(f, width - 1);
  putc((unsigned char)c, f->fp);
}

void __pas_write_int(PasFile *f, int64_t v, int32_t width) {
  need_generate(f);
  char buf[24];
  int n = snprintf(buf, sizeof buf, "%" PRId64, v);
  pad(f, width - n); /* integers are never truncated */
  fwrite(buf, 1, (size_t)n, f->fp);
}

void __pas_write_uint(PasFile *f, uint64_t v, int32_t width) {
  need_generate(f);
  char buf[24];
  int n = snprintf(buf, sizeof buf, "%" PRIu64, v);
  pad(f, width - n);
  fwrite(buf, 1, (size_t)n, f->fp);
}

void __pas_write_bool(PasFile *f, int32_t b, int32_t width) {
  if (b)
    __pas_write_str(f, __pas_tp_mode ? "TRUE" : "true", 4, width);
  else
    __pas_write_str(f, __pas_tp_mode ? "FALSE" : "false", 5, width);
}

void __pas_write_real(PasFile *f, double x, int32_t width, int32_t frac) {
  need_generate(f);
  char buf[512];
  int n;
  if (isnan(x) || isinf(x)) {
    const char *s = isnan(x) ? "NaN" : (x < 0 ? "-Inf" : " Inf");
    __pas_write_str(f, s, (int64_t)strlen(s), width);
    return;
  }
  if (frac >= 0) {
    /* Fixed-point: [-]digits.fraction, right-aligned in `width`. */
    n = snprintf(buf, sizeof buf, "%.*f", frac > 300 ? 300 : frac, x);
  } else {
    /* Floating-point: a sign position (' ' or '-'), one digit, '.',
     * DecPlaces digits, 'E', exponent sign, 3 exponent digits;
     * DecPlaces = max(width, 9) - 3 - 5. Default width is 22. */
    int total = width > 0 ? width : 22;
    if (total < 9)
      total = 9;
    int dec = total - 3 - 5;
    if (dec > 300)
      dec = 300;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%.*E", dec, fabs(x));
    char *e = strchr(tmp, 'E');
    int exponent = atoi(e + 1);
    *e = '\0';
    n = snprintf(buf, sizeof buf, "%c%sE%c%03d", signbit(x) ? '-' : ' ', tmp,
                 exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
  }
  if (n < 0)
    n = 0;
  if (n > (int)sizeof buf - 1)
    n = (int)sizeof buf - 1;
  pad(f, width - n); /* reals are never truncated */
  fwrite(buf, 1, (size_t)n, f->fp);
}

void __pas_writeln(PasFile *f) {
  need_generate(f);
  putc('\n', f->fp);
}

/*===- Text input -----------------------------------------------------------===*/

/* Current char (after fill), or -1 at end of file. Line ends read as ' '. */
static int peek(PasFile *f) {
  fill(f);
  return f->at_eof ? -1 : f->buf[0];
}

static void advance(PasFile *f) { f->buf_full = 0; }

static void skip_blanks(PasFile *f, const char *what) {
  for (;;) {
    int c = peek(f);
    if (c < 0) {
      char msg[128];
      snprintf(msg, sizeof msg, "unexpected end of file while reading %s", what);
      file_error(f, msg);
    }
    if (c != ' ' && c != '\t' && c != '\f' && c != '\v' && !f->eoln)
      return;
    advance(f);
  }
}

/* Reads an integer whose magnitude is at most limit (+1 if negative). */
static int64_t read_integer(PasFile *f, uint64_t limit) {
  need_inspect(f);
  skip_blanks(f, "an integer");
  int negative = 0;
  int c = peek(f);
  if (c == '+' || c == '-') {
    negative = c == '-';
    advance(f);
    c = peek(f);
  }
  if (c < '0' || c > '9' || f->eoln)
    file_error(f, "invalid integer in input");
  uint64_t value = 0;
  while (c >= '0' && c <= '9' && !f->eoln) {
    unsigned d = (unsigned)(c - '0');
    if (value > (limit + (uint64_t)negative - d) / 10)
      file_error(f, "integer in input is out of range");
    value = value * 10 + d;
    advance(f);
    c = peek(f);
  }
  return negative ? (int64_t)(0 - value) : (int64_t)value;
}

int32_t __pas_read_int(PasFile *f) {
  return (int32_t)read_integer(f, INT32_MAX);
}

int64_t __pas_read_int64(PasFile *f) {
  return read_integer(f, INT64_MAX);
}

uint64_t __pas_read_uint64(PasFile *f) {
  need_inspect(f);
  skip_blanks(f, "an integer");
  int c = peek(f);
  if (c == '+') {
    advance(f);
    c = peek(f);
  }
  if (c < '0' || c > '9' || f->eoln)
    file_error(f, c == '-' ? "integer in input is out of range"
                           : "invalid integer in input");
  uint64_t value = 0;
  while (c >= '0' && c <= '9' && !f->eoln) {
    unsigned d = (unsigned)(c - '0');
    if (value > (UINT64_MAX - d) / 10)
      file_error(f, "integer in input is out of range");
    value = value * 10 + d;
    advance(f);
    c = peek(f);
  }
  return value;
}

double __pas_read_real(PasFile *f) {
  need_inspect(f);
  skip_blanks(f, "a real");
  char buf[128];
  size_t n = 0;
  int digits = 0;
  int c = peek(f);
#define TAKE()                                                                 \
  do {                                                                         \
    if (n < sizeof buf - 1)                                                    \
      buf[n++] = (char)c;                                                      \
    advance(f);                                                                \
    c = peek(f);                                                               \
  } while (0)
#define IS_DIGIT() (c >= '0' && c <= '9' && !f->eoln)
  if (c == '+' || c == '-')
    TAKE();
  while (IS_DIGIT()) {
    digits++;
    TAKE();
  }
  if (c == '.' && !f->eoln) {
    TAKE();
    while (IS_DIGIT()) {
      digits++;
      TAKE();
    }
  }
  if (digits && (c == 'e' || c == 'E') && !f->eoln) {
    TAKE();
    if ((c == '+' || c == '-') && !f->eoln)
      TAKE();
    while (IS_DIGIT())
      TAKE();
  }
#undef TAKE
#undef IS_DIGIT
  buf[n] = '\0';
  if (!digits)
    file_error(f, "invalid real number in input");
  return strtod(buf, NULL);
}

char __pas_read_char(PasFile *f) {
  need_inspect(f);
  int c = peek(f);
  if (c < 0)
    file_error(f, "unexpected end of file while reading a char");
  advance(f);
  return (char)c;
}

void __pas_readln(PasFile *f) {
  need_inspect(f);
  for (;;) {
    int c = peek(f);
    if (c < 0)
      return; /* lenient: readln at end of file does nothing */
    int was_eoln = f->eoln;
    advance(f);
    if (was_eoln)
      return;
  }
}

/*===- Heap -----------------------------------------------------------------===*/

void *__pas_new(int64_t size) {
  void *p = calloc(1, size > 0 ? (size_t)size : 1);
  if (!p)
    __pas_runtime_error("out of memory in 'new'");
  return p;
}

void __pas_dispose(void *p) {
  if (!p)
    __pas_runtime_error("'dispose' of a nil pointer");
  free(p);
}

void *__pas_realloc(void *p, int64_t size) {
  if (size <= 0) {
    free(p);
    return NULL;
  }
  void *q = realloc(p, (size_t)size);
  if (!q)
    __pas_runtime_error("out of memory in 'reallocmem'");
  return q;
}

/*===- Classes (--mode=objfpc) ---------------------------------------------===*/

void __pas_tobject_destroy(void *link, void *self) {
  (void)link;
  (void)self;
}

void *const __pas_tobject_vmt[5] = {
    NULL, (void *)"\007TObject", (void *)(uintptr_t)sizeof(void *), NULL,
    (void *)(uintptr_t)__pas_tobject_destroy};

/* TInterfacedObject: TObject, then FRefCount; implements IUnknown. */
struct PasInterfacedObject {
  void *const *vmt;
  int32_t refcount;
};
const unsigned char __pas_iunknown_iid[] = "\010IUnknown";
static void *const iunknown_methods[1] = {NULL};
static void *const interfaced_intfs[4] = {
    (void *)(uintptr_t)1, NULL, (void *)__pas_iunknown_iid,
    (void *)iunknown_methods};
void *const __pas_tinterfacedobject_vmt[5] = {
    (void *)__pas_tobject_vmt, (void *)"\021TInterfacedObject",
    (void *)(uintptr_t)sizeof(struct PasInterfacedObject),
    (void *)interfaced_intfs, (void *)(uintptr_t)__pas_tobject_destroy};

static int32_t *refcount_of(void *obj) {
  return &((struct PasInterfacedObject *)obj)->refcount;
}

void __pas_intf_addref(void *obj) {
  if (obj)
    ++*refcount_of(obj);
}

void __pas_intf_release(void *obj) {
  if (obj && --*refcount_of(obj) == 0)
    __pas_free_object(obj);
}

void __pas_intf_assign(PasIntf *dst, void *obj, void *methods) {
  __pas_intf_addref(obj);
  void *old = dst->obj;
  dst->obj = obj;
  dst->methods = methods;
  __pas_intf_release(old);
}

void __pas_intf_clear(PasIntf *p) {
  void *old = p->obj;
  p->obj = NULL;
  p->methods = NULL;
  __pas_intf_release(old);
}

void __pas_interfaced_construct(void *obj) { *refcount_of(obj) = 1; }

void __pas_interfaced_constructed(void *obj) { --*refcount_of(obj); }

void __pas_free_instance(void *obj) {
  for (void *const *vmt = *(void *const *const *)obj; vmt;
       vmt = (void *const *)vmt[0]) {
    void *const *intfs = (void *const *)vmt[3];
    if (intfs && intfs[1])
      ((void (*)(void *))intfs[1])(obj);
  }
  free(obj);
}

void *__pas_intf_lookup(const void *obj, const void *iid) {
  if (!obj)
    return NULL;
  void *const *table = (void *const *)(*(void *const *const *)obj)[3];
  if (!table)
    return NULL;
  uintptr_t n = (uintptr_t)table[0];
  for (uintptr_t i = 0; i < n; ++i)
    if (table[2 + 2 * i] == iid)
      return table[3 + 2 * i];
  return NULL;
}

void *__pas_intf_as(const void *obj, const void *iid, const char *msg) {
  void *t = __pas_intf_lookup(obj, iid);
  if (obj && !t)
    __pas_runtime_error(msg);
  return t;
}

int32_t __pas_class_inherits(const void *cls, const void *vmt) {
  for (void *const *v = cls; v; v = (void *const *)v[0])
    if (v == vmt)
      return 1;
  return 0;
}

int32_t __pas_class_is(const void *obj, const void *vmt) {
  return obj && __pas_class_inherits(*(void *const *)obj, vmt);
}

void *__pas_class_as(void *obj, const void *vmt, const char *msg) {
  if (obj && !__pas_class_is(obj, vmt))
    __pas_runtime_error(msg);
  return obj;
}

void __pas_abstract_error(void) {
  __pas_runtime_error("call of an abstract method");
}

/*===- Exceptions (--mode=objfpc) ------------------------------------------===
 *
 * Every try statement (and every exception handler while it runs) has a
 * PasExcFrame in its routine's stack frame, linked into a stack whose top
 * is `__pas_exc_top`. The compiled code pushes the frame, calls _setjmp on its
 * buffer and pops it when the protected statements end. A raise pops the
 * top frame, stores the exception in it and _longjmps there.
 */

_Static_assert(offsetof(PasExcFrame, buf) == 3 * sizeof(void *),
               "the compiler addresses the jmp_buf at this offset");
_Static_assert(sizeof(jmp_buf) <= sizeof(((PasExcFrame *)0)->buf),
               "PasExcFrame.buf is too small for this C library's jmp_buf");

PasExcFrame *__pas_exc_top;

/* An Exception instance: the VMT pointer, then Message (a short string). */
struct PasExceptionObject {
  void *const *vmt;
  unsigned char message[256];
};

#define EXCEPTION_VMT(name, parent, text)                                      \
  void *const name[5] = {(void *)(parent), (void *)(text),                     \
                         (void *)(uintptr_t)sizeof(struct PasExceptionObject), \
                         NULL, (void *)(uintptr_t)__pas_tobject_destroy}
EXCEPTION_VMT(__pas_exception_vmt, __pas_tobject_vmt, "\011Exception");
EXCEPTION_VMT(__pas_eexternal_vmt, __pas_exception_vmt, "\011EExternal");
EXCEPTION_VMT(__pas_einterror_vmt, __pas_eexternal_vmt, "\011EIntError");
EXCEPTION_VMT(__pas_edivbyzero_vmt, __pas_einterror_vmt, "\012EDivByZero");
EXCEPTION_VMT(__pas_econverterror_vmt, __pas_exception_vmt, "\015EConvertError");

void __pas_exc_push(PasExcFrame *f) {
  f->prev = __pas_exc_top;
  __pas_exc_top = f;
}

void __pas_exc_pop(PasExcFrame *f) { __pas_exc_top = f->prev; }

/* "Runtime error: unhandled exception <class> at <where>[: <message>]" */
static void unhandled(void *obj, const char *where) {
  const unsigned char *name = (*(const unsigned char *const *const *)obj)[1];
  char msg[700];
  int n = snprintf(msg, sizeof msg, "unhandled exception %.*s", name[0],
                   (const char *)name + 1);
  if (where && *where)
    n += snprintf(msg + n, sizeof msg - (size_t)n, " at %s", where);
  if (__pas_class_is(obj, __pas_exception_vmt)) {
    const unsigned char *text = (const unsigned char *)obj + sizeof(void *);
    snprintf(msg + n, sizeof msg - (size_t)n, ": %.*s", text[0],
             (const char *)text + 1);
  }
  __pas_runtime_error(msg);
}

void __pas_raise(void *obj, const char *where) {
  if (!obj) {
    char msg[300];
    snprintf(msg, sizeof msg, "'raise' of a nil object%s%s",
             where && *where ? " at " : "", where ? where : "");
    __pas_runtime_error(msg);
  }
  PasExcFrame *f = __pas_exc_top;
  if (!f)
    unhandled(obj, where);
  __pas_exc_top = f->prev;
  f->object = obj;
  f->where = where;
  _longjmp(*(jmp_buf *)(void *)f->buf, 1);
}

void __pas_reraise(PasExcFrame *f) { __pas_raise(f->object, f->where); }

void __pas_raise_divzero(const char *where) {
  static const char text[] = "Division by zero";
  struct PasExceptionObject *e = __pas_new(sizeof *e);
  e->vmt = __pas_edivbyzero_vmt;
  e->message[0] = sizeof text - 1;
  memcpy(e->message + 1, text, sizeof text - 1);
  __pas_raise(e, where);
}

void __pas_raise_convert(char *msg, size_t n) {
  struct PasExceptionObject *e = __pas_new(sizeof *e);
  e->vmt = __pas_econverterror_vmt;
  if (n > 255)
    n = 255;
  e->message[0] = (unsigned char)n;
  memcpy(e->message + 1, msg, n);
  free(msg);
  __pas_raise(e, NULL);
}

void __pas_free_object(void *obj) {
  void (*destroy)(void *, void *) =
      (void (*)(void *, void *))(*(void *const *const *)obj)[4];
  destroy(NULL, obj);
  __pas_free_instance(obj);
}

void __pas_reraise_free(PasExcFrame *f, void *obj) {
  if (obj != f->object)
    __pas_free_object(obj);
  __pas_reraise(f);
}

/*===- Program (Turbo/FPC) --------------------------------------------------===*/

void __pas_halt(int32_t code) {
  __pas_fini();
  exit(code);
}

int32_t __pas_paramcount(void) { return __pas_prog_argc > 0 ? __pas_prog_argc - 1 : 0; }

void __pas_paramstr(unsigned char *dst, int32_t index) {
  const char *s = index >= 0 && index < __pas_prog_argc ? __pas_prog_argv[index] : "";
  size_t n = strlen(s);
  if (n > 255)
    n = 255;
  dst[0] = (unsigned char)n;
  memcpy(dst + 1, s, n);
}

/*===- Short strings --------------------------------------------------------===*/

void __pas_str_assign(unsigned char *dst, int32_t maxlen, const unsigned char *src) {
  int32_t n = src[0] < maxlen ? src[0] : maxlen;
  memmove(dst + 1, src + 1, (size_t)n); /* src may overlap dst */
  dst[0] = (unsigned char)n;
}

void __pas_str_concat(unsigned char *dst, const unsigned char *a, const unsigned char *b) {
  unsigned char tmp[256];
  int32_t na = a[0], nb = b[0];
  if (na + nb > 255)
    nb = 255 - na;
  memcpy(tmp + 1, a + 1, (size_t)na);
  memcpy(tmp + 1 + na, b + 1, (size_t)nb);
  tmp[0] = (unsigned char)(na + nb);
  memcpy(dst, tmp, (size_t)tmp[0] + 1);
}

int32_t __pas_str_compare(const unsigned char *a, const unsigned char *b) {
  int32_t n = a[0] < b[0] ? a[0] : b[0];
  int c = memcmp(a + 1, b + 1, (size_t)n);
  if (c)
    return c < 0 ? -1 : 1;
  return a[0] < b[0] ? -1 : a[0] > b[0];
}

void __pas_str_copy(unsigned char *dst, const unsigned char *s, int32_t index,
                    int32_t count) {
  /* Turbo semantics: out-of-range positions yield what is available. */
  if (index < 1)
    index = 1;
  if (count < 0 || index > s[0])
    count = 0;
  else if (count > s[0] - index + 1)
    count = s[0] - index + 1;
  memmove(dst + 1, s + index, (size_t)count);
  dst[0] = (unsigned char)count;
}

int32_t __pas_str_pos(const unsigned char *sub, const unsigned char *s) {
  if (sub[0] == 0)
    return 0;
  for (int32_t i = 1; i + sub[0] - 1 <= s[0]; ++i)
    if (memcmp(s + i, sub + 1, sub[0]) == 0)
      return i;
  return 0;
}

void __pas_str_append(unsigned char *s, int32_t maxlen, const unsigned char *src) {
  int32_t n = s[0], m = src[0];
  if (m > maxlen - n)
    m = maxlen > n ? maxlen - n : 0;
  memmove(s + 1 + n, src + 1, (size_t)m); /* src may be s */
  s[0] = (unsigned char)(n + m);
}

void __pas_str_delete(unsigned char *s, int32_t index, int32_t count) {
  if (index < 1 || index > s[0] || count <= 0)
    return;
  if (count > s[0] - index + 1)
    count = s[0] - index + 1;
  memmove(s + index, s + index + count, (size_t)(s[0] - index - count + 1));
  s[0] = (unsigned char)(s[0] - count);
}

void __pas_str_insert(const unsigned char *src, unsigned char *s, int32_t maxlen,
                      int32_t index) {
  unsigned char tmp[512];
  if (index < 1)
    index = 1;
  if (index > s[0] + 1)
    index = s[0] + 1;
  int32_t head = index - 1, tail = s[0] - head;
  memcpy(tmp, s + 1, (size_t)head);
  memcpy(tmp + head, src + 1, src[0]);
  memcpy(tmp + head + src[0], s + index, (size_t)tail);
  int32_t n = head + src[0] + tail;
  if (n > maxlen)
    n = maxlen;
  memcpy(s + 1, tmp, (size_t)n);
  s[0] = (unsigned char)n;
}

void __pas_write_pstr(PasFile *f, const unsigned char *s, int32_t width) {
  __pas_write_str(f, (const char *)s + 1, s[0], width);
}

void __pas_write_astr(PasFile *f, const char *s, int32_t width) {
  __pas_write_str(f, s ? s : "", __pas_astr_length(s), width);
}

/* The rest of the line into *dst, an ansistring variable. */
void __pas_read_astr(PasFile *f, char **dst) {
  need_inspect(f);
  size_t n = 0, cap = 256;
  char *buf = malloc(cap + 1);
  if (!buf)
    __pas_runtime_error("out of memory in a string");
  for (;;) {
    int c = peek(f);
    if (c < 0 || f->eoln)
      break;
    if (n == cap) {
      char *bigger = realloc(buf, (cap *= 2) + 1);
      if (!bigger)
        __pas_runtime_error("out of memory in a string");
      buf = bigger;
    }
    buf[n++] = (char)c;
    advance(f);
  }
  buf[n] = '\0';
  __pas_astr_clear(dst);
  *dst = __pas_astr_from_cstr(buf);
  free(buf);
}

void __pas_read_str(PasFile *f, unsigned char *dst, int32_t maxlen) {
  need_inspect(f);
  int32_t n = 0;
  for (;;) {
    int c = peek(f);
    if (c < 0 || f->eoln)
      break;
    if (n < maxlen)
      dst[1 + n++] = (unsigned char)c;
    advance(f);
  }
  dst[0] = (unsigned char)n;
}
