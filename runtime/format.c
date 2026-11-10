/*
 * Format: Free Pascal's algorithm and results, on the array of const that
 * compiled code builds: a header slot holding the element count, then one
 * {VType, value} slot per element (see pascalrt.h).
 */
#include "pascalrt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VtInteger = 0, VtBoolean = 1, VtChar = 2, VtExtended = 3, VtString = 4,
       VtPointer = 5, VtPChar = 6, VtAnsiString = 11, VtInt64 = 16 };

typedef struct {
  char *data;
  size_t len, cap;
} Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1)
      cap *= 2;
    char *d = realloc(b->data, cap);
    if (!d)
      __pas_runtime_error("out of memory in format");
    b->data = d;
    b->cap = cap;
  }
  if (n > 0)
    memcpy(b->data + b->len, s, n);
  b->len += n;
}

static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_fill(Buf *b, char c, long n) {
  for (long i = 0; i < n; i++)
    buf_add(b, &c, 1);
}

typedef struct {
  const char *fmt;
  int64_t len, pos, argpos;
  int64_t count;
  const PasVarRec *args;
  int64_t index, width, prec, value;
  int left;
  int64_t doarg;
  Buf result, add;
} Fmt;

static void fail(Fmt *f, const char *before, const char *after) {
  Buf m = {0};
  buf_str(&m, before);
  buf_add(&m, f->fmt, (size_t)f->len);
  buf_str(&m, after);
  free(f->result.data);
  free(f->add.data);
  __pas_raise_convert(m.data, m.len);
}

static void invalid_format(Fmt *f) { fail(f, "Invalid format specifier : \"", "\""); }
static void invalid_index(Fmt *f) { fail(f, "Invalid argument index in format \"", "\""); }
static void missing_arg(Fmt *f) { fail(f, "Missing argument in format \"", "\""); }

/* The character at 1-based position i; NUL past the end. */
static char at(const Fmt *f, int64_t i) {
  return i >= 1 && i <= f->len ? f->fmt[i - 1] : '\0';
}

static void read_integer(Fmt *f) {
  if (f->value != -1)
    return;
  int64_t old = f->pos;
  while (f->pos <= f->len && at(f, f->pos) >= '0' && at(f, f->pos) <= '9')
    f->pos++;
  if (f->pos > f->len)
    invalid_format(f);
  if (at(f, f->pos) == '*') {
    if (f->pos > old || f->argpos >= f->count)
      invalid_format(f);
    const PasVarRec *a = &f->args[f->argpos];
    if (a->vtype == VtInteger)
      f->value = a->v.integer;
    else if (a->vtype == VtInt64)
      f->value = *a->v.int64;
    else
      invalid_format(f);
    f->argpos++;
    f->pos++;
  } else if (old < f->pos) {
    int64_t v = 0;
    for (int64_t i = old; i < f->pos; i++)
      v = v * 10 + (at(f, i) - '0');
    f->value = v;
  } else {
    f->value = -1;
  }
}

