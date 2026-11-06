/*===- pascalrt.h - Pascal runtime library ABI ------------------------------===
 *
 * Functions called by code that pcc generates. The compiler declares these
 * itself (src/codegen/CodeGen.cpp); this header documents the ABI and lets
 * the runtime be tested from C. Keep both in sync.
 *
 * Pascal integer = int32_t, char = char (8-bit), field width 0 = "default".
 *
 *===------------------------------------------------------------------------===*/
#ifndef PASCALRT_H
#define PASCALRT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Program start-up / shut-down. Generated main() calls these around the
 * program body. __pas_fini flushes output and exits with status 1 if
 * writing failed. */
void __pas_init(int32_t argc, char **argv);
void __pas_fini(void);

/* write / writeln on the standard file `output`.
 * width <= 0 means no field width was given. Per ISO 7185 6.9.3, a string
 * longer than its field width is truncated to the first `width` chars. */
void __pas_write_str(const char *s, int64_t len, int32_t width);
void __pas_write_char(char c, int32_t width);
void __pas_write_int(int32_t v, int32_t width);
void __pas_write_bool(int32_t b, int32_t width); /* "true" / "false" */
/* Reals (ISO 7185 6.9.3.4). frac < 0: floating-point form " d.dddE+ddd"
 * (default width 22, i.e. 14 fraction digits); frac >= 0: fixed-point form
 * with that many fraction digits. */
void __pas_write_real(double x, int32_t width, int32_t frac);
void __pas_writeln(void);

/* read / readln on the standard file `input` (ISO 7185 6.9.1, 6.9.2).
 * read_int skips blanks and line ends, then reads an optionally signed
 * decimal integer; it is a runtime error if none follows or it overflows.
 * read_char returns the next character; the end of a line reads as ' '.
 * readln skips to the start of the next line. Reading past the end of the
 * file is a runtime error. */
int32_t __pas_read_int(void);
double __pas_read_real(void);
char __pas_read_char(void);
void __pas_readln(void);

/* Reports a runtime error and terminates with exit status 2. The compiler
 * passes messages like "division by zero at prog.pas:12:9". */
void __pas_runtime_error(const char *msg) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* PASCALRT_H */
