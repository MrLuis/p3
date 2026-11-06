/*===- pascalrt.h - Pascal runtime library ABI ------------------------------===
 *
 * Functions called by code that pcc generates. The compiler declares these
 * itself (src/codegen/CodeGen.cpp); this header documents the ABI and lets
 * the runtime be tested from C. Keep both in sync.
 *
 * Pascal integer = int32_t (int64 = int64_t), char = char (8-bit), real = double,
 * field width 0 = "default", runtime errors exit with status 2.
 *
 *===------------------------------------------------------------------------===*/
#ifndef PASCALRT_H
#define PASCALRT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===- Program start-up / shut-down -----------------------------------------===*/

/* main() calls these around the program body. __pas_fini flushes all files
 * and exits with status 1 if writing to standard output failed. */
void __pas_init(int32_t argc, char **argv);
void __pas_fini(void);

/* Nonzero in --mode=tp programs (set by main): field widths are minimums
 * (no truncation) and booleans print as TRUE/FALSE. */
extern int32_t __pas_tp_mode;

/* Reports a runtime error ("Runtime error: <msg>") and exits with status 2.
 * The compiler passes messages like "division by zero at prog.pas:12:9". */
void __pas_runtime_error(const char *msg) __attribute__((noreturn));

/*===- Files (ISO 7185 6.4.3.5, 6.6.5.2, 6.9) -------------------------------===
 *
 * A Pascal file variable holds a PasFile*. Inspection (after reset) is lazy:
 * the buffer variable f^ is filled only when needed (f^, eof, eoln, get,
 * read), so interactive input works line by line.
 */
typedef struct PasFile PasFile;

/* The standard text files `input` and `output`. */
extern PasFile *__pas_input;
extern PasFile *__pas_output;

/* Creates a file variable. elem_size: bytes per component (1 for text).
 * name: external file name, or NULL for an internal (temporary) file. */
PasFile *__pas_file_new(int64_t elem_size, int32_t is_text, const char *name);
/* Releases a local file variable when its routine returns. */
void __pas_file_free(PasFile *f);
/* External name for program parameter number `index` (1-based among the
 * non-standard parameters): argv[index] if given, else `name`. */
const char *__pas_program_param(int32_t index, const char *name);

void __pas_reset(PasFile *f);
void __pas_rewrite(PasFile *f);
void __pas_get(PasFile *f);
void __pas_put(PasFile *f);
int32_t __pas_eof(PasFile *f);
int32_t __pas_eoln(PasFile *f);
void __pas_page(PasFile *f);
/* Address of the buffer variable f^. */
void *__pas_buffer(PasFile *f);

/* Typed files: read(f, x) == x := f^; get(f).  write(f, x) == f^ := x; put(f) */
void __pas_read_elem(PasFile *f, void *dst);
void __pas_write_elem(PasFile *f, const void *src);

/*===- Text output (write / writeln) ----------------------------------------===
 * width <= 0 means no field width was given. Per ISO 7185 6.9.3, strings and
 * booleans longer than their field width are truncated; numbers never are. */
void __pas_write_str(PasFile *f, const char *s, int64_t len, int32_t width);
void __pas_write_char(PasFile *f, char c, int32_t width);
void __pas_write_int(PasFile *f, int64_t v, int32_t width); /* any integer type */
void __pas_write_bool(PasFile *f, int32_t b, int32_t width); /* true/false */
/* frac < 0: floating-point form " d.dddE+ddd" (default width 22); frac >= 0:
 * fixed-point form with that many fraction digits (ISO 6.9.3.4). */
void __pas_write_real(PasFile *f, double x, int32_t width, int32_t frac);
void __pas_writeln(PasFile *f);

/*===- Text input (read / readln) -------------------------------------------===
 * Integers and reals skip blanks and line ends first. A line end reads as
 * ' ' (eoln is then true). Reading past the end is a runtime error. */
int32_t __pas_read_int(PasFile *f);
int64_t __pas_read_int64(PasFile *f); /* int64 and unsigned targets */
double __pas_read_real(PasFile *f);
char __pas_read_char(PasFile *f);
void __pas_readln(PasFile *f);

/*===- Short strings (Turbo/FPC `string`, M12) ------------------------------===
 * A short string is a length byte followed by up to 255 chars. `maxlen` is
 * the capacity of the destination; longer results are truncated. */
void __pas_str_assign(unsigned char *dst, int32_t maxlen, const unsigned char *src);
void __pas_str_concat(unsigned char *dst, const unsigned char *a, const unsigned char *b);
int32_t __pas_str_compare(const unsigned char *a, const unsigned char *b);
void __pas_str_copy(unsigned char *dst, const unsigned char *s, int32_t index, int32_t count);
int32_t __pas_str_pos(const unsigned char *sub, const unsigned char *s);
void __pas_str_delete(unsigned char *s, int32_t index, int32_t count);
void __pas_str_insert(const unsigned char *src, unsigned char *s, int32_t maxlen, int32_t index);
void __pas_write_pstr(PasFile *f, const unsigned char *s, int32_t width);
/* Reads the rest of the current line (not the line end) into dst. */
void __pas_read_str(PasFile *f, unsigned char *dst, int32_t maxlen);

/*===- Heap -----------------------------------------------------------------===*/

/* new / dispose. __pas_new returns zeroed memory; disposing nil is an error. */
void *__pas_new(int64_t size);
void __pas_dispose(void *p);

#ifdef __cplusplus
}
#endif

#endif /* PASCALRT_H */
