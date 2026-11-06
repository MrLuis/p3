/*===- io.c - Text output for Pascal programs -------------------------------===*/

#include "pascalrt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static void pad(int32_t n) {
  for (; n > 0; --n)
    putchar(' ');
}

void __pas_init(int32_t argc, char **argv) {
  (void)argc;
  (void)argv; /* program parameters bind to argv from M10 */
}

void __pas_fini(void) {
  if (fflush(stdout) != 0 || ferror(stdout)) {
    fputs("Runtime error: error writing to output\n", stderr);
    exit(1);
  }
}

void __pas_write_str(const char *s, int64_t len, int32_t width) {
  if (width > 0) {
    if (len > width)
      len = width; /* ISO 7185: truncate */
    else
      pad(width - (int32_t)len);
  }
  fwrite(s, 1, (size_t)len, stdout);
}

void __pas_write_char(char c, int32_t width) {
  pad(width - 1);
  putchar((unsigned char)c);
}

void __pas_write_int(int32_t v, int32_t width) {
  char buf[16];
  int n = snprintf(buf, sizeof buf, "%" PRId32, v);
  pad(width - n); /* integers are never truncated */
  fwrite(buf, 1, (size_t)n, stdout);
}

void __pas_writeln(void) { putchar('\n'); }

void __pas_runtime_error(const char *msg) {
  fflush(stdout);
  fprintf(stderr, "Runtime error: %s\n", msg);
  exit(2);
}

void __pas_write_bool(int32_t b, int32_t width) {
  if (b)
    __pas_write_str("true", 4, width);
  else
    __pas_write_str("false", 5, width);
}

/*===- Input ----------------------------------------------------------------===*/

static int peek_input(void) {
  int c = getchar();
  if (c != EOF)
    ungetc(c, stdin);
  return c;
}

int32_t __pas_read_int(void) {
  int c;
  do
    c = getchar();
  while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v');
  if (c == EOF)
    __pas_runtime_error("unexpected end of file while reading an integer");

  int negative = 0;
  if (c == '+' || c == '-') {
    negative = c == '-';
    c = getchar();
  }
  if (c < '0' || c > '9')
    __pas_runtime_error("invalid integer in input");

  int64_t value = 0;
  while (c >= '0' && c <= '9') {
    value = value * 10 + (c - '0');
    if (value > (int64_t)INT32_MAX + negative)
      __pas_runtime_error("integer in input is out of range");
    c = getchar();
  }
  if (c != EOF)
    ungetc(c, stdin);
  return (int32_t)(negative ? -value : value);
}

char __pas_read_char(void) {
  int c = getchar();
  if (c == EOF)
    __pas_runtime_error("unexpected end of file while reading a char");
  if (c == '\r' && peek_input() == '\n')
    c = getchar(); /* CRLF counts as one line end */
  return c == '\n' ? ' ' : (char)c;
}

void __pas_readln(void) {
  int c;
  do
    c = getchar();
  while (c != '\n' && c != EOF);
}
