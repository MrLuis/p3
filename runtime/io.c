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
 * Turbo/Free Pascal files: a typed file named with assign, and every
 * untyped file (`file`), are random access instead: no buffer variable,
 * read and write (and blockread, blockwrite, seek) at the current position
 * whether opened with reset or rewrite, eof when the position reaches the
 * size. Text files keep the model above, named or not.
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
  int turbo;             /* random access (see above) */
  int untyped;           /* `file`: elem_size is the record size */
  int name_owned;        /* name: a copy (assign) to free */
  int std_stream;        /* assign(f, ''): stdin / stdout, never closed */
  int last_io;           /* turbo: 1 read, 2 write (stdio needs a seek
                            between the two) */
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

/* Class destructors to run at exit (__pas_at_exit), last registered
 * first. */
struct PasExitFn {
  void (*fn)(void *);
  struct PasExitFn *next;
};
struct PasExitFn *__pas_exit_fns;

void __pas_at_exit(void (*fn)(void *)) {
  struct PasExitFn *e = malloc(sizeof *e);
  if (!e)
    __pas_runtime_error("out of memory");
  e->fn = fn;
  e->next = __pas_exit_fns;
  __pas_exit_fns = e;
}

void __pas_fini(void) {
  while (__pas_exit_fns) { /* (one may halt: each runs once) */
    struct PasExitFn *e = __pas_exit_fns;
    __pas_exit_fns = e->next;
    e->fn(NULL); /* a Pascal routine: its static link */
    free(e);
  }
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
  f->untyped = is_text == 2; /* `file`: 128-byte records until reset/rewrite */
  f->turbo = f->untyped;
  if (f->untyped)
    elem_size = 128;
  f->elem_size = elem_size > 0 ? elem_size : 1;
  f->is_text = is_text == 1;
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
  if (f->fp && !f->std_stream)
    fclose(f->fp);
  if (f->name_owned)
    free((char *)f->name);
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
    if (f->fp && !f->std_stream)
      fclose(f->fp);
    f->std_stream = !f->name[0]; /* assign(f, ''): standard input */
    if (f->std_stream)
      f->fp = stdin;
    else if (f->turbo) { /* reading and writing (FPC's FileMode 2) */
      f->fp = fopen(f->name, "r+b");
      if (!f->fp)
        f->fp = fopen(f->name, "rb");
    } else
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
  f->last_io = 0;
}

void __pas_rewrite(PasFile *f) {
  if (f->is_std) {
    if (f != &__pas_std_output)
      file_error(f, "'rewrite' of the standard input file");
    return;
  }
  if (f->fp && !f->std_stream)
    fclose(f->fp);
  f->std_stream = f->name && !f->name[0]; /* assign(f, ''): standard output */
  f->fp = f->std_stream ? stdout
          : f->name    ? fopen(f->name, f->turbo ? "w+b" : "wb")
                       : tmpfile();
  if (!f->fp)
    file_error(f, "cannot open file for writing");
  f->last_io = 0;
  f->mode = MODE_GENERATE;
  f->at_eof = 1; /* ISO: eof is true while generating */
  f->buf_full = f->eoln = f->line_has_chars = 0;
}

/*===- Turbo/Free Pascal file routines --------------------------------------===*/

void __pas_assign(PasFile *f, const char *name) {
  if (f->is_std)
    file_error(f, "'assign' of a standard file");
  if (f->fp && !f->std_stream)
    fclose(f->fp);
  f->fp = NULL;
  f->mode = MODE_UNDEFINED;
  if (f->name_owned)
    free((char *)f->name);
  int32_t n = name ? __pas_astr_length(name) : 0;
  char *copy = malloc((size_t)n + 1);
  if (!copy)
    __pas_runtime_error("out of memory assigning a file name");
  if (n)
    memcpy(copy, name, (size_t)n);
  copy[n] = '\0';
  f->name = copy;
  f->name_owned = 1;
  f->std_stream = 0;
  if (!f->is_text)
    f->turbo = 1;
}

void __pas_close(PasFile *f) {
  if (f->is_std) { /* close(output) flushes it */
    fflush(f->fp ? f->fp : stdout);
    return;
  }
  if (f->mode == MODE_UNDEFINED || !f->fp)
    file_error(f, "'close' of a file that is not open");
  if (f->std_stream)
    fflush(f->fp);
  else
    fclose(f->fp);
  f->fp = NULL;
  f->mode = MODE_UNDEFINED;
}

