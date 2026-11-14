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

#include <stddef.h>
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

/* Turbo/Free Pascal: assign(f, name) (an ansistring; '' is standard
 * input/output), close(f); reset/rewrite with a record size (untyped files);
 * blockread/blockwrite (count records; checked: an error when fewer are
 * done), seek/filepos/filesize in records. A typed file named with assign,
 * and an untyped file, are random access. is_text of __pas_file_new: 2 for
 * an untyped file. */
void __pas_assign(PasFile *f, const char *name);
void __pas_close(PasFile *f);
/* flush(f): writes out a text file's buffered output. */
void __pas_flush(PasFile *f);
void __pas_reset_size(PasFile *f, int64_t recsize);
void __pas_rewrite_size(PasFile *f, int64_t recsize);
int64_t __pas_blockread(PasFile *f, void *buf, int64_t count, int32_t checked);
int64_t __pas_blockwrite(PasFile *f, const void *buf, int64_t count,
                         int32_t checked);
void __pas_seek(PasFile *f, int64_t n);
int64_t __pas_filepos(PasFile *f);
int64_t __pas_filesize(PasFile *f);
/* GetFileHandle(f): the file descriptor of an open file (input 0, output
 * 1), -1 if it is not open (FPC's UnusedHandle). */
int32_t __pas_file_handle(PasFile *f);

/* Typed files: read(f, x) == x := f^; get(f).  write(f, x) == f^ := x; put(f) */
void __pas_read_elem(PasFile *f, void *dst);
void __pas_write_elem(PasFile *f, const void *src);

/*===- Text output (write / writeln) ----------------------------------------===
 * width <= 0 means no field width was given. Per ISO 7185 6.9.3, strings and
 * booleans longer than their field width are truncated; numbers never are. */
void __pas_write_str(PasFile *f, const char *s, int64_t len, int32_t width);
void __pas_write_char(PasFile *f, char c, int32_t width);
void __pas_write_int(PasFile *f, int64_t v, int32_t width); /* any integer type */
void __pas_write_uint(PasFile *f, uint64_t v, int32_t width); /* qword */
/* An enumeration's value v (checked by the caller): names[v], a
 * length-prefixed string, padded on the right (as Free Pascal). */
void __pas_write_enum(PasFile *f, const unsigned char *const *names, int64_t v,
                      int32_t width);
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
uint64_t __pas_read_uint64(PasFile *f); /* qword: 0 .. 2^64-1 */
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
/* s := s + src, in place: what fits into s's maxlen characters. */
void __pas_str_append(unsigned char *s, int32_t maxlen, const unsigned char *src);
void __pas_str_insert(const unsigned char *src, unsigned char *s, int32_t maxlen, int32_t index);
void __pas_write_pstr(PasFile *f, const unsigned char *s, int32_t width);
/* Reads the rest of the current line (not the line end) into dst. */
void __pas_read_str(PasFile *f, unsigned char *dst, int32_t maxlen);

/*===- Ansistrings (ansi.c) -------------------------------------------------===*/
/* Dynamic arrays: a value is NULL (empty) or a pointer to the elements,
 * preceded by a header (reference count, length, element info). Elements
 * start zeroed. The compiler emits one PasDynInfo per element type; fini and
 * addref are NULL unless the elements hold managed values. */
typedef struct PasDynInfo {
  int64_t elem_size;
  void (*fini)(void *elem);
  void (*addref)(void *elem);
} PasDynInfo;
void *__pas_dyn_new(const PasDynInfo *info, int64_t count);
int64_t __pas_dyn_length(const void *a);
void __pas_dyn_addref(void *a);
void __pas_dyn_clear(void **a);
void __pas_dyn_assign(void **dst, void *src);
void __pas_dyn_setlength(void **a, const PasDynInfo *info, int64_t count);
/* A new array of up to count elements from index start (clamped). */
void *__pas_dyn_copy(const void *a, int64_t start, int64_t count);
/* Nonzero if index is outside the array. */
int32_t __pas_dyn_bad_index(const void *a, int64_t index);

void __pas_astr_addref(char *s);
void __pas_astr_assign(char **dst, char *src);
void __pas_astr_clear(char **p);
int32_t __pas_astr_length(const char *s);
char *__pas_astr_from_chars(const char *p, int64_t len);
char *__pas_astr_from_pstr(const unsigned char *p);
char *__pas_astr_from_char(char c);
char *__pas_astr_from_cstr(const char *p);
void __pas_astr_to_pstr(unsigned char *dst, int32_t maxlen, const char *s);
const char *__pas_astr_pchar(const char *s);
char *__pas_astr_concat(char *a, char *b);
int32_t __pas_astr_compare(const char *a, const char *b);
int32_t __pas_astr_pos(const char *sub, const char *s);
char *__pas_astr_copy(char *s, int32_t index, int32_t count);
char *__pas_astr_unique(char **p);
int32_t __pas_astr_bad_index(const char *s, int64_t index);
void __pas_astr_setlength(char **p, int32_t len);
void __pas_astr_delete(char **p, int32_t index, int32_t count);
void __pas_astr_insert(char *src, char **p, int32_t index);
void __pas_str_setlength(unsigned char *s, int32_t maxlen, int32_t len);
void __pas_write_astr(PasFile *f, const char *s, int32_t width);
/* write(f, p) of a pchar: the C string p points to (nothing for nil, not
 * even padding). */