static char read_format(Fmt *f) {
  f->index = f->width = f->prec = f->value = -1;
  f->pos++;
  if (at(f, f->pos) == '%')
    return '%';
  if (at(f, f->pos) != ':')
    read_integer(f);
  else
    f->value = 0;
  if (at(f, f->pos) == ':') {
    if (f->value == -1)
      missing_arg(f);
    f->index = f->value;
    f->value = -1;
    f->pos++;
  }
  f->left = 0;
  if (f->pos <= f->len && at(f, f->pos) == '-') {
    f->left = 1;
    f->pos++;
  }
  read_integer(f);
  if (f->value != -1) {
    f->width = f->value;
    f->value = -1;
  }
  if (f->pos <= f->len && at(f, f->pos) == '.') {
    f->pos++;
    read_integer(f);
    if (f->value == -1)
      f->value = 0;
    f->prec = f->value;
  }
  char c = at(f, f->pos);
  return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static int check_arg(Fmt *f, int64_t vtype, int err) {
  f->doarg = f->index == -1 ? f->argpos : f->index;
  f->argpos = f->doarg + 1;
  if (f->doarg >= f->count || f->args[f->doarg].vtype != vtype) {
    if (err)
      invalid_index(f);
    f->argpos--;
    return 0;
  }
  return 1;
}

/* value's first 15 significant digits (rounded by the C library) and
 * decimal exponent; returns 0 for NaN and the infinities, written to add. */
static int float_digits(Fmt *f, double value, char digits[16], int *exponent,
                        int *negative) {
  if (isnan(value)) {
    buf_str(&f->add, "Nan");
    return 0;
  }
  if (isinf(value)) {
    buf_str(&f->add, value > 0 ? "+Inf" : "-Inf");
    return 0;
  }
  char text[32];
  *negative = signbit(value) != 0 && value != 0.0;
  snprintf(text, sizeof text, "%.14e", fabs(value));
  digits[0] = text[0];
  memcpy(digits + 1, text + 2, 14);
  digits[15] = '\0';
  *exponent = atoi(strchr(text, 'e') + 1);
  return 1;
}

/* Rounds digits half up to keep digits; a carry out of the first digit
 * raises the exponent. keep may be 0 (the result is 0 or a lone 1). */
static void round_digits(char digits[16], int keep, int *exponent) {
  if (keep >= 15)
    return;
  int up = keep < 0 ? 0 : digits[keep] >= '5';
  for (int i = keep < 0 ? 0 : keep; i < 15; i++)
    digits[i] = '0';
  if (!up)
    return;
  int i = keep - 1;
  while (i >= 0 && digits[i] == '9')
    digits[i--] = '0';
  if (i >= 0) {
    digits[i]++;
  } else {
    memmove(digits + 1, digits, 14);
    digits[0] = '1';
    (*exponent)++;
  }
}

/* ffFixed, ffNumber and ffCurrency. */
static void add_fixed(Fmt *f, double value, int decimals, int group, int currency) {
  char digits[16];
  int exponent, negative;
  if (!float_digits(f, value, digits, &exponent, &negative))
    return;
  if (decimals < 0)
    decimals = 2;
  if (decimals > 18)
    decimals = 18;
  round_digits(digits, exponent + 1 + decimals, &exponent);
  int zero = 1;
  for (int i = 0; i < 15; i++)
    zero &= digits[i] == '0';
  if (negative && !zero)
    buf_add(&f->add, "-", 1);
  int whole = exponent >= 0 ? exponent + 1 : 1;
  for (int i = 0; i < whole; i++) {
    char c = exponent < 0 || i >= 15 ? '0' : digits[i];
    buf_add(&f->add, &c, 1);
    if (group && (whole - 1 - i) % 3 == 0 && i != whole - 1)
      buf_add(&f->add, ",", 1);
  }
  if (decimals > 0) {
    buf_add(&f->add, ".", 1);
    for (int i = 1; i <= decimals; i++) {
      int p = exponent + i;
      char c = p >= 0 && p < 15 ? digits[p] : '0';
      buf_add(&f->add, &c, 1);
    }
  }
  if (currency)
    buf_add(&f->add, "$", 1);
}

/* ffExponent, with three exponent digits. */
static void add_exponent(Fmt *f, double value, int precision) {
  char digits[16], tail[16];
  int exponent, negative;
  if (!float_digits(f, value, digits, &exponent, &negative))
    return;
  if (precision < 0 || precision > 15)
    precision = 15;
  if (precision < 2)
    precision = 2;
  int zero = value == 0.0;
  if (zero)
    exponent = 0;
  round_digits(digits, precision, &exponent);
  if (negative)
    buf_add(&f->add, "-", 1);
  buf_add(&f->add, digits, 1);
  buf_add(&f->add, ".", 1);
  buf_add(&f->add, digits + 1, (size_t)precision - 1);
  snprintf(tail, sizeof tail, "E%c%03d", exponent < 0 ? '-' : '+', abs(exponent));
  buf_str(&f->add, tail);
}

/* ffGeneral: fixed notation unless the exponent is below -5 or at least
 * the precision. */
static void add_general(Fmt *f, double value, int precision) {
  char digits[16], tail[16];
  int exponent, negative;
  if (!float_digits(f, value, digits, &exponent, &negative))
    return;
  if (value == 0.0) {
    buf_add(&f->add, "0", 1);
    return;
  }
  if (precision <= 0 || precision > 15)
    precision = 15;
  round_digits(digits, precision, &exponent);
  int last = precision;
  while (last > 1 && digits[last - 1] == '0')
    last--;
  if (negative)
    buf_add(&f->add, "-", 1);
  if (exponent < -5 || exponent >= precision) {
    buf_add(&f->add, digits, 1);
    if (last > 1) {
      buf_add(&f->add, ".", 1);
      buf_add(&f->add, digits + 1, (size_t)last - 1);
    }
    snprintf(tail, sizeof tail, "E%d", exponent);
    buf_str(&f->add, tail);
  } else if (exponent < 0) {
    buf_add(&f->add, "0.", 2);
    buf_fill(&f->add, '0', -exponent - 1);
    buf_add(&f->add, digits, (size_t)last);
  } else if (last <= exponent + 1) {
    buf_add(&f->add, digits, (size_t)last);
    buf_fill(&f->add, '0', exponent + 1 - last);
  } else {
    buf_add(&f->add, digits, (size_t)exponent + 1);
    buf_add(&f->add, ".", 1);
    buf_add(&f->add, digits + exponent + 1, (size_t)(last - exponent - 1));
  }
}

static void add_decimal(Fmt *f, const char *text) {
  long pad = (long)f->prec - (long)strlen(text);
  if (f->width < 0 && f->width != -1)
    f->width = -f->width;
  if (text[0] == '-') {
    buf_add(&f->add, "-", 1);
    buf_fill(&f->add, '0', pad + 1);
    buf_str(&f->add, text + 1);
  } else {
    buf_fill(&f->add, '0', pad);
    buf_str(&f->add, text);
  }
}

static void add_hex(Fmt *f, uint64_t v, int digits) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = digits - 1; i >= 0; i--) {
    char c = i < 16 ? hex[(v >> (4 * i)) & 15] : '0';
    buf_add(&f->add, &c, 1);
  }
}