void __pas_reset_size(PasFile *f, int64_t recsize) {
  if (recsize <= 0)
    file_error(f, "the record size of 'reset' must be positive");
  f->elem_size = recsize;
  __pas_reset(f);
}

void __pas_rewrite_size(PasFile *f, int64_t recsize) {
  if (recsize <= 0)
    file_error(f, "the record size of 'rewrite' must be positive");
  f->elem_size = recsize;
  __pas_rewrite(f);
}

static void need_open(PasFile *f, const char *what) {
  if (f->mode == MODE_UNDEFINED || !f->fp) {
    char msg[96];
    snprintf(msg, sizeof msg, "'%s' of a file that is not open", what);
    file_error(f, msg);
  }
}

/* stdio needs a positioning call between reading and writing. */
static void turbo_switch(PasFile *f, int io) {
  if (f->last_io && f->last_io != io && !f->std_stream)
    fseek(f->fp, 0, SEEK_CUR);
  f->last_io = io;
}

static int64_t turbo_size(PasFile *f) { /* in bytes */
  if (f->std_stream)
    return 0;
  long cur = ftell(f->fp);
  fseek(f->fp, 0, SEEK_END);
  long end = ftell(f->fp);
  fseek(f->fp, cur, SEEK_SET);
  f->last_io = 0;
  return end;
}

int64_t __pas_blockread(PasFile *f, void *buf, int64_t count, int32_t checked) {
  need_open(f, "blockread");
  turbo_switch(f, 1);
  int64_t n = count > 0 ? (int64_t)fread(buf, (size_t)f->elem_size,
                                         (size_t)count, f->fp)
                        : 0;
  if (checked && n < count)
    file_error(f, "'blockread' read fewer records than asked (give it a "
                  "variable for the count read)");
  return n;
}

int64_t __pas_blockwrite(PasFile *f, const void *buf, int64_t count,
                         int32_t checked) {
  need_open(f, "blockwrite");
  turbo_switch(f, 2);
  int64_t n = count > 0 ? (int64_t)fwrite(buf, (size_t)f->elem_size,
                                          (size_t)count, f->fp)
                        : 0;
  if (checked && n < count)
    file_error(f, "error writing file");
  return n;
}

void __pas_seek(PasFile *f, int64_t n) {
  need_open(f, "seek");
  if (!f->turbo)
    file_error(f, "'seek' of a file not named with 'assign'");
  if (n < 0 || fseek(f->fp, (long)(n * f->elem_size), SEEK_SET) != 0)
    file_error(f, "'seek' to a position outside the file");
  f->last_io = 0;
}

int64_t __pas_filepos(PasFile *f) {
  need_open(f, "filepos");
  if (!f->turbo)
    file_error(f, "'filepos' of a file not named with 'assign'");
  if (f->std_stream)
    return 0;
  return (int64_t)ftell(f->fp) / f->elem_size;
}

int64_t __pas_filesize(PasFile *f) {
  need_open(f, "filesize");
  if (!f->turbo)
    file_error(f, "'filesize' of a file not named with 'assign'");
  return turbo_size(f) / f->elem_size;
}

int32_t __pas_file_handle(PasFile *f) {
  if (f->is_std)
    return f->fp ? fileno(f->fp) : f == &__pas_std_input ? 0 : 1;
  if (f->mode == MODE_UNDEFINED || !f->fp)
    return -1;
  return fileno(f->fp);
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
  if (f->turbo)
    file_error(f, "the buffer variable of a file named with 'assign'");
  if (f->mode == MODE_INSPECT)
    fill(f);
  return f->buf;
}

int32_t __pas_eof(PasFile *f) {
  if (f->turbo) { /* the position at the size */
    need_open(f, "eof");
    if (f->std_stream)
      return feof(f->fp) != 0;
    return ftell(f->fp) >= turbo_size(f);
  }
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
  if (f->turbo)
    file_error(f, "'get' of a file named with 'assign' (use 'read')");
  need_inspect(f);
  fill(f);
  if (f->at_eof)
    file_error(f, "'get' past the end of the file");
  f->buf_full = 0;
}

void __pas_put(PasFile *f) {
  if (f->turbo)
    file_error(f, "'put' of a file named with 'assign' (use 'write')");
  need_generate(f);
  if (fwrite(f->buf, (size_t)f->elem_size, 1, f->fp) != 1)
    file_error(f, "error writing file");
}

