/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/*===- pascalrt.h - Pascal runtime library ABI ------------------------------===
 *
 * Functions called by code that p3 generates. The compiler declares these
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

/* Nonzero once the runtime has started a thread (the task pool or the
 * timer thread, async.c). Until then reference counts (ansistrings,
 * dynamic arrays, interfaces) change with plain operations, as in Free
 * Pascal without cthreads; from then on atomically (optimization3.md item
 * 6). It is set before the first pthread_create, while one thread runs,
 * and never cleared: no other thread can see it change, and the new
 * thread sees it set, as it sees every value made before it started. */
extern int32_t __pas_threads_started;
void __pas_start_threads(void);

/* A reference count's increment, decrement (the new count) and acquire
 * load, atomic once threads have started. */
#define PAS_RC_INC(p)                                                       \
  (__pas_threads_started ? (void)__atomic_fetch_add((p), 1, __ATOMIC_RELAXED) \
                         : (void)(++*(p)))
#define PAS_RC_DEC(p)                                                       \
  (__pas_threads_started ? __atomic_sub_fetch((p), 1, __ATOMIC_ACQ_REL)     \
                         : --*(p))
#define PAS_RC_LOAD_ACQUIRE(p)                                              \
  (__pas_threads_started ? __atomic_load_n((p), __ATOMIC_ACQUIRE) : *(p))

/* Reports a runtime error ("Runtime error: <msg>") and exits with status 2. */
void __pas_runtime_error(const char *msg) __attribute__((noreturn));

/* A pooled source location, shared by the checks of one source position. */
typedef struct PasLoc {
  const char *file;
  int32_t line;
  int32_t column;
} PasLoc;

/* Reports "Runtime error: <msg> at <file>:<line>:<column>" (or without the
 * location when loc is NULL) and exits with status 2. The compiler shares
 * one file string per file and one PasLoc per source position. */
void __pas_runtime_error_at(const char *msg, const PasLoc *loc)
    __attribute__((noreturn));

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
 * A write statement whose arguments are simple: format into a thread-local
 * buffer, then write it with one fwrite and one lock. Discard drops an
 * unflushed buffer (an exception left the statement). */
void __pas_write_begin(PasFile *f);
void __pas_write_end(PasFile *f);
void __pas_write_discard(PasFile *f);
/* width <= 0 means no field width was given. Per ISO 7185 6.9.3, strings and
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
/* Stores src without counting it, moving its reference to dst. */
void __pas_astr_take(char **dst, char *src);
void __pas_astr_clear(char **p);
int32_t __pas_astr_length(const char *s);
char *__pas_astr_from_chars(const char *p, int64_t len);
char *__pas_astr_from_pstr(const unsigned char *p);
char *__pas_astr_from_char(char c);
char *__pas_astr_from_cstr(const char *p);
void __pas_astr_to_pstr(unsigned char *dst, int32_t maxlen, const char *s);
const char *__pas_astr_pchar(const char *s);
char *__pas_astr_concat(char *a, char *b);
/* A new string: a, the n characters at p, then b (a and b may be null). */
char *__pas_astr_join(const char *a, const char *p, int64_t n, const char *b);
int32_t __pas_astr_compare(const char *a, const char *b);
int32_t __pas_astr_eq(const char *a, const char *b);
int32_t __pas_astr_slice_compare(const char *a, int32_t la, const char *b,
                                 int32_t lb);
int32_t __pas_astr_pos(const char *sub, const char *s);
char *__pas_astr_copy(char *s, int32_t index, int32_t count);
char *__pas_astr_unique(char **p);
int32_t __pas_astr_bad_index(const char *s, int64_t index);
void __pas_astr_setlength(char **p, int32_t len);
void __pas_astr_delete(char **p, int32_t index, int32_t count);
void __pas_astr_insert(char *src, char **p, int32_t index);
/* s := s + x, in place when s is the only reference to its characters. */
void __pas_astr_append(char **p, char *x);
void __pas_astr_append_char(char **p, char c);
void __pas_astr_append2(char **p, char *a, char *b);
void __pas_astr_append_sc(char **p, char *x, char c);
void __pas_astr_append_cs(char **p, char c, char *x);
void __pas_astr_append_n(char **p, const char *const *ptrs, const int64_t *lens,
                         int64_t count);