void __pas_write_cstr(PasFile *f, const char *s, int32_t width);
void __pas_read_astr(PasFile *f, char **dst);

/*===- Array of const and Format (format.c) ---------------------------------===
 * An array of const is passed as a pointer to a header slot, whose first
 * word is the element count, followed by the elements, TVarRec's layout. */
typedef struct PasVarRec {
  int64_t vtype; /* the element count in the header slot */
  union {
    int32_t integer; /* also vtBoolean and vtChar */
    const double *real;
    const int64_t *int64;
    const char *pointer; /* vtString, vtPointer, vtPChar, vtAnsiString, ... */
  } v;
} PasVarRec;
char *__pas_format(const char *fmt, const PasVarRec *args);
void __pas_format_pstr(unsigned char *dst, const char *fmt, const PasVarRec *args);
void __pas_format_to(char **dst, const char *fmt, const PasVarRec *args);
int32_t __pas_aoc_bad_index(const PasVarRec *args, int64_t index);
/* Raises EConvertError with the n characters of msg, and frees msg. */
void __pas_raise_convert(char *msg, size_t n) __attribute__((noreturn));

/*===- Heap -----------------------------------------------------------------===*/

/* new / dispose. __pas_new returns zeroed memory; disposing nil is an error. */
void *__pas_new(int64_t size);
void __pas_dispose(void *p);
/* reallocmem (Turbo/FPC): realloc; size <= 0 frees and returns NULL. getmem
 * uses __pas_new, freemem calls free(). */
void *__pas_realloc(void *p, int64_t size);

/*===- Program (Turbo/FPC) --------------------------------------------------===*/

/* halt(code): flushes files like __pas_fini, then exits with code. */
void __pas_halt(int32_t code) __attribute__((noreturn));
/* Registers a class destructor (a Pascal routine, called with a null static
 * link) to run at exit (__pas_fini, also from halt), last registered
 * first. */
void __pas_at_exit(void (*fn)(void *));
int32_t __pas_paramcount(void);
/* paramstr(index) into a short string (index 0 is the program name). */
void __pas_paramstr(unsigned char *dst, int32_t index);

/*===- Classes (--mode=objfpc) ---------------------------------------------===
 * An object starts with a pointer to its class's VMT, an array of pointers:
 * [0] the parent class's VMT (NULL for TObject), [1] the class name as a
 * length-prefixed string, [2] the instance size (an integer), [3] the
 * interface table or NULL, [4...] the virtual methods; [4] is Destroy. A
 * class reference is a VMT pointer. Methods take (static link, Self,
 * parameters...); a class method's Self is a class reference.
 * The interface table: [n, cleanup, iid1, methods1, ..., iidn, methodsn]
 * (n an integer): the interfaces the class implements and its method
 * tables for them; cleanup (or NULL) releases the COM interfaces held by
 * the class's own fields. An interface value is {object, method table}. */
extern void *const __pas_tobject_vmt[5];
/* COM interfaces: their objects are TInterfacedObjects, whose reference
 * count (FRefCount, an int32) follows the VMT pointer. TInterfacedObject
 * implements IUnknown (iid __pas_iunknown_iid). */
extern void *const __pas_tinterfacedobject_vmt[5];
extern const unsigned char __pas_iunknown_iid[];
typedef struct PasIntf {
  void *obj;
  void *methods;
} PasIntf;
/* One more / one fewer reference to obj (NULL: nothing); the last one
 * destroys and frees it. */
void __pas_intf_addref(void *obj);
void __pas_intf_release(void *obj);
/* *dst := {obj, methods}: references the new object, then releases the
 * old one. */
void __pas_intf_assign(PasIntf *dst, void *obj, void *methods);
/* Releases *p's object and sets *p to nil. */
void __pas_intf_clear(PasIntf *p);
/* While a TInterfacedObject's constructor runs its count is 1, so that
 * references taken and dropped there do not free it. */
void __pas_interfaced_construct(void *obj);
void __pas_interfaced_constructed(void *obj);
/* Frees an object after its destructor ran: releases the COM interfaces
 * in its fields (each class's cleanup), then the memory. */
void __pas_free_instance(void *obj);
/* The method table of obj's class for the interface iid, or NULL (also if
 * obj is NULL). */
