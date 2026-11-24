/*
 * The operating system side of p3's unit library (rtl/): what differs
 * between Linux, macOS and 64-bit Windows, behind one set of functions.
 * Paths are NUL-terminated; functions returning int32_t return 1 on success
 * and 0 on failure.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "pascalrt.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
typedef struct _stat64 StatBuf;
#define STAT(p, b) _stat64((p), (b))
#define IS_DIR(m) (((m)&_S_IFMT) == _S_IFDIR)
#else
typedef struct stat StatBuf;
#define STAT(p, b) stat((p), (b))
#define IS_DIR(m) S_ISDIR(m)
#endif

#if defined(_WIN32) && defined(__x86_64__)
/* The _setjmp/_longjmp pair of compiled code and of the runtime on 64-bit
 * Windows, named __pas_setjmp/__pas_longjmp (MinGW's C library has a
 * _setjmp of its own): every callee-saved register of the Microsoft x64 convention, in
 * a buffer without alignment requirements. Control moves directly, without
 * structured exception unwinding. */
__asm__(".text\n"
        ".globl __pas_setjmp\n"
        ".def __pas_setjmp; .scl 2; .type 32; .endef\n"
        "__pas_setjmp:\n"
        "  movq %rbx, 0(%rcx)\n"
        "  movq %rbp, 8(%rcx)\n"
        "  movq %rdi, 16(%rcx)\n"
        "  movq %rsi, 24(%rcx)\n"
        "  movq %r12, 32(%rcx)\n"
        "  movq %r13, 40(%rcx)\n"
        "  movq %r14, 48(%rcx)\n"
        "  movq %r15, 56(%rcx)\n"
        "  leaq 8(%rsp), %rdx\n"
        "  movq %rdx, 64(%rcx)\n"
        "  movq (%rsp), %rdx\n"
        "  movq %rdx, 72(%rcx)\n"
        "  movups %xmm6, 80(%rcx)\n"
        "  movups %xmm7, 96(%rcx)\n"
        "  movups %xmm8, 112(%rcx)\n"
        "  movups %xmm9, 128(%rcx)\n"
        "  movups %xmm10, 144(%rcx)\n"
        "  movups %xmm11, 160(%rcx)\n"
        "  movups %xmm12, 176(%rcx)\n"
        "  movups %xmm13, 192(%rcx)\n"
        "  movups %xmm14, 208(%rcx)\n"
        "  movups %xmm15, 224(%rcx)\n"
        "  stmxcsr 240(%rcx)\n"
        "  fnstcw 244(%rcx)\n"
        "  xorl %eax, %eax\n"
        "  ret\n"
        ".globl __pas_longjmp\n"
        ".def __pas_longjmp; .scl 2; .type 32; .endef\n"
        "__pas_longjmp:\n"
        "  movl %edx, %eax\n"
        "  testl %eax, %eax\n"
        "  jnz 1f\n"
        "  movl $1, %eax\n"
        "1:\n"
        "  movq 0(%rcx), %rbx\n"
        "  movq 8(%rcx), %rbp\n"
        "  movq 16(%rcx), %rdi\n"
        "  movq 24(%rcx), %rsi\n"
        "  movq 32(%rcx), %r12\n"
        "  movq 40(%rcx), %r13\n"
        "  movq 48(%rcx), %r14\n"
        "  movq 56(%rcx), %r15\n"
        "  movups 80(%rcx), %xmm6\n"
        "  movups 96(%rcx), %xmm7\n"
        "  movups 112(%rcx), %xmm8\n"
        "  movups 128(%rcx), %xmm9\n"
        "  movups 144(%rcx), %xmm10\n"
        "  movups 160(%rcx), %xmm11\n"
        "  movups 176(%rcx), %xmm12\n"
        "  movups 192(%rcx), %xmm13\n"
        "  movups 208(%rcx), %xmm14\n"
        "  movups 224(%rcx), %xmm15\n"
        "  ldmxcsr 240(%rcx)\n"
        "  fldcw 244(%rcx)\n"
        "  movq 64(%rcx), %rsp\n"
        "  jmpq *72(%rcx)\n");
#endif

#if defined(_WIN32) || defined(__APPLE__)
/* round() compiles to llvm.roundeven, a call to this C23 function on CPUs
 * without the instruction; these C libraries do not have it. */
double roundeven(double x);
double roundeven(double x) { return rint(x); }
#endif

const char *__pas_sys_strerror(void) { return strerror(errno); }

int32_t __pas_sys_file_exists(const char *path) {
  StatBuf st;
  return STAT(path, &st) == 0 && !IS_DIR(st.st_mode);
}

int32_t __pas_sys_dir_exists(const char *path) {
  StatBuf st;
  return STAT(path, &st) == 0 && IS_DIR(st.st_mode);
}

int32_t __pas_sys_mkdir(const char *path) {
#ifdef _WIN32
  return _mkdir(path) == 0;
#else
  return mkdir(path, 0777) == 0;
#endif
}

int32_t __pas_sys_rmdir(const char *path) {
#ifdef _WIN32
  return _rmdir(path) == 0;
#else
  return rmdir(path) == 0;
#endif
}

int32_t __pas_sys_chdir(const char *path) {
#ifdef _WIN32
  return _chdir(path) == 0;
#else
  return chdir(path) == 0;
#endif
}

int32_t __pas_sys_unlink(const char *path) {
#ifdef _WIN32
  return _unlink(path) == 0;
#else
  return unlink(path) == 0;
#endif
}

int32_t __pas_sys_getcwd(char *buf, int32_t size) {
#ifdef _WIN32
  return _getcwd(buf, size) != NULL;
#else
  return getcwd(buf, (size_t)size) != NULL;
#endif
}

void __pas_sys_format_double(char *buf, int32_t size, double value,
                             int32_t precision) {
  snprintf(buf, (size_t)size, "%.*e", precision - 1, value);
}

int32_t __pas_sys_parse_double(const char *s, double *value) {
  char *end;
  *value = strtod(s, &end);
  return end != s && *end == '\0';
}

void __pas_sys_localtime(int32_t *fields) {
  int32_t ms;
  struct tm tm;
#ifdef _WIN32
  SYSTEMTIME now;
  GetLocalTime(&now);
  fields[0] = now.wYear;
  fields[1] = now.wMonth;
  fields[2] = now.wDay;
  fields[3] = now.wHour;
  fields[4] = now.wMinute;
  fields[5] = now.wSecond;
  fields[6] = now.wMilliseconds;
  (void)ms;
  (void)tm;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);
  ms = (int32_t)(ts.tv_nsec / 1000000);
  fields[0] = tm.tm_year + 1900;
  fields[1] = tm.tm_mon + 1;
  fields[2] = tm.tm_mday;
  fields[3] = tm.tm_hour;
  fields[4] = tm.tm_min;
  fields[5] = tm.tm_sec;
  fields[6] = ms;
#endif
}

int64_t __pas_sys_tick_ms(void) {
#ifdef _WIN32
  return (int64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* SysUtils.Sleep: in a task it waits (cancellable) without holding its
 * thread; elsewhere the thread runs its tasks meanwhile (async.c). */
void __pas_sys_sleep(int32_t ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  __pas_task_sleep(ms);
#endif
}