/* s := x + s, in place when s is the only reference to its characters. */
void __pas_astr_prepend(char **p, char *x);
void __pas_astr_prepend_char(char **p, char c);
void __pas_str_setlength(unsigned char *s, int32_t maxlen, int32_t len);
void __pas_write_astr(PasFile *f, const char *s, int32_t width);
/* write(f, p) of a pchar: the C string p points to (nothing for nil, not
 * even padding). */
void __pas_write_cstr(PasFile *f, const char *s, int32_t width);
void __pas_read_astr(PasFile *f, char **dst);

/* Template strings, `a ${x} b`: each value is written, with write's
 * routines, to the text file __pas_tmpl_sink() returns, which collects the
 * characters. __pas_tmpl_add(s, t) returns a new ansistring (one reference,
 * the caller's): s, the characters collected, then t; nothing is collected
 * afterwards. s and t may be null. */
PasFile *__pas_tmpl_sink(void);
char *__pas_tmpl_add(const char *s, const char *t);

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
void __pas_freemem(void *p);
/* reallocmem (Turbo/FPC): realloc; size <= 0 frees and returns NULL. */
void *__pas_realloc(void *p, int64_t size);
/* Checked pointer arithmetic / indexing / subtraction helpers: return 0 if ok,
 * non-zero if p (or q) is nil, outside a live heap allocation, out of bounds,
 * or in a different allocation. */
int32_t __pas_ptr_bad_add(const void *p, int64_t delta, int64_t elem_size);
int32_t __pas_ptr_bad_index(const void *p, int64_t index, int64_t elem_size);
int32_t __pas_ptr_bad_diff(const void *p, const void *q);

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
 * interface table or NULL, [4] the depth of the class in its hierarchy,
 * [5] its ancestor display (the VMT of each class from the root down to
 * it), [6...] the virtual methods; [6] is Destroy. A class reference is a
 * VMT pointer. Methods take (static link, Self,
 * parameters...); a class method's Self is a class reference.
 * The interface table: [n, cleanup, iid1, methods1, ..., iidn, methodsn]
 * (n an integer): the interfaces the class implements and its method
 * tables for them; cleanup (or NULL) releases the COM interfaces held by
 * the class's own fields. An interface value is {object, method table}. */
extern void *const __pas_tobject_vmt[7];
/* COM interfaces: their objects are TInterfacedObjects, whose reference
 * count (FRefCount, an int32) follows the VMT pointer. TInterfacedObject
 * implements IUnknown (iid __pas_iunknown_iid). */
extern void *const __pas_tinterfacedobject_vmt[7];
extern void *const __pas_tsharedobject_vmt[7];
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
/* Closures (objfpc `reference to` types): a value is {function, frame},
 * like a procedural value's {function, static link}; the frame is NULL or
 * the frame record of a routine on the heap, preceded by a header (a
 * reference count and the function that finalizes the record). */
typedef struct PasClosure {
  void *fn;
  void *frame;
} PasClosure;
/* A zeroed frame record of size bytes, referenced once (by its routine). */
void *__pas_frame_new(int64_t size, void (*fini)(void *frame));
/* One more / one fewer reference to frame (NULL: nothing); the last one
 * finalizes and frees it. */
void __pas_frame_addref(void *frame);
void __pas_frame_release(void *frame);
/* *dst := {fn, frame}: references the new frame, then releases the old. */
void __pas_closure_assign(PasClosure *dst, void *fn, void *frame);
/* Releases *p's frame and sets *p to nil. */
void __pas_closure_clear(PasClosure *p);
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
extern void *const __pas_exception_vmt[7];
/* EExternal = class(Exception), EIntError = class(EExternal),
 * EDivByZero = class(EIntError) (SysUtils). */