void *__pas_intf_lookup(const void *obj, const void *iid);
/* obj as the interface: its table; a runtime error `msg` if obj is not nil
 * and its class does not implement it. */
void *__pas_intf_as(const void *obj, const void *iid, const char *msg);
/* cls.InheritsFrom(vmt): vmt is in the chain of VMTs from cls. */
int32_t __pas_class_inherits(const void *cls, const void *vmt);
/* TObject.Destroy, which does nothing. */
void __pas_tobject_destroy(void *link, void *self);
/* obj is C: obj is not nil and C's VMT is in obj's chain of VMTs. */
int32_t __pas_class_is(const void *obj, const void *vmt);
/* obj as C: obj, or a runtime error `msg` if obj is not nil and not a C. */
void *__pas_class_as(void *obj, const void *vmt, const char *msg);
/* The VMT entry of an abstract method: a runtime error. */
void __pas_abstract_error(void) __attribute__((noreturn));

/*===- Exceptions (--mode=objfpc) ------------------------------------------===
 * A PasExcFrame lives in the stack frame of the routine with the try
 * statement (the compiler's type is { ptr, ptr, ptr, [64 x i64] }). The
 * compiled code calls __pas_exc_push, then _setjmp(&f->buf): 0 runs the
 * protected statements, which end with __pas_exc_pop; nonzero means an
 * exception landed, f->object (already popped). Errors exit with status 2.
 * (The compiler's type is { ptr, ptr, ptr, ptr, [64 x i64] }.) */
typedef struct PasExcFrame {
  struct PasExcFrame *prev;
  void *object;      /* the exception, once raised to this frame */
  const char *where; /* "file:line:col" of its raise statement; NULL for
                        `raise ... at`, "" when raised by the runtime */
  void *address;     /* its address (ExceptAddr): the `at` address, else
                        where the raise statement's code calls the runtime */
  int64_t buf[64];   /* a jmp_buf */
} PasExcFrame;

/* Exception (Free Pascal's SysUtils): [TObject's VMT, "Exception",
 * Destroy]; its instances hold the message, a short string, after the VMT
 * pointer. */
extern void *const __pas_exception_vmt[5];
/* EExternal = class(Exception), EIntError = class(EExternal),
 * EDivByZero = class(EIntError) (SysUtils). */
extern void *const __pas_eexternal_vmt[5];
extern void *const __pas_einterror_vmt[5];
extern void *const __pas_edivbyzero_vmt[5];
/* EConvertError = class(Exception) (SysUtils). */
extern void *const __pas_econverterror_vmt[5];
/* The compiled code keeps what it reads after an exception arrives in
 * memory (see pinInMemory in the code generator): _longjmp does not undo
 * changes there, as it does for registers. */
void __pas_exc_push(PasExcFrame *f);
void __pas_exc_pop(PasExcFrame *f);
/* raise obj: to the innermost frame, else "unhandled exception". */
void __pas_raise(void *obj, const char *where) __attribute__((noreturn));
/* raise obj at address[, frame]: as __pas_raise, with the given address;
 * unhandled, it is reported at that address, as by Free Pascal. */
void __pas_raise_at(void *obj, const char *where, void *address)
    __attribute__((noreturn));
/* Integer div or mod by zero (objfpc): raises a new EDivByZero, with the
 * message 'Division by zero'. */
void __pas_raise_divzero(const char *where) __attribute__((noreturn));
/* Raises f's exception again (after a finally part, or with no match). */
void __pas_reraise(PasExcFrame *f) __attribute__((noreturn));
/* obj.Destroy (virtual) and releases it: a handled exception. */
void __pas_free_object(void *obj);
/* Frees obj unless it is f's exception, then re-raises that: an exception
 * leaving a handler (obj: the handled one) or a constructor (obj: the new
 * object). */
void __pas_reraise_free(PasExcFrame *f, void *obj) __attribute__((noreturn));

/* Owned variables (`x: TFoo owned`).
 * __pas_owned_set: x := obj, freeing the object x had (unless it is obj).
 * When x's routine ends, in its finally part: __pas_owned_free(&x, &exc)
 * for each owned variable, the last declared first, frees its object and
 * makes it nil; a destructor's exception is kept in exc if it is the first
 * and dropped otherwise. Then __pas_owned_done(&exc, raised) raises the one
 * kept, or drops it too if an exception is already leaving the routine. */
typedef struct PasOwnedExc {
  void *object;
  const char *where;
  void *address;
} PasOwnedExc;
void __pas_owned_set(void **slot, void *obj);
void __pas_owned_free(void **slot, PasOwnedExc *exc);
void __pas_owned_done(PasOwnedExc *exc, int32_t raised);

#ifdef __cplusplus
}
#endif

#endif /* PASCALRT_H */
