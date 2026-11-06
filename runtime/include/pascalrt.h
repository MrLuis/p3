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
void __pas_writeln(void);

/* Reports a runtime error and terminates with exit status 2. */
void __pas_runtime_error(const char *msg) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* PASCALRT_H */