extern void *const __pas_eexternal_vmt[7];
extern void *const __pas_einterror_vmt[7];
extern void *const __pas_edivbyzero_vmt[7];
/* EConvertError = class(Exception) (SysUtils). */
extern void *const __pas_econverterror_vmt[7];
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
/* Integer overflow (objfpc): raises a new EIntOverflow, with the message
 * 'Arithmetic overflow'. */
void __pas_raise_overflow(const char *where) __attribute__((noreturn));
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

/*===- Operating system (sys.c) ---------------------------------------------===*/
const char *__pas_sys_strerror(void);
int32_t __pas_sys_file_exists(const char *path);
int32_t __pas_sys_dir_exists(const char *path);
int32_t __pas_sys_mkdir(const char *path);
int32_t __pas_sys_rmdir(const char *path);
int32_t __pas_sys_chdir(const char *path);
int32_t __pas_sys_unlink(const char *path);
int32_t __pas_sys_getcwd(char *buf, int32_t size);
void __pas_sys_format_double(char *buf, int32_t size, double value,
                             int32_t precision);
int32_t __pas_sys_parse_double(const char *s, double *value);
void __pas_sys_localtime(int32_t *fields);
int64_t __pas_sys_tick_ms(void);
void __pas_sys_sleep(int32_t ms);


/*===- Iterators (async.c) ---------------------------------------------------===*/
/* An iterator routine's call is a generator: made by __pas_gen_new (values
 * of type info; args_size bytes of arguments, which args_fini finalizes;
 * entry runs its body), counted, run by __pas_gen_next (1: a value in
 * __pas_gen_value; 0: the end). In its body, yield stores into
 * __pas_gen_slot() and calls __pas_gen_yield(). */
typedef struct PasGen PasGen;
PasGen *__pas_gen_new(const PasDynInfo *info, int64_t args_size,
                      void (*entry)(PasGen *), void (*args_fini)(void *));
void *__pas_gen_args(PasGen *g);
void *__pas_gen_value(PasGen *g);
int32_t __pas_gen_next(PasGen *g);
void *__pas_gen_slot(void);
void __pas_gen_yield(void);
void __pas_gen_addref(PasGen *g);
void __pas_gen_release(PasGen *g);
void __pas_gen_assign(PasGen **dst, PasGen *src);
void __pas_gen_clear(PasGen **p);

/*===- Tasks (async.c) ------------------------------------------------------===*/
/* An async routine's call is a task: made by __pas_task_new (result: of
 * type info, or none; args_size bytes of arguments, which args_fini
 * finalizes when it ends; entry runs it), counted, and started here
 * (__pas_task_start), on the pool (__pas_task_run_pool), or as a child of
 * the innermost scope (__pas_task_spawn). __pas_task_await waits for it
 * (in a task: suspended; outside: running this thread's tasks meanwhile)
 * and raises its exception if it failed; then its result is at
 * __pas_task_result. */
typedef struct PasTask PasTask;
typedef struct PasToken PasToken;
PasTask *__pas_task_new(const PasDynInfo *info, int64_t args_size,
                        void (*entry)(PasTask *), void (*args_fini)(void *));
void *__pas_task_result(PasTask *t);
void *__pas_task_args(PasTask *t);
void __pas_task_addref(PasTask *t);
void __pas_task_release(PasTask *t);
void __pas_task_assign(PasTask **dst, PasTask *src);
void __pas_task_clear(PasTask **p);
void __pas_task_start(PasTask *t);
void __pas_task_spawn(PasTask *t);
void __pas_task_run_pool(PasTask *t);
void __pas_task_await(PasTask *t);
int32_t __pas_task_done(PasTask *t);
void __pas_task_detach(PasTask *t, void *handler_fn, void *handler_link);
void __pas_task_sleep(int32_t ms);
/* The size of task (and iterator) stacks; set it before the first task or
 * iterator starts (1), or not anymore (0). Also P3_TASK_STACK_SIZE. */
int64_t __pas_task_stack_size(void);
int32_t __pas_task_set_stack_size(int64_t bytes);
/* Guard pages below task stacks (1, the default) or not (0: a slab of
 * stacks is one mapping; overflow is caught by a canary); set it before the
 * first task or iterator (1), or not anymore (0). Also P3_TASK_STACK_GUARD. */