void __pas_page(PasFile *f) {
  need_generate(f);
  putc('\f', f->fp);
}

void __pas_read_elem(PasFile *f, void *dst) {
  if (f->turbo) {
    need_open(f, "read");
    turbo_switch(f, 1);
    if (fread(dst, (size_t)f->elem_size, 1, f->fp) != 1)
      file_error(f, "'read' past the end of the file");
    return;
  }
  need_inspect(f);
  fill(f);
  if (f->at_eof)
    file_error(f, "'read' past the end of the file");
  memcpy(dst, f->buf, (size_t)f->elem_size);
  f->buf_full = 0;
}

void __pas_write_elem(PasFile *f, const void *src) {
  if (f->turbo) {
    need_open(f, "write");
    turbo_switch(f, 2);
    if (fwrite(src, (size_t)f->elem_size, 1, f->fp) != 1)
      file_error(f, "error writing file");
    return;
  }
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

void __pas_write_enum(PasFile *f, const unsigned char *const *names, int64_t v,
                      int32_t width) {
  need_generate(f);
  const unsigned char *s = names[v];
  fwrite(s + 1, 1, s[0], f->fp);
  pad(f, width - s[0]);
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

_Static_assert(offsetof(PasExcFrame, buf) == 4 * sizeof(void *),
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

/* "Runtime error: unhandled exception <class> at <where>[: <message>]";
 * at $<address> (16 hexadecimal digits, as Free Pascal) for raise ... at. */
static void unhandled(void *obj, const char *where, void *address) {
  const unsigned char *name = (*(const unsigned char *const *const *)obj)[1];
  char msg[700];
  int n = snprintf(msg, sizeof msg, "unhandled exception %.*s", name[0],
                   (const char *)name + 1);
  if (where && *where)
    n += snprintf(msg + n, sizeof msg - (size_t)n, " at %s", where);
  else if (!where)
    n += snprintf(msg + n, sizeof msg - (size_t)n, " at $%016llX",
                  (unsigned long long)(uintptr_t)address);
  if (__pas_class_is(obj, __pas_exception_vmt)) {
    const unsigned char *text = (const unsigned char *)obj + sizeof(void *);
    snprintf(msg + n, sizeof msg - (size_t)n, ": %.*s", text[0],
             (const char *)text + 1);
  }
  __pas_runtime_error(msg);
}

static void raise_to_frame(void *obj, const char *where, void *address)
    __attribute__((noreturn));

void __pas_raise(void *obj, const char *where) {
  raise_to_frame(obj, where, __builtin_return_address(0));
}

void __pas_raise_at(void *obj, const char *where, void *address) {
  if (!obj)
    raise_to_frame(obj, where, address); /* reported at the raise */
  raise_to_frame(obj, NULL, address);
}

static void raise_to_frame(void *obj, const char *where, void *address) {
  if (!obj) {
    char msg[300];
    snprintf(msg, sizeof msg, "'raise' of a nil object%s%s",
             where && *where ? " at " : "", where ? where : "");
    __pas_runtime_error(msg);
  }
  PasExcFrame *f = __pas_exc_top;
  if (!f)
    unhandled(obj, where, address);
  __pas_exc_top = f->prev;
  f->object = obj;
  f->where = where;
  f->address = address;
  _longjmp(*(jmp_buf *)(void *)f->buf, 1);
}

void __pas_reraise(PasExcFrame *f) {
  raise_to_frame(f->object, f->where, f->address);
}

void __pas_raise_divzero(const char *where) {
  static const char text[] = "Division by zero";
  struct PasExceptionObject *e = __pas_new(sizeof *e);
  e->vmt = __pas_edivbyzero_vmt;
  e->message[0] = sizeof text - 1;
  memcpy(e->message + 1, text, sizeof text - 1);
  raise_to_frame(e, where, __builtin_return_address(0));
}

void __pas_raise_convert(char *msg, size_t n) {
  struct PasExceptionObject *e = __pas_new(sizeof *e);
  e->vmt = __pas_econverterror_vmt;
  if (n > 255)
    n = 255;
  e->message[0] = (unsigned char)n;
  memcpy(e->message + 1, msg, n);
  free(msg);
  raise_to_frame(e, "", __builtin_return_address(0));
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