char *__pas_format(const char *fmt, const PasVarRec *args) {
  Fmt st, *f = &st;
  memset(f, 0, sizeof st);
  f->fmt = fmt ? fmt : "";
  f->len = __pas_astr_length(fmt);
  f->count = args[0].vtype;
  f->args = args + 1;
  f->pos = 1;
  int64_t old = 1;
  while (f->pos <= f->len) {
    while (f->pos <= f->len && at(f, f->pos) != '%')
      f->pos++;
    if (f->pos > old)
      buf_add(&f->result, f->fmt + old - 1, (size_t)(f->pos - old));
    if (f->pos < f->len) {
      f->add.len = 0;
      char kind = read_format(f);
      char num[32];
      switch (kind) {
      case 'D':
        if (check_arg(f, VtInteger, 0))
          snprintf(num, sizeof num, "%d", f->args[f->doarg].v.integer);
        else {
          check_arg(f, VtInt64, 1);
          snprintf(num, sizeof num, "%lld", (long long)*f->args[f->doarg].v.int64);
        }
        add_decimal(f, num);
        break;
      case 'U':
        if (check_arg(f, VtInteger, 0))
          snprintf(num, sizeof num, "%u", (unsigned)f->args[f->doarg].v.integer);
        else {
          check_arg(f, VtInt64, 1);
          snprintf(num, sizeof num, "%llu",
                   (unsigned long long)*f->args[f->doarg].v.int64);
        }
        if (f->width != -1 && f->width < 0)
          f->width = -f->width;
        buf_fill(&f->add, '0', (long)f->prec - (long)strlen(num));
        buf_str(&f->add, num);
        break;
      case 'E':
        check_arg(f, VtExtended, 1);
        add_exponent(f, *f->args[f->doarg].v.real, (int)f->prec);
        break;
      case 'F':
        check_arg(f, VtExtended, 1);
        add_fixed(f, *f->args[f->doarg].v.real, (int)f->prec, 0, 0);
        break;
      case 'G':
        check_arg(f, VtExtended, 1);
        add_general(f, *f->args[f->doarg].v.real, (int)f->prec);
        break;
      case 'N':
        check_arg(f, VtExtended, 1);
        add_fixed(f, *f->args[f->doarg].v.real, (int)f->prec, 1, 0);
        break;
      case 'M':
        check_arg(f, VtExtended, 1);
        add_fixed(f, *f->args[f->doarg].v.real, (int)f->prec, 1, 1);
        break;
      case 'S': {
        const char *s = "";
        int64_t n = 0;
        char ch;
        if (check_arg(f, VtString, 0)) {
          s = (const char *)f->args[f->doarg].v.pointer + 1;
          n = *(const unsigned char *)f->args[f->doarg].v.pointer;
        } else if (check_arg(f, VtChar, 0)) {
          ch = (char)f->args[f->doarg].v.integer;
          s = &ch;
          n = 1;
        } else if (check_arg(f, VtPChar, 0)) {
          s = f->args[f->doarg].v.pointer ? f->args[f->doarg].v.pointer : "";
          n = (int64_t)strlen(s);
        } else {
          check_arg(f, VtAnsiString, 1);
          s = f->args[f->doarg].v.pointer ? f->args[f->doarg].v.pointer : "";
          n = __pas_astr_length(f->args[f->doarg].v.pointer);
        }
        if (f->prec != -1 && n > f->prec)
          n = f->prec;
        buf_add(&f->add, s, (size_t)n);
        break;
      }
      case 'P':
        check_arg(f, VtPointer, 1);
        add_hex(f, (uint64_t)(uintptr_t)f->args[f->doarg].v.pointer, 16);
        break;
      case 'X': {
        uint64_t v;
        int limit;
        if (check_arg(f, VtInteger, 0)) {
          v = (uint32_t)f->args[f->doarg].v.integer;
          limit = 16;
        } else {
          check_arg(f, VtInt64, 1);
          v = (uint64_t)*f->args[f->doarg].v.int64;
          limit = 31;
        }
        if (f->prec > limit) {
          add_hex(f, v, limit);
        } else {
          int need = 1;
          while (need < 16 && (v >> (need * 4)) != 0)
            need++;
          add_hex(f, v, f->prec > need ? (int)f->prec : need);
        }
        break;
      }
      case '%':
        buf_add(&f->add, "%", 1);
        break;
      default:
        break;
      }
      long pad = f->width == -1 ? 0 : (long)f->width - (long)f->add.len;
      if (pad > 0 && !f->left)
        buf_fill(&f->result, ' ', pad);
      buf_add(&f->result, f->add.data, f->add.len);
      if (pad > 0 && f->left)
        buf_fill(&f->result, ' ', pad);
    }
    f->pos++;
    old = f->pos;
  }
  char *r = __pas_astr_from_chars(f->result.data ? f->result.data : "", (int64_t)f->result.len);
  free(f->result.data);
  free(f->add.data);
  return r;
}

void __pas_format_pstr(unsigned char *dst, const char *fmt, const PasVarRec *args) {
  char *r = __pas_format(fmt, args);
  __pas_astr_to_pstr(dst, 255, r);
  __pas_astr_clear(&r);
}

void __pas_format_to(char **dst, const char *fmt, const PasVarRec *args) {
  char *r = __pas_format(fmt, args);
  __pas_astr_clear(dst);
  *dst = r;
}

int32_t __pas_aoc_bad_index(const PasVarRec *args, int64_t index) {
  return index < 0 || index >= args[0].vtype;
}