int32_t __pas_task_stack_guard(void);
int32_t __pas_task_set_stack_guard(int32_t on);
/* Parks the current task until fd is readable (want_write 0) or writable
 * (want_write 1), through one shared poller thread. Its result is the
 * bitmask of what fired: 1 readable, 2 writable, 4 failed (POLLERR/POLLHUP
 * on the poll backends, a failed poll completion on io_uring). Wakes the
 * task with EOperationCancelled if its token is cancelled. For unit
 * netasync. */
int32_t __pas_task_wait_fd(int32_t fd, int32_t want_write);
/* Same, with a deadline (0: wait as long as it takes). DeadlineMs counts
 * from the call; the result's bit 8 (NW_TIMEOUT) says the deadline expired
 * with nothing else to report, and — unlike a cancellation — the wait
 * record completes and is freed normally. */
int32_t __pas_task_wait_fd2(int32_t fd, int32_t want_write, int32_t deadline_ms);
/* A syscall on fd succeeded without parking: wake the waits parked on it
 * (in that direction). */
void __pas_fd_wake_peers(int32_t fd, int32_t want_write);
/* Marks fd blocking (on 0) or non-blocking, through the C runtime (plain C
   can call the variadic fcntl; calls emitted by p3 threads cannot) */
void __pas_fd_nonblock(int32_t fd, int32_t on);
/* Starts the netasync poller thread (unit netasync calls it at init) */
void __pas_fd_poller_start(void);
/* The wait records a cancelled io_uring wait abandoned by design (~72 B
 * each; the poll backends retire cleanly). For unit netasync's leak tests. */
int64_t __pas_net_waits_abandoned(void);
/* The netasync poller backend in place: 0 kevent, 1 epoll, 2 io_uring.
 * For benchmark reports and logs. */
int32_t __pas_net_fd_backend(void);
/* Parks the current task until recv(fd, buf, len) completes; result: the
 * count, 0 at connection end, or negative (-errno). For unit netasync. */
int64_t __pas_task_recv(int32_t fd, void *buf, int32_t len);
/* Parks the current task until send(fd, buf, len) completes; result: the
 * count sent or negative (-errno). For unit netasync. */
int64_t __pas_task_send(int32_t fd, void *buf, int32_t len);
PasTask *__pas_when_all(PasTask *const *tasks, int64_t n, const PasDynInfo *result_info,
                        const PasDynInfo *elem);
PasTask *__pas_when_any(PasTask *const *tasks, int64_t n, const PasDynInfo *int_info);
/* scope [token] ... end */
void *__pas_scope_enter(PasToken *given);
void __pas_scope_leave(void *scope, int32_t raised);
/* Cancellation tokens (counted). */
PasToken *__pas_token_new(PasToken *parent);
PasToken *__pas_token_with_timeout(PasToken *parent, int32_t ms);
void __pas_token_addref(PasToken *k);
void __pas_token_release(PasToken *k);
void __pas_token_assign(PasToken **dst, PasToken *src);
void __pas_token_clear(PasToken **p);
void __pas_token_cancel(PasToken *k);
int32_t __pas_token_cancelled(PasToken *k);
void __pas_token_check(PasToken *k);
void __pas_token_make(PasToken *parent, int32_t ms, PasToken **dst);
/* Mutexes and wait queues (condition variables that tasks can wait on). */
void *__pas_mutex_new(void);
void __pas_mutex_free(void *m);
void __pas_mutex_lock(void *m);
void __pas_mutex_unlock(void *m);
void *__pas_waitq_new(void);
void __pas_waitq_free(void *q);
void __pas_waitq_wait(void *q, void *m);
void __pas_waitq_signal(void *q);
void __pas_waitq_broadcast(void *q);
/* EOperationCancelled = class(Exception): a cancelled task's wait. */
extern void *const __pas_eoperationcancelled_vmt[7];
/* Raises a new exception of class vmt (Exception's layout) with text. */
void __pas_raise_message(void *const *vmt, const char *text) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* PASCALRT_H */
