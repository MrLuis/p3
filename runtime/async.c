/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/*
 * Tasks: async functions, await, structured concurrency (scopes), the
 * thread pool, cancellation tokens, timers, and the mutexes and wait queues
 * the unit `threading` builds channels on.
 *
 * A task runs on a stack of its own (a coroutine: ucontext), so the code of
 * an async function is ordinary code: `try` blocks, exceptions (setjmp and
 * longjmp, each task with its own chain of exception frames) and nested
 * calls work across `await`. A task belongs to the thread it was started
 * on (its home), which alone runs it: when it waits, its thread goes on
 * with other work, and when what it waits for is done it is queued on its
 * home again. Threads run their queued tasks in a loop: pool threads
 * forever, other threads (the main program's) while they wait for a task
 * (Wait, a scope's end, Sleep).
 *
 * One lock (__pas_async_mutex) protects all the shared state here: tasks'
 * states and waiters, queues, scopes, tokens, timers, wait queues.
 */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* ucontext */
#endif
#include "pascalrt.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
/* Windows (MinGW-w64): threads from winpthreads, stacks from VirtualAlloc,
 * sockets from Winsock and their per-thread pollers from wepoll (epoll over
 * the AFD driver's polling). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "../wepoll/wepoll.h"
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <ucontext.h>
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#include <sys/event.h>
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define PAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PAS_ASAN 1
#endif
#endif
#ifdef PAS_ASAN
#include <sanitizer/common_interface_defs.h>
#endif
/* ThreadSanitizer: each task is a fiber of its own, switched to by hand
 * (TSan cannot follow a stack switch otherwise). */
#if defined(__SANITIZE_THREAD__)
#define PAS_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PAS_TSAN 1
#endif
#endif
#ifdef PAS_TSAN
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void *fiber);
void __tsan_switch_to_fiber(void *fiber, unsigned flags);
#endif

#ifdef _WIN32
/* the runtime's own pair on Windows (sys.c) */
int __pas_setjmp(jmp_buf env) __attribute__((returns_twice));
#define _setjmp __pas_setjmp
#else
int _setjmp(jmp_buf env);
#endif
extern _Thread_local PasExcFrame *__pas_exc_top;

/* The page size and the number of processors. */
static size_t page_size(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
#else
  return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

static long cpu_count(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (long)si.dwNumberOfProcessors;
#else
  return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

/*===- Task context switch --------------------------------------------------===*/

/* A task switch saves the callee-saved registers on the running stack and
 * loads the other stack's — what a function call would preserve anyway, and
 * no syscall. swapcontext also saves and restores the signal mask, one
 * sigprocmask syscall per switch (two per park and wake); the runtime never
 * changes masks per task, so the fast switch leaves the mask alone.
 * aarch64 and x86_64 have it; other targets keep ucontext. */
#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(PAS_UCONTEXT)
#define PAS_FAST_CTX 1
typedef struct PasCtx {
  void *sp;   /* the saved stack pointer, its registers just above */
} PasCtx;

/* Saves the running context's registers and stack pointer in *save_sp and
 * continues the one whose stack pointer is load_sp. */
void __pas_ctx_switch(void **save_sp, void *load_sp);
/* First code of a new context: calls the function made in its frame. */
void __pas_ctx_start(void);

#if defined(__APPLE__)
#define PAS_ASM_SYM(x) "_" #x
#else
#define PAS_ASM_SYM(x) #x
#endif

#if defined(__aarch64__)
/* frame (176 bytes): x19..x28, x29, x30, then d8..d15 */
__asm__(".text\n"
        ".p2align 2\n"
        ".globl " PAS_ASM_SYM(__pas_ctx_switch) "\n"
        PAS_ASM_SYM(__pas_ctx_switch) ":\n"
        "  sub sp, sp, #176\n"
        "  stp x19, x20, [sp, #0]\n"
        "  stp x21, x22, [sp, #16]\n"
        "  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n"
        "  stp x27, x28, [sp, #64]\n"
        "  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n"
        "  stp d10, d11, [sp, #112]\n"
        "  stp d12, d13, [sp, #128]\n"
        "  stp d14, d15, [sp, #144]\n"
        "  mov x9, sp\n"
        "  str x9, [x0]\n"
        "  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n"
        "  ldp x21, x22, [sp, #16]\n"
        "  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n"
        "  ldp x27, x28, [sp, #64]\n"
        "  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n"
        "  ldp d10, d11, [sp, #112]\n"
        "  ldp d12, d13, [sp, #128]\n"
        "  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #176\n"
        "  ret\n"
        ".p2align 2\n"
        ".globl " PAS_ASM_SYM(__pas_ctx_start) "\n"
        PAS_ASM_SYM(__pas_ctx_start) ":\n"
        "  mov x29, #0\n"
        "  mov x30, #0\n"
        "  blr x19\n"
        "  brk #0\n");
#define PAS_CTX_FRAME 176
#elif defined(_WIN32)
/* Windows x64: frame (272 bytes): xmm6..xmm15, mxcsr + x87 control word,
 * the thread's stack bounds in its TIB (StackBase, StackLimit,
 * DeallocationStack: Windows checks the stack pointer against them), a
 * spare slot, r15, r14, r13, r12, rsi, rdi, rbx, rbp, then the return
 * address. Arguments in rcx and rdx. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl " PAS_ASM_SYM(__pas_ctx_switch) "\n"
        PAS_ASM_SYM(__pas_ctx_switch) ":\n"
        "  pushq %rbp\n"
        "  pushq %rbx\n"
        "  pushq %rdi\n"
        "  pushq %rsi\n"
        "  pushq %r12\n"
        "  pushq %r13\n"
        "  pushq %r14\n"
        "  pushq %r15\n"
        "  subq $200, %rsp\n"
        "  movups %xmm6, 0(%rsp)\n"
        "  movups %xmm7, 16(%rsp)\n"
        "  movups %xmm8, 32(%rsp)\n"
        "  movups %xmm9, 48(%rsp)\n"
        "  movups %xmm10, 64(%rsp)\n"
        "  movups %xmm11, 80(%rsp)\n"
        "  movups %xmm12, 96(%rsp)\n"
        "  movups %xmm13, 112(%rsp)\n"
        "  movups %xmm14, 128(%rsp)\n"
        "  movups %xmm15, 144(%rsp)\n"
        "  stmxcsr 160(%rsp)\n"
        "  fnstcw 164(%rsp)\n"
        "  movq %gs:0x08, %rax\n"
        "  movq %rax, 168(%rsp)\n"
        "  movq %gs:0x10, %rax\n"
        "  movq %rax, 176(%rsp)\n"
        "  movq %gs:0x1478, %rax\n"
        "  movq %rax, 184(%rsp)\n"
        "  movq %rsp, (%rcx)\n"
        "  movq %rdx, %rsp\n"
        "  movq 168(%rsp), %rax\n"
        "  movq %rax, %gs:0x08\n"
        "  movq 176(%rsp), %rax\n"
        "  movq %rax, %gs:0x10\n"
        "  movq 184(%rsp), %rax\n"
        "  movq %rax, %gs:0x1478\n"
        "  ldmxcsr 160(%rsp)\n"
        "  fldcw 164(%rsp)\n"
        "  movups 0(%rsp), %xmm6\n"
        "  movups 16(%rsp), %xmm7\n"
        "  movups 32(%rsp), %xmm8\n"
        "  movups 48(%rsp), %xmm9\n"
        "  movups 64(%rsp), %xmm10\n"
        "  movups 80(%rsp), %xmm11\n"
        "  movups 96(%rsp), %xmm12\n"
        "  movups 112(%rsp), %xmm13\n"
        "  movups 128(%rsp), %xmm14\n"
        "  movups 144(%rsp), %xmm15\n"
        "  addq $200, %rsp\n"
        "  popq %r15\n"
        "  popq %r14\n"
        "  popq %r13\n"
        "  popq %r12\n"
        "  popq %rsi\n"
        "  popq %rdi\n"
        "  popq %rbx\n"
        "  popq %rbp\n"
        "  ret\n"
        ".p2align 4\n"
        ".globl " PAS_ASM_SYM(__pas_ctx_start) "\n"
        PAS_ASM_SYM(__pas_ctx_start) ":\n"
        "  andq $-16, %rsp\n"
        "  subq $32, %rsp\n"  /* the callee's shadow space */
        "  xorl %ebp, %ebp\n"
        "  callq *%rbx\n"
        "  ud2\n");
#define PAS_CTX_FRAME 272
#else
/* frame (64 bytes): mxcsr + x87 control word, r15, r14, r13, r12, rbx,
 * rbp, then the return address */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl " PAS_ASM_SYM(__pas_ctx_switch) "\n"
        PAS_ASM_SYM(__pas_ctx_switch) ":\n"
        "  pushq %rbp\n"
        "  pushq %rbx\n"
        "  pushq %r12\n"
        "  pushq %r13\n"
        "  pushq %r14\n"
        "  pushq %r15\n"
        "  subq $8, %rsp\n"
        "  stmxcsr (%rsp)\n"
        "  fnstcw 4(%rsp)\n"
        "  movq %rsp, (%rdi)\n"
        "  movq %rsi, %rsp\n"
        "  ldmxcsr (%rsp)\n"
        "  fldcw 4(%rsp)\n"
        "  addq $8, %rsp\n"
        "  popq %r15\n"
        "  popq %r14\n"
        "  popq %r13\n"
        "  popq %r12\n"
        "  popq %rbx\n"
        "  popq %rbp\n"
        "  ret\n"
        ".p2align 4\n"
        ".globl " PAS_ASM_SYM(__pas_ctx_start) "\n"
        PAS_ASM_SYM(__pas_ctx_start) ":\n"
        "  andq $-16, %rsp\n"
        "  xorl %ebp, %ebp\n"
        "  callq *%rbx\n"
        "  ud2\n");
#define PAS_CTX_FRAME 64
#endif

#ifdef _WIN32
static size_t stack_floor_gap(void);
#endif

/* A context that starts in fn on [stack, stack + size). */
static void ctx_make(PasCtx *c, void *stack, size_t size, void (*fn)(void)) {
  uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
  uintptr_t *f = (uintptr_t *)(top - 16 - PAS_CTX_FRAME);
  memset(f, 0, PAS_CTX_FRAME);
#if defined(__aarch64__)
  f[0] = (uintptr_t)fn;                  /* x19 */
  f[11] = (uintptr_t)__pas_ctx_start;    /* x30: where the first ret goes */
#elif defined(_WIN32)
  ((uint32_t *)f)[40] = 0x1F80;          /* mxcsr: the default */
  ((uint32_t *)f)[41] = 0x037F;          /* x87 control word: the default */
  f[21] = top;                           /* StackBase */
  f[22] = (uintptr_t)stack;              /* StackLimit */
  /* DeallocationStack: the guard page (or gap) below is part of the
     stack, so an overflow into it is a stack overflow to Windows */
  f[23] = (uintptr_t)stack - stack_floor_gap();
  f[31] = (uintptr_t)fn;                 /* rbx */
  f[33] = (uintptr_t)__pas_ctx_start;    /* return address */
#else
  ((uint32_t *)f)[0] = 0x1F80;           /* mxcsr: the default */
  ((uint32_t *)f)[1] = 0x037F;           /* x87 control word: the default */
  f[5] = (uintptr_t)fn;                  /* rbx */
  f[7] = (uintptr_t)__pas_ctx_start;     /* return address */
#endif
  c->sp = f;
}
#else
typedef ucontext_t PasCtx;
#endif

/* Locking. No lock covers everything: each piece of shared state has its
 * own, and the hot paths of a thread's own tasks touch no lock another
 * thread contends for.
 *
 *   __pas_async_mutex (LOCK)  process-wide state only: the timer list, the
 *                             pool's start, the global stack cache, the
 *                             io_uring ring and its records
 *   PasToken.lk               a token's observers (cancelled: atomic)
 *   PasTask.lk                a task's awaiters, exception and handler
 *   PasWaitQueue.lk           a wait queue's waiters
 *   fd_stripe(fd)             the parked waits of a descriptor (fd_parked)
 *   PasContext.qmu            a thread's ready queue and sleep state: a
 *                             leaf, taken by wake() under any of the above
 *
 * Order: LOCK, then a token, task or wait-queue lock (never two of these
 * at once), then a fd stripe, then qmu. A task's state is atomic: only its
 * home thread runs it (NEW/READY -> RUNNING -> WAITING), and a waker moves
 * it WAITING -> READY with a compare-and-swap before queueing it.
 *
 * A waiter node (PasWaiter) is on one list at a time, under that list's
 * lock; its owner unlinks it under the same lock before leaving the wait,
 * so a waker holding the lock may always touch it. */

/* (Variables have external linkage: see utils/gen-runtime-ir.py.) */
pthread_mutex_t __pas_async_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&__pas_async_mutex)
#define UNLOCK() pthread_mutex_unlock(&__pas_async_mutex)

/* Small spin locks for the per-object state above: the critical sections
 * are a few list operations long. */
static inline void cpu_relax(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#elif defined(__x86_64__)
  __asm__ __volatile__("pause");
#endif
}

static inline void spin_lock(int *l) {
  for (int n = 0;;) {
    if (!__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
      return;
    while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
      if (++n < 64)
        cpu_relax();
      else
        sched_yield();
    }
  }
}

static inline void spin_unlock(int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

enum { TS_NEW, TS_RUNNING, TS_WAITING, TS_READY, TS_DONE, TS_FAILED };
enum { WAKE_NORMAL, WAKE_CANCEL };

typedef struct PasContext PasContext;
typedef struct PasScope PasScope;

/* Who waits: a task (suspended), or a thread outside any task (blocked on
 * its context's condition). */
typedef struct PasWaiter {
  struct PasWaiter *next;
  PasTask *task;
  PasContext *thread;
  int signaled; /* (set when woken) */
} PasWaiter;

struct PasToken {
  int64_t refs;
  int lk;        /* its observers */
  int cancelled; /* (atomic) */
  struct PasToken *parent, *parent2; /* cancelled with either (counted) */
  PasWaiter *waiters;      /* tasks waiting while they observe it */
};

struct PasTask {
  int64_t refs;
  int state;     /* TS_* (atomic) */
  int lk;        /* its waiters, exception, handler */
  int wake;      /* why it was woken: WAKE_* (io_uring waits) */
  int detached;
  const PasDynInfo *info; /* the result's type, or NULL */
  void (*entry)(PasTask *);
  void (*args_fini)(void *args);
  int64_t result_size;
  PasContext *home;
  PasCtx ctx;
  void *stack;
  size_t stack_size;
  PasCtx *resumer;              /* where it goes when it waits or ends */
#ifdef PAS_TSAN
  void *tsan_fiber, *tsan_resumer;
#endif
  const void *resumer_bottom;   /* (the resumer's stack, for ASan) */
  size_t resumer_size;
  void *fake_stack;
  PasExcFrame *exc_top;         /* its exception frames, while switched out */
  void *exception;              /* TS_FAILED: the exception, until raised */
  const char *exc_where;        /* (where it was raised) */
  PasWaiter *waiters;           /* those waiting for it to end */
  PasTask *next_ready;
  PasScope *scope;              /* the scope it was spawned into */
  PasScope *scopes;             /* the innermost scope open in it */
  PasToken *token;              /* the token it observes (counted) */
  PasClosure on_error;          /* detached: its error handler */
  char data[];                  /* the result, then the arguments */
};

struct PasContext {
  pthread_mutex_t qmu;  /* head/tail, sleeping, the cv: a leaf lock */
  pthread_cond_t cv;
  PasTask *head, *tail; /* ready */
  PasTask *current;     /* the task running on this thread, or NULL */
  PasTask *starting;    /* (handed to task_main) */
  PasScope *scopes;     /* scopes open outside any task */
  int is_pool;
  /* The thread's own poller (kqueue/epoll), created by its first fd wait:
   * from then on the thread blocks in it instead of on cv, so socket
   * readiness wakes the tasks right on the thread that runs them, in
   * batches, with no poller thread in between (the event-loop design of
   * a hand-written server). -1 before. */
  int pfd;
  int sleeping;         /* blocked in pfd: a post from elsewhere pokes it */
  int wakefd;           /* Linux: the eventfd those pokes write; Windows:
                           the loopback socket they send a byte to */
#ifdef _WIN32
  HANDLE ep;            /* Windows: the wepoll port (pfd is then 0) */
#endif
  uint8_t *fdtab;       /* per descriptor: FDT_* (what is armed directly) */
  int fdcap;
  struct PasFdWait **dheap; /* its waits with a deadline, a min-heap */
  int dcount, dcap;
};

#define PAS_WAKE_MARK ((void *)1) /* the poke's registration */

/* Under c->qmu: there is work for c's thread (a task was queued, or a
 * thread waiter was woken). A thread blocked in its poller is poked —
 * only when it sleeps, so a busy thread pays no syscall. */
static void ctx_signal_locked(PasContext *c) {
  if (c->pfd >= 0) {
    if (c->sleeping) {
      c->sleeping = 0;
#if defined(_WIN32)
      /* a byte to the poller's loopback socket */
      send((SOCKET)(intptr_t)c->wakefd, "", 1, 0);
#elif defined(__linux__)
      uint64_t one = 1;
      ssize_t r = write(c->wakefd, &one, sizeof one);
      (void)r;
#else
      struct kevent k;
      /* (a change rewrites the event's udata: keep the marker) */
      EV_SET(&k, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, PAS_WAKE_MARK);
      kevent(c->pfd, &k, 1, NULL, 0, NULL);
#endif
    }
    return;
  }
  pthread_cond_broadcast(&c->cv);
}

static void ctx_signal(PasContext *c) {
  pthread_mutex_lock(&c->qmu);
  ctx_signal_locked(c);
  pthread_mutex_unlock(&c->qmu);
}

struct PasScope {
  PasScope *outer;
  PasToken *token;       /* its own (a child of the one it was given) */
  PasToken *saved_token; /* the owner's token before it */
  PasTask **children;
  int64_t count, cap;
};

_Thread_local PasContext *__pas_async_ctx;
PasContext **__pas_async_pool;
int __pas_async_pool_size;
int __pas_async_pool_next;

extern void *const __pas_eoperationcancelled_vmt[7];
void __pas_raise_message(void *const *vmt, const char *text)
    __attribute__((noreturn));

/*===- Contexts and the ready queues ----------------------------------------===*/

static PasContext *new_context(int is_pool) {
  PasContext *c = calloc(1, sizeof *c);
  if (!c)
    __pas_runtime_error("out of memory for a thread's tasks");
  pthread_mutex_init(&c->qmu, NULL);
  pthread_cond_init(&c->cv, NULL);
  c->is_pool = is_pool;
  c->pfd = -1;
  c->wakefd = -1;
  return c;
}

static PasContext *context(void) {
  if (!__pas_async_ctx)
    __pas_async_ctx = new_context(0);
  return __pas_async_ctx;
}

/* t joins its home's ready queue. */
static void post(PasTask *t) {
  PasContext *c = t->home;
  pthread_mutex_lock(&c->qmu);
  t->next_ready = NULL;
  if (c->tail)
    c->tail->next_ready = t;
  else
    c->head = t;
  c->tail = t;
  ctx_signal_locked(c);
  pthread_mutex_unlock(&c->qmu);
}

/* Under c->qmu. */
static PasTask *pop(PasContext *c) {
  PasTask *t = c->head;
  if (t) {
    c->head = t->next_ready;
    if (!c->head)
      c->tail = NULL;
  }
  return t;
}

/* Under the lock of the list w is on: w is woken — signaled, and its task
 * queued if it waits (a task not waiting yet sees the signal itself, see
 * park), or its thread told. */
static void wake(PasWaiter *w, int why) {
  PasTask *t = w->task;
  PasContext *th = w->thread;
  __atomic_store_n(&w->signaled, 1, __ATOMIC_SEQ_CST);
  if (t) {
    int expected = TS_WAITING;
    if (!__atomic_compare_exchange_n(&t->state, &expected, TS_READY, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return;
    t->wake = why;
    post(t);
  } else {
    ctx_signal(th);
  }
}

static int task_state(PasTask *t) { return __atomic_load_n(&t->state, __ATOMIC_ACQUIRE); }

static void add_waiter(PasWaiter **list, PasWaiter *w) {
  w->next = *list;
  *list = w;
}

static void remove_waiter(PasWaiter **list, PasWaiter *w) {
  for (PasWaiter **p = list; *p; p = &(*p)->next)
    if (*p == w) {
      *p = w->next;
      return;
    }
}

/*===- Tokens ----------------------------------------------------------------===*/

void __pas_token_addref(PasToken *k) {
  if (k)
    __atomic_fetch_add(&k->refs, 1, __ATOMIC_RELAXED);
}

/* A new token, cancelled with parent and with parent2 (either may be nil). */
static PasToken *token_new2(PasToken *parent, PasToken *parent2) {
  PasToken *k = calloc(1, sizeof *k);
  if (!k)
    __pas_runtime_error("out of memory for a cancellation token");
  k->refs = 1;
  k->parent = parent;
  k->parent2 = parent2;
  __pas_token_addref(parent);
  __pas_token_addref(parent2);
  return k;
}

PasToken *__pas_token_new(PasToken *parent) { return token_new2(parent, NULL); }

void __pas_token_release(PasToken *k) {
  if (k && __atomic_sub_fetch(&k->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    PasToken *p = k->parent, *p2 = k->parent2;
    free(k);
    __pas_token_release(p);
    __pas_token_release(p2);
  }
}

void __pas_token_assign(PasToken **dst, PasToken *src) {
  __pas_token_addref(src);
  PasToken *old = *dst;
  *dst = src;
  __pas_token_release(old);
}

void __pas_token_clear(PasToken **p) {
  PasToken *old = *p;
  *p = NULL;
  __pas_token_release(old);
}

/* k or an ancestor is cancelled (lock-free: the flags only go 0 -> 1 and
 * the ancestors never change). */
static int cancelled_now(PasToken *k) {
  return k && (__atomic_load_n(&k->cancelled, __ATOMIC_SEQ_CST) ||
               cancelled_now(k->parent) || cancelled_now(k->parent2));
}

int32_t __pas_token_cancelled(PasToken *k) {
  return cancelled_now(k);
}

/* The tasks that wait observing k, or a token made from it, are
 * registered with k too (see observe). An observer registers under k->lk
 * and then checks the flag, the canceller sets it under k->lk and then
 * wakes the registered: one of the two sees the other. */
static void cancel_token(PasToken *k) {
  if (!k)
    return;
  spin_lock(&k->lk);
  if (!k->cancelled) {
    __atomic_store_n(&k->cancelled, 1, __ATOMIC_SEQ_CST);
    for (PasWaiter *w = k->waiters; w; w = w->next)
      wake(w, WAKE_CANCEL);
  }
  spin_unlock(&k->lk);
}

void __pas_token_cancel(PasToken *k) {
  cancel_token(k);
}

/* A task about to wait: registered with its token and the token's
 * ancestors, so that cancelling any wakes it. nodes: one per token. */
#define OBSERVED 16
typedef struct {
  PasWaiter nodes[OBSERVED];
  PasToken *tokens[OBSERVED];
  int n;
} Observation;

static void observe_token(PasTask *t, PasToken *k, Observation *o) {
  if (!k || o->n == OBSERVED)
    return;
  for (int i = 0; i < o->n; ++i)
    if (o->tokens[i] == k)
      return;
  memset(&o->nodes[o->n], 0, sizeof(PasWaiter));
  o->nodes[o->n].task = t;
  o->tokens[o->n] = k;
  spin_lock(&k->lk);
  add_waiter(&k->waiters, &o->nodes[o->n]);
  spin_unlock(&k->lk);
  o->n++;
  observe_token(t, k->parent, o);
  observe_token(t, k->parent2, o);
}

static void observe(PasTask *t, Observation *o) {
  o->n = 0;
  observe_token(t, t->token, o);
}

static void unobserve(Observation *o) {
  for (int i = 0; i < o->n; ++i) {
    PasToken *k = o->tokens[i];
    spin_lock(&k->lk);
    remove_waiter(&k->waiters, &o->nodes[i]);
    spin_unlock(&k->lk);
  }
}

/* The current task me waits until done(arg) holds, or (cancellable) until
 * its token is cancelled; 1 when it was cancelled first. Its wakers make
 * done(arg) true and then wake() it. me says it waits (state WAITING)
 * before it looks at done: a waker that came first is seen here, one that
 * comes later finds WAITING and queues it — the compare-and-swap decides
 * who resumes it when both happen. */
static void suspend(PasTask *t);

static int park(PasTask *me, int (*done)(void *), void *arg, int cancellable) {
  for (;;) {
    __atomic_store_n(&me->state, TS_WAITING, __ATOMIC_SEQ_CST);
    int ready = done(arg) || (cancellable && cancelled_now(me->token));
    int expected = TS_WAITING;
    if (!ready || !__atomic_compare_exchange_n(&me->state, &expected, TS_RUNNING, 0,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      suspend(me); /* (queued by a waker meanwhile: resumed right away) */
    if (done(arg))
      return 0;
    if (cancellable && cancelled_now(me->token))
      return 1;
  }
}

static void raise_cancelled(void) __attribute__((noreturn));
static void raise_cancelled(void) {
  __pas_raise_message(__pas_eoperationcancelled_vmt, "Operation cancelled");
}

/*===- Switching -------------------------------------------------------------===*/

/* Task (and iterator) stacks: 1 MB unless P3_TASK_STACK_SIZE (bytes, or
 * with a k/K or m/M suffix) or __pas_task_set_stack_size chose another; at
 * least 16 KB, rounded up to whole pages, each with a guard page below.
 * The size is fixed when the first stack is made: the caches hold stacks of
 * one size (and munmap is given that size). A stack is a mapping of its own,
 * so the size sets the address space a parked task reserves, not what it
 * uses: only the pages it touched are resident. */
#define STACK_SIZE_DEFAULT ((size_t)1024 * 1024)
#define STACK_SIZE_MIN ((size_t)16 * 1024)
#define STACK_SIZE_MAX ((size_t)1024 * 1024 * 1024)
static size_t stack_size_v;     /* 0 until chosen (atomic) */
static int stack_size_fixed;    /* a stack was made: no more changes (atomic) */

static size_t stack_size_round(uint64_t n) {
  size_t page = page_size();
  if (n < STACK_SIZE_MIN)
    n = STACK_SIZE_MIN;
  if (n > STACK_SIZE_MAX)
    n = STACK_SIZE_MAX;
  return ((size_t)n + page - 1) / page * page;
}

/* "65536", "64k", "1m" -> bytes; 0 when it is not a size. */
static uint64_t stack_size_parse(const char *s) {
  if (!s || !*s)
    return 0;
  uint64_t n = 0;
  const char *p = s;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (uint64_t)(*p - '0');
    if (n > ((uint64_t)1 << 40))
      return 0;
    p++;
  }
  if (p == s)
    return 0;
  if (*p == 'k' || *p == 'K') {
    n *= 1024;
    p++;
  } else if (*p == 'm' || *p == 'M') {
    n *= 1024 * 1024;
    p++;
  }
  return *p ? 0 : n;
}

/* Under LOCK: the size, chosen now if it was not (the environment, or the
 * default). */
static size_t stack_size_choose_locked(void) {
  size_t v = stack_size_v;
  if (!v) {
    const char *env = getenv("P3_TASK_STACK_SIZE");
    uint64_t n = stack_size_parse(env);
    if (env && !n) {
      static const char warn[] =
          "p3: P3_TASK_STACK_SIZE is not a size (bytes, or with k or m); "
          "using the default 1m\n";
      ssize_t r = write(2, warn, sizeof warn - 1);
      (void)r;
    }
    v = n ? stack_size_round(n) : STACK_SIZE_DEFAULT;
    __atomic_store_n(&stack_size_v, v, __ATOMIC_RELEASE);
  }
  return v;
}

static int stack_guard_choose_locked(void);

/* The size of every task stack; fixed from the first stack on (and the
 * guard pages' choice with it). */
static size_t stack_size(void) {
  size_t v = __atomic_load_n(&stack_size_v, __ATOMIC_ACQUIRE);
  if (v && __atomic_load_n(&stack_size_fixed, __ATOMIC_ACQUIRE))
    return v;
  LOCK();
  v = stack_size_choose_locked();
  stack_guard_choose_locked();
  __atomic_store_n(&stack_size_fixed, 1, __ATOMIC_RELEASE);
  UNLOCK();
  return v;
}

int64_t __pas_task_stack_size(void) {
  if (__atomic_load_n(&stack_size_fixed, __ATOMIC_ACQUIRE))
    return (int64_t)__atomic_load_n(&stack_size_v, __ATOMIC_ACQUIRE);
  LOCK();
  size_t v = stack_size_choose_locked();
  UNLOCK();
  return (int64_t)v;
}

/* Sets the stack size (clamped and rounded as above) if no stack was made
 * yet: 1; 0 once tasks or iterators have started (the size stays). */
int32_t __pas_task_set_stack_size(int64_t bytes) {
  LOCK();
  if (__atomic_load_n(&stack_size_fixed, __ATOMIC_ACQUIRE)) {
    UNLOCK();
    return 0;
  }
  __atomic_store_n(&stack_size_v, stack_size_round(bytes > 0 ? (uint64_t)bytes : 0),
                   __ATOMIC_RELEASE);
  UNLOCK();
  return 1;
}
/* Stacks come from slabs: one mapping holds many stacks, carved in order,
 * and a stack goes back to a free list when its task ends — slabs are never
 * unmapped. With guard pages (the default; P3_TASK_STACK_GUARD=0 or
 * __pas_task_set_stack_guard drop them) each stack has a PROT_NONE page
 * below it, which splits the slab into two kernel mappings (VMAs) per
 * stack: an overflow faults at once, but a process can hold only about
 * vm.max_map_count / 2 parked tasks (Linux: 65530 -> ~32k). Without guards
 * a slab is one mapping however many stacks it holds, and an overflow is
 * only caught, not stopped: below each stack lies an unused gap of
 * STACK_GAP bytes (address space only: untouched, it costs no memory),
 * and a task that switches out with its stack pointer below its stack is
 * stopped with an error. An overflow that returned before its task
 * switched out goes unseen (it landed in the gap, harmlessly, if it was
 * smaller); a bigger one reaches the stack below, whose parked task keeps
 * its wait records near its top, and may crash elsewhere. (A canary at the
 * bottom of each stack would see more, but writing it makes that page
 * resident: a page per parked task, as much as all the rest it uses.) *
 * The check needs the fast context switch (aarch64, x86_64). */
#define STACK_SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define STACK_GAP ((size_t)16 * 1024)
static int stack_guard_v = -1;   /* -1 until chosen (under LOCK) */

/* Each thread keeps the stacks of its ended tasks, without the lock: every
 * `await` of an async call is a task of its own, so a server starts and
 * ends tasks at its request rate. */
#define STACK_TCACHE 256
_Thread_local void *__pas_async_tstacks[STACK_TCACHE];
_Thread_local int __pas_async_ntstacks;

/* Under LOCK: the slab being carved, and the free list (an array: the
 * stacks in it keep no link, so their memory can be released). */
static char *stack_slab_next;
static size_t stack_slab_left;     /* stacks still to carve from it */
static void **stack_free;
static size_t stack_nfree, stack_capfree;
int64_t __pas_async_stack_slabs;   /* slabs mapped (for tests) */

/* Under LOCK: guard pages on or off, chosen now if they were not. */
static int stack_guard_choose_locked(void) {
  if (stack_guard_v < 0) {
    const char *env = getenv("P3_TASK_STACK_GUARD");
    stack_guard_v = !(env && (env[0] == '0' || strcmp(env, "off") == 0 ||
                              strcmp(env, "no") == 0));
  }
  return stack_guard_v;
}

static int stack_guard(void) {
  stack_size();   /* (fixes both: the size and the guards) */
  return stack_guard_v;
}

int32_t __pas_task_stack_guard(void) {
  LOCK();
  int g = stack_guard_choose_locked();
  UNLOCK();
  return g;
}

/* Guard pages on (1) or off (0) — only before the first stack is made: 1
 * when set, 0 when it is too late. */
int32_t __pas_task_set_stack_guard(int32_t on) {
  LOCK();
  if (__atomic_load_n(&stack_size_fixed, __ATOMIC_ACQUIRE)) {
    UNLOCK();
    return 0;
  }
  stack_guard_v = on ? 1 : 0;
  UNLOCK();
  return 1;
}

/* After task t switched out: its stack pointer within its stack. */
static void stack_check(PasTask *t) {
#ifdef PAS_FAST_CTX
  if (!stack_guard_v && t->stack && (char *)t->ctx.sp < (char *)t->stack)
    __pas_runtime_error("a task overflowed its stack (P3_TASK_STACK_SIZE is "
                        "too small for it; without guard pages, "
                        "P3_TASK_STACK_GUARD=0, an overflow is only seen when "
                        "the task switches out)");
#else
  (void)t;
#endif
}

/* len bytes of fresh address space for stacks. */
static char *stack_map(size_t len) {
#ifdef _WIN32
  /* reserved only: each stack is committed when it is carved (new_stack);
     its guard page stays reserved, which faults like PROT_NONE */
  char *m = VirtualAlloc(NULL, len, MEM_RESERVE, PAGE_NOACCESS);
  if (!m)
    __pas_runtime_error("out of memory for task stacks");
  return m;
#else
  char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED)
    __pas_runtime_error("out of memory for task stacks");
#ifdef __APPLE__
  /* macOS malloc reserves a huge span of address space (MAGMA: observed
     0x1000000000..0x7000000000 on macOS 26) whose slabs it brings up by
     changing their protection as needed. An anonymous mmap(NULL) can land
     inside it, and the allocator's next slab then runs into a stack's
     guard page (SIGBUS in the middle of malloc). Task stacks go above it. */
  const uintptr_t lo = 0x1000000000ULL, hi = 0x7000000000ULL;
  if ((uintptr_t)m >= lo && (uintptr_t)m < hi) {
    munmap(m, len);
    uintptr_t hint = 0xFC00000000ULL;
    m = MAP_FAILED;
    for (int t = 0; t < 4096 && m == MAP_FAILED; t++, hint -= len) {
      m = mmap((void *)hint, len, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (m == MAP_FAILED)
        continue;
      if ((uintptr_t)m >= lo && (uintptr_t)m < hi) {
        munmap(m, len);
        m = MAP_FAILED;
      }
    }
    if (m == MAP_FAILED)
      __pas_runtime_error("out of memory for task stacks");
  }
#endif
  return m;
#endif
}

/* Below each stack: its guard page, or the unprotected gap. */
static size_t stack_below(size_t page, int guard) {
  if (guard)
    return page;
  return (STACK_GAP + page - 1) / page * page;
}

#ifdef _WIN32
/* What lies below each stack (its guard page or gap). */
static size_t stack_floor_gap(void) {
  return stack_below(page_size(), stack_guard());
}
#endif

/* Under LOCK: a new slab to carve (stride: a stack and what lies below). */
static void stack_slab_new(size_t size, size_t page, int guard) {
  size_t stride = size + stack_below(page, guard);
  size_t n = STACK_SLAB_BYTES / stride;
  if (n < 1)
    n = 1;
  if (n > 4096)
    n = 4096;
  char *m = stack_map(n * stride);
#ifndef _WIN32
  if (guard)
    for (size_t i = 0; i < n; i++)
      if (mprotect(m + i * stride, page, PROT_NONE) != 0)
        __pas_runtime_error("cannot place a task stack's guard page (too many "
                            "mappings: vm.max_map_count?); P3_TASK_STACK_GUARD=0 "
                            "keeps a slab of stacks in one mapping");
#else
  (void)guard;
#endif
  stack_slab_next = m;
  stack_slab_left = n;
  __pas_async_stack_slabs++;
}

static void *new_stack(void) {
  if (__pas_async_ntstacks)
    return __pas_async_tstacks[--__pas_async_ntstacks];
  size_t size = stack_size();
  int guard = stack_guard();
  size_t page = page_size();
  void *s;
  LOCK();
  if (stack_nfree) {
    s = stack_free[--stack_nfree];
  } else {
    if (!stack_slab_left)
      stack_slab_new(size, page, guard);
    s = stack_slab_next + stack_below(page, guard);
    stack_slab_next += size + stack_below(page, guard);
    stack_slab_left--;
#ifdef _WIN32
    /* The stack and what lies below it, committed. Without guard pages the
       gap is writable, as on the other systems (an overflow is caught when
       its task switches out). With them, the page below is a PAGE_GUARD
       page inside the stack's bounds (see ctx_make), as on a Windows
       thread's own stack: an overflow is STATUS_STACK_OVERFLOW, which ends
       the program (a plain inaccessible page would fault again in the
       exception's dispatch, on the same stack, endlessly). */
    char *lo = (char *)s - stack_below(page, guard);
    DWORD old;
    if (!VirtualAlloc(lo, (size_t)((char *)s + size - lo), MEM_COMMIT, PAGE_READWRITE) ||
        (guard && !VirtualProtect(lo, page, PAGE_READWRITE | PAGE_GUARD, &old))) {
      UNLOCK();
      __pas_runtime_error("out of memory for task stacks");
    }
#endif
  }
  UNLOCK();
  return s;
}

static void free_stack(void *s) {
  if (__pas_async_ntstacks < STACK_TCACHE) {
    __pas_async_tstacks[__pas_async_ntstacks++] = s;
    return;
  }
  /* past the thread's cache: its memory goes back to the system (the
   * address space stays, for the next stack) */
#if defined(_WIN32)
  VirtualAlloc(s, stack_size(), MEM_RESET, PAGE_READWRITE);
#elif defined(__APPLE__)
  madvise(s, stack_size(), MADV_FREE);
#else
  madvise(s, stack_size(), MADV_DONTNEED);
#endif
  LOCK();
  if (stack_nfree == stack_capfree) {
    size_t n = stack_capfree ? 2 * stack_capfree : 1024;
    void **a = realloc(stack_free, n * sizeof *a);
    if (!a)
      __pas_runtime_error("out of memory for task stacks");
    stack_free = a;
    stack_capfree = n;
  }
  stack_free[stack_nfree++] = s;
  UNLOCK();
}

void __pas_task_release(PasTask *t);

/* From here into t until it waits or ends; from is where it comes back. */
static void switch_into(PasTask *t, PasCtx *from) {
  PasContext *c = context();
  PasTask *prev = c->current;
  PasExcFrame *saved = __pas_exc_top;
  c->current = t;
  __pas_exc_top = t->exc_top;
  t->resumer = from;
#ifdef PAS_ASAN
  void *fake = NULL;
  __sanitizer_start_switch_fiber(&fake, t->stack, t->stack_size);
#endif
#ifdef PAS_TSAN
  t->tsan_resumer = __tsan_get_current_fiber();
  __tsan_switch_to_fiber(t->tsan_fiber, 0);
#endif
#ifdef PAS_FAST_CTX
  __pas_ctx_switch(&from->sp, t->ctx.sp);
#else
  swapcontext(from, &t->ctx);
#endif
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(fake, NULL, NULL);
#endif
  c->current = prev;
  __pas_exc_top = saved;
  stack_check(t);
  int st = task_state(t);
  if (st == TS_DONE || st == TS_FAILED) { /* it ended */
#ifdef PAS_TSAN
    __tsan_destroy_fiber(t->tsan_fiber);
#endif
    free_stack(t->stack);
    t->stack = NULL;
    __pas_task_release(t); /* its own reference while it ran */
  }
}

/* In t: back to whoever resumed it; returns when it is resumed. */
static void suspend(PasTask *t) {
  t->exc_top = __pas_exc_top;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(&t->fake_stack, t->resumer_bottom, t->resumer_size);
#endif
#ifdef PAS_TSAN
  __tsan_switch_to_fiber(t->tsan_resumer, 0);
#endif
#ifdef PAS_FAST_CTX
  __pas_ctx_switch(&t->ctx.sp, t->resumer->sp);
#else
  swapcontext(&t->ctx, t->resumer);
#endif
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(t->fake_stack, &t->resumer_bottom, &t->resumer_size);
#endif
}

static void finish(PasTask *t, void *exception);

static void task_main(void) {
  PasContext *c = context();
  PasTask *t = c->starting;
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(NULL, &t->resumer_bottom, &t->resumer_size);
#endif
  PasExcFrame f;
  memset(&f, 0, sizeof f);
  __pas_exc_push(&f);
  if (_setjmp(*(jmp_buf *)(void *)f.buf) == 0) {
    t->entry(t);
    __pas_exc_pop(&f);
    if (t->args_fini)
      t->args_fini(t->data + t->result_size);
    finish(t, NULL);
  } else {
    if (t->args_fini)
      t->args_fini(t->data + t->result_size);
    t->exc_where = f.where;
    finish(t, f.object);
  }
  t->exc_top = NULL;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(NULL, t->resumer_bottom, t->resumer_size);
#endif
#ifdef PAS_TSAN
  __tsan_switch_to_fiber(t->tsan_resumer, 0);
#endif
#ifdef PAS_FAST_CTX
  {
    void *dead; /* this context is never resumed */
    __pas_ctx_switch(&dead, t->resumer->sp); /* (its stack is freed there) */
  }
#else
  setcontext(t->resumer); /* (its stack is freed there) */
#endif
  __pas_runtime_error("a task resumed after it ended");
}

/* Starts t (a new task, here: its home's thread). */
static void start(PasTask *t, PasCtx *from) {
  t->stack = new_stack();
  t->stack_size = stack_size();
#ifdef PAS_TSAN
  t->tsan_fiber = __tsan_create_fiber(0);
#endif
#ifdef PAS_FAST_CTX
  ctx_make(&t->ctx, t->stack, t->stack_size, task_main);
#else
  getcontext(&t->ctx);
  t->ctx.uc_stack.ss_sp = t->stack;
  t->ctx.uc_stack.ss_size = t->stack_size;
  t->ctx.uc_link = NULL;
  makecontext(&t->ctx, task_main, 0);
#endif
  __atomic_store_n(&t->state, TS_RUNNING, __ATOMIC_SEQ_CST);
  context()->starting = t;
  switch_into(t, from);
}

/* Runs t (ready or new) on this thread until it waits or ends. */
static void resume(PasTask *t) {
  PasCtx here;
  if (task_state(t) == TS_NEW) {
    start(t, &here);
    return;
  }
  __atomic_store_n(&t->state, TS_RUNNING, __ATOMIC_SEQ_CST);
  switch_into(t, &here);
}

/* On a thread outside any task: runs this thread's ready tasks until
 * done(arg), waiting when there are none (until deadline_ms, a
 * CLOCK_MONOTONIC time in ms, if not 0). done is read without a lock (its
 * wakers set what it reads, then wake()); it is checked again under qmu
 * before sleeping, and a wake takes qmu, so none is lost. */
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ctx_poll_once(PasContext *c, int64_t deadline_ms);
static int woken(void *arg);

static void loop_until(PasContext *c, int (*done)(void *), void *arg,
                       int64_t deadline_ms) {
  while (!done(arg)) {
    pthread_mutex_lock(&c->qmu);
    PasTask *t = pop(c);
    if (t) {
      pthread_mutex_unlock(&c->qmu);
      resume(t);
      continue;
    }
    if (done(arg)) {
      pthread_mutex_unlock(&c->qmu);
      return;
    }
    if (c->pfd >= 0) {
      /* this thread waits on sockets: block in its poller */
      if (deadline_ms && deadline_ms - now_ms() <= 0) {
        pthread_mutex_unlock(&c->qmu);
        return;
      }
      c->sleeping = 1;
      pthread_mutex_unlock(&c->qmu);
      ctx_poll_once(c, deadline_ms);
      continue;
    }
    if (deadline_ms) {
      int64_t left = deadline_ms - now_ms();
      if (left <= 0) {
        pthread_mutex_unlock(&c->qmu);
        return;
      }
      /* (pthread_cond_timedwait takes CLOCK_REALTIME) */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += left / 1000;
      ts.tv_nsec += (left % 1000) * 1000000;
      if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&c->cv, &c->qmu, &ts);
    } else {
      pthread_cond_wait(&c->cv, &c->qmu);
    }
    pthread_mutex_unlock(&c->qmu);
  }
}

static int never(void *arg) {
  (void)arg;
  return 0;
}

static void *pool_main(void *arg) {
  PasContext *c = arg;
  __pas_async_ctx = c;
  loop_until(c, never, NULL, 0);
  return NULL;
}

/* Before a thread is created: reference counts become atomic from now on
   (pascalrt.h). Set while this is the only thread, so a plain store does;
   pthread_create publishes it to the new thread. */
void __pas_start_threads(void) {
  __pas_threads_started = 1;
}

/* The pool, started when first needed; the next thread, round robin. */
static PasContext *pool_context(void) {
  if (!__atomic_load_n(&__pas_async_pool_size, __ATOMIC_ACQUIRE)) {
    LOCK();
    if (__pas_async_pool) {
      UNLOCK();
      goto started;
    }
    long n = cpu_count();
    const char *env = getenv("P3_POOL_THREADS");
    if (env && atoi(env) > 0)
      n = atoi(env);
    if (n < 2)
      n = 2;
    if (n > 64)
      n = 64;
    __pas_async_pool = calloc((size_t)n, sizeof(PasContext *));
    for (long i = 0; i < n; ++i) {
      __pas_async_pool[i] = new_context(1);
      pthread_t th;
      pthread_attr_t a;
      pthread_attr_init(&a);
      pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
      __pas_start_threads();
      if (pthread_create(&th, &a, pool_main, __pas_async_pool[i]) != 0)
        __pas_runtime_error("cannot start the thread pool");
      pthread_attr_destroy(&a);
    }
    __atomic_store_n(&__pas_async_pool_size, (int)n, __ATOMIC_RELEASE);
    UNLOCK();
  }
started:;
  unsigned i = (unsigned)__atomic_fetch_add(&__pas_async_pool_next, 1, __ATOMIC_RELAXED);
  return __pas_async_pool[i % (unsigned)__pas_async_pool_size];
}

/*===- Tasks -----------------------------------------------------------------===*/

static int64_t round16(int64_t n) { return (n + 15) & ~(int64_t)15; }

PasTask *__pas_task_new(const PasDynInfo *info, int64_t args_size,
                        void (*entry)(PasTask *), void (*args_fini)(void *)) {
  int64_t rs = info ? round16(info->elem_size) : 0;
  PasTask *t = calloc(1, sizeof(PasTask) + (size_t)rs + (size_t)args_size);
  if (!t)
    __pas_runtime_error("out of memory for a task");
  t->refs = 1;
  t->info = info;
  t->entry = entry;
  t->args_fini = args_fini;
  t->result_size = rs;
  return t;
}

void *__pas_task_result(PasTask *t) { return t->data; }
void *__pas_task_args(PasTask *t) { return t->data + t->result_size; }

void __pas_task_addref(PasTask *t) {
  if (t)
    __atomic_fetch_add(&t->refs, 1, __ATOMIC_RELAXED);
}

void __pas_task_release(PasTask *t) {
  if (!t || __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  if (t->state == TS_DONE && t->info && t->info->fini)
    t->info->fini(t->data);
  if (t->exception)
    __pas_free_object(t->exception);
  __pas_token_release(t->token);
  free(t);
}

void __pas_task_assign(PasTask **dst, PasTask *src) {
  __pas_task_addref(src);
  PasTask *old = *dst;
  *dst = src;
  __pas_task_release(old);
}

void __pas_task_clear(PasTask **p) {
  PasTask *old = *p;
  *p = NULL;
  __pas_task_release(old);
}

/* The token the current code observes: its task's, or none. */
static PasToken *current_token(void) {
  PasTask *t = context()->current;
  return t ? t->token : NULL;
}

static PasScope **current_scopes(void) {
  PasContext *c = context();
  return c->current ? &c->current->scopes : &c->scopes;
}

/* An async routine called: t runs here at once, until it waits or ends,
 * observing the caller's token. */
void __pas_task_start(PasTask *t) {
  PasContext *c = context();
  t->home = c;
  t->token = current_token();
  __pas_token_addref(t->token);
  __pas_task_addref(t); /* its own, while it runs */
  PasCtx here;
  start(t, &here);
}

/* Queues new task t on a pool thread. */
static void run_on_pool(PasTask *t) {
  __pas_task_addref(t); /* its own, while it runs */
  t->home = pool_context();
  __atomic_store_n(&t->state, TS_NEW, __ATOMIC_RELAXED);
  post(t);
}

/* spawn: t runs on the pool, a child of the innermost scope open here
 * (observing its token). */
void __pas_task_spawn(PasTask *t) {
  PasScope *s = *current_scopes();
  if (s) {
    /* (a scope's children list belongs to its owner, which runs here) */
    if (s->count == s->cap) {
      s->cap = s->cap ? 2 * s->cap : 8;
      s->children = realloc(s->children, (size_t)s->cap * sizeof(PasTask *));
      if (!s->children)
        __pas_runtime_error("out of memory in a scope");
    }
    s->children[s->count++] = t;
    __pas_task_addref(t);
    t->scope = s;
    t->token = s->token;
    __pas_token_addref(t->token);
  }
  run_on_pool(t);
}

/* Pool.Run: t runs on the pool, observing the caller's token. */
void __pas_task_run_pool(PasTask *t) {
  t->token = current_token();
  __pas_token_addref(t->token);
  run_on_pool(t);
}

static void call_error_handler(PasTask *t, void *exception);

/* In t, its code done: the result is in place, or the exception. */
static void finish(PasTask *t, void *exception) {
  /* first, while t is not over yet: a scope's owner frees the scope once
   * all its children ended */
  if (exception && t->scope)
    cancel_token(t->scope->token); /* the others stop */
  spin_lock(&t->lk);
  t->exception = exception;
  int handle = exception && t->detached && t->on_error.fn;
  if (handle)
    t->exception = NULL;
  __atomic_store_n(&t->state, exception ? TS_FAILED : TS_DONE, __ATOMIC_SEQ_CST);
  for (PasWaiter *w = t->waiters; w; w = w->next)
    wake(w, WAKE_NORMAL);
  t->waiters = NULL;
  spin_unlock(&t->lk);
  if (handle)
    call_error_handler(t, exception);
}

static int task_ended(void *arg) {
  int st = task_state(arg);
  return st == TS_DONE || st == TS_FAILED;
}

/* Until t ends. In a task: it waits (cancelled if its token is, unless
 * ignore_cancel); outside: this thread runs its tasks. */
static void wait_for(PasTask *t, int ignore_cancel) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (task_ended(t))
    return;
  if (me == t)
    __pas_runtime_error("a task waits for itself");
  if (me && !ignore_cancel && cancelled_now(me->token))
    raise_cancelled();
  PasWaiter w = {NULL, me, me ? NULL : c, 0};
  spin_lock(&t->lk);
  if (task_ended(t)) {
    spin_unlock(&t->lk);
    return;
  }
  add_waiter(&t->waiters, &w);
  spin_unlock(&t->lk);
  int cancelled = 0;
  if (!me) {
    loop_until(c, task_ended, t, 0);
  } else {
    Observation o;
    o.n = 0;
    if (!ignore_cancel)
      observe(me, &o);
    cancelled = park(me, task_ended, t, !ignore_cancel);
    unobserve(&o);
  }
  spin_lock(&t->lk);
  remove_waiter(&t->waiters, &w);
  spin_unlock(&t->lk);
  if (cancelled)
    raise_cancelled();
}

/* Raises t's exception (once; then a new one that says so). */
static void raise_failure(PasTask *t) __attribute__((noreturn));
static void raise_failure(PasTask *t) {
  spin_lock(&t->lk);
  void *e = t->exception;
  t->exception = NULL;
  spin_unlock(&t->lk);
  if (e)
    __pas_raise(e, t->exc_where ? t->exc_where : "");
  __pas_raise_message(__pas_exception_vmt,
                      "the task failed; its exception was raised by an earlier await");
}

/* await t / Wait(t): after it, the result is at __pas_task_result(t). */
void __pas_task_await(PasTask *t) {
  if (!t)
    __pas_runtime_error("await of a nil task");
  wait_for(t, 0);
  if (task_state(t) == TS_FAILED)
    raise_failure(t);
}

int32_t __pas_task_done(PasTask *t) {
  return t && task_ended(t);
}

/* detached: t's failure goes to handler (a procedure(E: TObject) value),
 * or nowhere. */
void __pas_task_detach(PasTask *t, void *handler_fn, void *handler_link) {
  spin_lock(&t->lk);
  t->detached = 1;
  t->on_error.fn = handler_fn;
  t->on_error.frame = handler_link;
  void *e = NULL;
  if (task_state(t) == TS_FAILED && handler_fn) {
    e = t->exception;
    t->exception = NULL;
  }
  spin_unlock(&t->lk);
  if (e)
    call_error_handler(t, e);
}

static void call_error_handler(PasTask *t, void *exception) {
  void (*fn)(void *, void *) = (void (*)(void *, void *))t->on_error.fn;
  PasExcFrame f;
  memset(&f, 0, sizeof f);
  __pas_exc_push(&f);
  if (_setjmp(*(jmp_buf *)(void *)f.buf) == 0) {
    fn(t->on_error.frame, exception);
    __pas_exc_pop(&f);
  } else if (f.object) { /* (an exception from the handler: dropped) */
    __pas_free_object(f.object);
  }
  __pas_free_object(exception);
}

/*===- Scopes ----------------------------------------------------------------===*/

/* scope [token] ... end: its children run observing a token of its own,
 * cancelled when one of them fails, when the token given is, or when the
 * block itself raises. */
void *__pas_scope_enter(PasToken *given) {
  PasScope *s = calloc(1, sizeof *s);
  if (!s)
    __pas_runtime_error("out of memory for a scope");
  PasContext *c = context();
  /* cancelled with the token given and with what the code observes */
  PasToken *outer = c->current ? c->current->token : NULL;
  s->token = token_new2(given, outer != given ? outer : NULL);
  PasScope **list = current_scopes();
  s->outer = *list;
  *list = s;
  if (c->current) { /* the block observes the scope's token */
    s->saved_token = c->current->token;
    c->current->token = s->token;
    __pas_token_addref(s->token);
  }
  return s;
}

/* The end of scope s (raised: the block is leaving with an exception, which
 * its caller raises again): waits for every child; raises the first
 * child's exception if none. */
void __pas_scope_leave(void *scope, int32_t raised) {
  PasScope *s = scope;
  PasContext *c = context();
  if (c->current) { /* the owner observes what it did before */
    PasToken *k = c->current->token;
    c->current->token = s->saved_token;
    __pas_token_release(k);
  }
  PasScope **list = current_scopes();
  *list = s->outer;
  if (raised)
    cancel_token(s->token);
  for (int64_t i = 0; i < s->count; ++i)
    wait_for(s->children[i], 1);
  void *first = NULL;
  for (int64_t i = 0; i < s->count; ++i) {
    PasTask *t = s->children[i];
    spin_lock(&t->lk);
    void *e = task_state(t) == TS_FAILED ? t->exception : NULL;
    t->exception = NULL;
    spin_unlock(&t->lk);
    if (e) {
      int cancel = __pas_class_is(e, __pas_eoperationcancelled_vmt);
      /* a child's own failure wins over the cancellations it caused */
      if (!first || (__pas_class_is(first, __pas_eoperationcancelled_vmt) && !cancel)) {
        if (first)
          __pas_free_object(first);
        first = e;
      } else {
        __pas_free_object(e);
      }
    }
    t->scope = NULL;
  }
  for (int64_t i = 0; i < s->count; ++i)
    __pas_task_release(s->children[i]);
  free(s->children);
  __pas_token_release(s->token);
  free(s);
  if (first && !raised)
    __pas_raise(first, "");
  if (first)
    __pas_free_object(first);
}

/*===- Timers ----------------------------------------------------------------===*/

typedef struct PasTimer {
  struct PasTimer *next;
  int64_t at;      /* CLOCK_MONOTONIC ms */
  PasTask *task;   /* to wake (Sleep), or */
  PasToken *token; /* to cancel (WithTimeout; counted) */
  int fired;
} PasTimer;

PasTimer *__pas_async_timers;
pthread_cond_t __pas_async_timer_cv = PTHREAD_COND_INITIALIZER;
int __pas_async_timer_started;

static void *timer_main(void *arg) {
  (void)arg;
  LOCK();
  for (;;) {
    int64_t now = now_ms();
    while (__pas_async_timers && __pas_async_timers->at <= now) {
      PasTimer *m = __pas_async_timers;
      __pas_async_timers = m->next;
      __atomic_store_n(&m->fired, 1, __ATOMIC_SEQ_CST);
      if (m->task) {
        /* (m is the sleeper's: it leaves only after taking LOCK) */
        PasWaiter w = {NULL, m->task, NULL, 0};
        wake(&w, WAKE_NORMAL);
      } else {
        cancel_token(m->token);
        PasToken *k = m->token;
        free(m);
        UNLOCK();
        __pas_token_release(k);
        LOCK();
      }
    }
    if (!__pas_async_timers) {
      pthread_cond_wait(&__pas_async_timer_cv, &__pas_async_mutex);
    } else {
      int64_t left = __pas_async_timers->at - now_ms();
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += left / 1000;
      ts.tv_nsec += (left % 1000) * 1000000;
      if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&__pas_async_timer_cv, &__pas_async_mutex, &ts);
    }
  }
  return NULL;
}

/* Under the lock. */
static void add_timer(PasTimer *m) {
  if (!__pas_async_timer_started) {
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    __pas_start_threads();
    if (pthread_create(&th, &a, timer_main, NULL) != 0)
      __pas_runtime_error("cannot start the timer thread");
    pthread_attr_destroy(&a);
    __pas_async_timer_started = 1;
  }
  PasTimer **p = &__pas_async_timers;
  while (*p && (*p)->at <= m->at)
    p = &(*p)->next;
  m->next = *p;
  *p = m;
  pthread_cond_signal(&__pas_async_timer_cv);
}

static void remove_timer(PasTimer *m) {
  for (PasTimer **p = &__pas_async_timers; *p; p = &(*p)->next)
    if (*p == m) {
      *p = m->next;
      return;
    }
}

/* A token cancelled after ms milliseconds, and with parent. */
PasToken *__pas_token_with_timeout(PasToken *parent, int32_t ms) {
  PasToken *k = __pas_token_new(parent);
  PasTimer *m = calloc(1, sizeof *m);
  if (!m)
    __pas_runtime_error("out of memory for a timer");
  m->at = now_ms() + (ms > 0 ? ms : 0);
  m->token = k;
  __pas_token_addref(k);
  LOCK();
  add_timer(m);
  UNLOCK();
  return k;
}

static int never_done(void *arg) {
  (void)arg;
  return 0;
}

static int timer_fired(void *arg) {
  return __atomic_load_n(&((PasTimer *)arg)->fired, __ATOMIC_SEQ_CST);
}

/* Sleep(ms): a task waits (cancellable); a thread runs its tasks meanwhile. */
void __pas_task_sleep(int32_t ms) {
  PasContext *c = context();
  PasTask *me = c->current;
  int64_t at = now_ms() + (ms > 0 ? ms : 0);
  if (!me) {
    loop_until(c, never_done, NULL, at);
    return;
  }
  if (cancelled_now(me->token))
    raise_cancelled();
  PasTimer m;
  memset(&m, 0, sizeof m);
  m.at = at;
  m.task = me;
  LOCK();
  add_timer(&m);
  UNLOCK();
  Observation o;
  observe(me, &o);
  int cancelled = park(me, timer_fired, &m, 1);
  unobserve(&o);
  LOCK();
  if (!m.fired)
    remove_timer(&m);
  UNLOCK();
  if (cancelled)
    raise_cancelled();
}

/* *dst := a new token, with parent (and cancelled after ms if ms >= 0);
 * the reference moves into *dst (for unit threading). */
void __pas_token_make(PasToken *parent, int32_t ms, PasToken **dst) {
  PasToken *k = ms >= 0 ? __pas_token_with_timeout(parent, ms) : __pas_token_new(parent);
  PasToken *old = *dst;
  *dst = k;
  __pas_token_release(old);
}

/* Cancellation checked by hand (TCancelToken.ThrowIfCancelled). */
void __pas_token_check(PasToken *k) {
  if (__pas_token_cancelled(k))
    raise_cancelled();
}

/*===- Waiting on descriptors (unit netasync) ------------------------------===*/

/* A task parks until a descriptor is readable or writable (__pas_task_wait_fd)
 * or until an async read/send completes (__pas_task_recv/__pas_task_send).
 * Backends, chosen at runtime: kqueue (macOS) and epoll (Linux), one poller
 * per thread (see "The per-thread pollers" below), and io_uring (Linux,
 * opt-in with P3_NET_URING=1, raw syscalls — no liburing dependency): one
 * ring whose small poller thread hands completions to the tasks. On the
 * io_uring backend a wait submits io_uring ops (RECV/SEND/POLL_ADD) and is
 * woken by its completion; on the poll backends the syscall happens in the
 * task (recv tries first and parks only on EAGAIN).
 *
 * A wait record belongs to its wait: the park helper frees it on return when
 * no completion can follow (poll backends; an io_uring completion reaped
 * before the task resumes). A cancelled io_uring wait abandons its record on
 * purpose — a completion may follow the task's rise, or never come — and
 * leaks it (bounded, ~72 bytes per cancellation); the record is marked freed
 * at the cancellation, and every completion reader skips freed records
 * without touching them again.
 *
 * Several records may wait on one descriptor at once (tasks of one or
 * several threads: a shared listener's acceptors, a database fd watched by
 * a driver task here and by the pool there). The poll backends arm each
 * record on its own registration (the fd itself in each thread's poller, a
 * dup for a second waiter in the same poller), so one record's retire
 * cannot take the kernel interest away from its still-parked peers; the
 * readiness of the descriptor wakes each of the records parked on it (a
 * firing record wakes its peers: fd_parked), and the extra ones simply
 * find the transient EAGAIN their next syscall would produce. On io_uring
 * every wait is already its own op keyed by user_data, so nothing is
 * shared.
 *
 * The wait reports what fired, as a bitmask it returns: 1 = readable,
 * 2 = writable, 4 = the descriptor is in an error state (POLLERR/POLLHUP
 * on the poll backends, a failed poll completion on io_uring). Everything
 * here is covered by the same lock as the rest of the file. */

/* The per-thread epoll pollers serve Linux and, through wepoll, Windows;
 * macOS has kqueue. On Windows descriptors are SOCKETs (handles that fit
 * an int32, as unit netasync passes them), errors come from
 * WSAGetLastError, and a socket is duplicated with WSADuplicateSocket. */
#if defined(__linux__) || defined(_WIN32)
#define PAS_EPOLL 1
#endif
#ifdef _WIN32
#define EPH(c) ((c)->ep)
#define SOCK(fd) ((SOCKET)(intptr_t)(fd))
#define MSG_NOSIGNAL 0
#define NET_EAGAIN WSAEWOULDBLOCK
#define NET_EINTR WSAEINTR
static int net_err(void) { return WSAGetLastError(); }
static void sock_close(int fd) { closesocket(SOCK(fd)); }
static int sock_dup(int fd) {
  WSAPROTOCOL_INFOW info;
  if (WSADuplicateSocketW(SOCK(fd), GetCurrentProcessId(), &info) != 0)
    return -1;
  SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                        FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
  return s == INVALID_SOCKET ? -1 : (int)(intptr_t)s;
}
#define pollfd_t WSAPOLLFD
#define sock_poll(p, n, ms) WSAPoll((p), (n), (ms))
#else
#define EPH(c) ((c)->pfd)
#define SOCK(fd) (fd)
#define NET_EAGAIN EAGAIN
#define NET_EINTR EINTR
static int net_err(void) { return errno; }
static void sock_close(int fd) { close(fd); }
static int sock_dup(int fd) { return dup(fd); }
#define pollfd_t struct pollfd
#define sock_poll(p, n, ms) poll((p), (n), (ms))
#endif

enum {
  FD_BACKEND_KQUEUE = 0,
  FD_BACKEND_EPOLL = 1,
  FD_BACKEND_URING = 2,
  FD_OP_RECV = 1,
  FD_OP_SEND = 2,
  FD_OP_POLLREAD = 3,
  FD_OP_POLLWRITE = 4
};

/* The kernel's __kernel_timespec for IORING_OP_TIMEOUT (16 bytes); the
 * wait record embeds one, so it is declared for all backends. */
struct pas_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

typedef struct PasFdWait {
  int fd;
  int arm_fd;        /* what the poll backends registered (a dup of fd); -1 on io_uring and before arming */
  int write;         /* 1: waiting to write */
  int op;            /* FD_OP_* (io_uring) */
  int armed;         /* kernel interest in place (poll backends) */
  int state;         /* io_uring: 0 waiting, 1 cancelled, 2 completed;
                        poll backends: 1 retired */
  int freed;         /* done for good: completions skip it, no free */
  int pending;       /* io_uring: completions still in flight (2 for a
                        record armed with a linked timeout, 1 otherwise) */
  int seen;          /* io_uring: its first completion is processed */
  int left;          /* io_uring: the task resumed and consumed the result;
                        the record is freed by the reaper that takes
                        pending to 0 (the task may be gone by then) */
  int has_deadline;  /* 1: armed with a linked timeout (io_uring) or on
                        the poller's deadline list (poll backends) */
  int deadline_ms;   /* io_uring: the linked timeout's value */
  int64_t deadline_us; /* poll backends: when it fires (monotonic us) */
  struct pas_timespec ts; /* io_uring: the linked timeout's timespec */
  int64_t res;       /* io_uring: completion result (POLL_ADD: revents,
                        or negative on failure); poll backends: the
                        bits the poller mapped at the wake (1/2/4, or
                        8 = the deadline fired first) */
  void *buf;         /* io_uring: recv/send buffer */
  int32_t len;
  PasContext *ctx;   /* poll backends: whose poller it is armed in */
  int direct;        /* poll backends: armed on fd itself (not a dup) */
  int fired;         /* poll backends: its one-shot event came */
  int hidx;          /* poll backends: its slot in ctx->dheap */
  int parked;        /* poll backends: on fd_parked[fd] */
  struct PasFdWait *fprev, *fnext; /* fd_parked[fd]: its peers on fd */
  PasWaiter w;
  struct PasFdWait *next;  /* io_uring: fd_waits */
} PasFdWait;

#ifdef __linux__
/* Raw io_uring: constants and ring/SQE/CQE structures defined here, so the
 * runtime needs neither liburing nor linux/io_uring.h. The opcodes are
 * io_uring.h's enum numbers (SEND 26, RECV 27 — the older 9/10 are
 * SENDMSG/RECVMSG, which take a struct msghdr, not a plain buffer). POLL_ADD
 * reads its events from the SQE's poll_events/msg_flags union, not from len. */
#ifndef IORING_OP_POLL_ADD
#define IORING_OP_POLL_ADD 6
#define IORING_OP_POLL_REMOVE 7
#define IORING_OP_TIMEOUT 11
#define IORING_OP_SEND 26
#define IORING_OP_RECV 27
#endif
#ifndef IOSQE_IO_LINK
#define IOSQE_IO_LINK (1U << 3)
#endif
#ifndef ETIME
#define ETIME 62
#endif
#ifndef ECANCELLED
#define ECANCELLED 125
#endif
#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS 1
#endif
#ifndef IORING_OFF_SQ_RING
#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES 0x10000000ULL
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#endif
#define PAS_URING_ENTRIES 16384

struct pas_uring_sqe {
  uint8_t opcode;
  uint8_t flags;
  uint16_t ioprio;
  int32_t fd;
  uint64_t off;
  uint64_t addr;
  uint32_t len;
  uint32_t msg_flags;
  uint64_t user_data;
  uint16_t buf_index;
  uint16_t personality;
  int32_t splice_fd_in;
  uint64_t pad2[2];
};

struct pas_uring_cqe {
  uint64_t user_data;
  int32_t res;
  uint32_t flags;
};

struct pas_uring_sq_off {
  uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array;
  uint32_t resv[3];
};

struct pas_uring_cq_off {
  uint32_t head, tail, ring_mask, ring_entries, overflow, cqes;
  uint32_t resv[3];   /* (the kernel's struct is 120 bytes total) */
};

struct pas_uring_params {
  uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle,
           features, wq_fd;
  uint32_t resv[3];
  struct pas_uring_sq_off sq_off;
  struct pas_uring_cq_off cq_off;
};
#endif

static void fd_poller_start(void);
static void fd_retire(PasFdWait *rec);
int32_t __pas_task_wait_fd2(int32_t fd, int32_t want_write, int32_t deadline_ms);
static int64_t fd_waits_abandoned; /* records a cancelled io_uring wait left behind */
static int fd_backend = FD_BACKEND_KQUEUE;
static int fd_poll_started;
#ifdef __linux__
static PasFdWait *fd_waits;       /* io_uring records, under the lock */
static int fd_poll_fd = -1;       /* the io_uring ring */
static pthread_t fd_poller_thread;
#endif

#ifdef __linux__
static void *ur_sq_ring, *ur_cq_ring;
static unsigned *ur_sq_head, *ur_sq_tail, *ur_sq_mask, *ur_sq_array;
static unsigned *ur_cq_head, *ur_cq_tail, *ur_cq_mask;
static struct pas_uring_cqe *ur_cqes;
static struct pas_uring_sqe *ur_sqes;
#endif

#ifdef __linux__
static void fd_unlink(PasFdWait *rec) {
  PasFdWait **p = &fd_waits;
  while (*p && *p != rec)
    p = &(*p)->next;
  if (*p)
    *p = rec->next;
}
#endif

/* The bits __pas_task_wait_fd returns: 1 = readable, 2 = writable,
 * 4 = failed. */

#ifdef PAS_EPOLL
static int64_t fd_bits_epoll(uint32_t events) {
  int64_t bits = 0;
  if (events & EPOLLIN)
    bits |= 1;
  if (events & EPOLLOUT)
    bits |= 2;
  if (events & (EPOLLERR | EPOLLHUP))
    bits |= 4;
  return bits;
}
#else
static int64_t fd_bits_kqueue(const struct kevent *ev) {
  int64_t bits = (ev->filter == EVFILT_WRITE) ? 2 : 1;
  if ((ev->flags & EV_EOF) && ev->filter == EVFILT_WRITE)
    bits |= 4;   /* the peer stopped receiving: sends end in EPIPE */
  /* an EV_EOF on EVFILT_READ is a normal read side at EOF */
  return bits;
}
#endif

static int64_t fd_now_us(void) {
  struct timespec ts;
#ifdef __linux__
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000;
}

/*===- The per-thread pollers (kqueue / epoll) -------------------------------===
 *
 * Every thread that waits on a descriptor gets its own poller (ctx_poller),
 * and a wait is armed in the poller of the task's home thread: the thread
 * that runs the task is the one that blocks on its socket, so readiness
 * turns into a queued task without another thread or a lock hand-off, and
 * one kevent/epoll_wait returns the events of many tasks at once.
 *
 * Registrations are one-shot (EV_ONESHOT; EPOLLONESHOT re-armed by one
 * EPOLL_CTL_MOD): a wait costs one syscall to arm and none to drop once it
 * fired. The first waiter of a descriptor (per direction on kqueue, per
 * descriptor on epoll, which keeps one registration per fd) arms the fd
 * itself; a second concurrent waiter in the same poller arms a dup of it,
 * as before, so each record stays addressable on its own. Pollers of
 * different threads register the same descriptor independently (a
 * listener shared by acceptors on several threads needs no dup at all).
 *
 * Records are only ever touched by their home thread's poller and their
 * own task, which runs on that same thread: an event batch is processed
 * completely before any task runs, so a record is never freed while an
 * event that names it is still pending. */

enum { FDT_READ = 1, FDT_WRITE = 2, FDT_EPOLL_REG = 4 };

/* Every armed poll-backend wait, by descriptor, whatever its thread. Each
 * poller tests readiness on its own, and a level-triggered filter is
 * re-checked when it is delivered: when the task woken on one thread
 * consumes the readiness first (an accept), the same descriptor's waiter
 * in another thread's poller would never hear of it. So a firing wait
 * wakes its parked peers on the same descriptor and direction too — every
 * waiter parked at the moment of the readiness gets its wake, as the
 * documented semantics say (the extra ones see a transient EAGAIN). */
#define FD_CHUNK 4096
#define FD_CHUNKS 65536
#define FD_STRIPES 64
static PasFdWait **fd_parked[FD_CHUNKS]; /* chunks of FD_CHUNK heads, made on demand */
static int fd_stripe[FD_STRIPES];        /* spin locks: fd's list is under fd_stripe[fd % FD_STRIPES] */

/* fd's list head (made when create), or NULL past the table. */
static PasFdWait **fd_slot(int fd, int create) {
  unsigned ci = (unsigned)fd / FD_CHUNK;
  if (fd < 0 || ci >= FD_CHUNKS)
    return NULL;
  PasFdWait **ch = __atomic_load_n(&fd_parked[ci], __ATOMIC_ACQUIRE);
  if (!ch) {
    if (!create)
      return NULL;
    PasFdWait **fresh = calloc(FD_CHUNK, sizeof *fresh);
    if (!fresh)
      __pas_runtime_error("out of memory waiting on a socket");
    PasFdWait **expected = NULL;
    if (__atomic_compare_exchange_n(&fd_parked[ci], &expected, fresh, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      ch = fresh;
    else {
      free(fresh);
      ch = expected;
    }
  }
  return &ch[(unsigned)fd % FD_CHUNK];
}

static int *fd_lock_of(int fd) { return &fd_stripe[(unsigned)fd % FD_STRIPES]; }

static void fd_park_link(PasFdWait *rec) {
  PasFdWait **head = fd_slot(rec->fd, 1);
  if (!head)
    return;
  int *lk = fd_lock_of(rec->fd);
  spin_lock(lk);
  rec->fprev = NULL;
  rec->fnext = *head;
  if (rec->fnext)
    rec->fnext->fprev = rec;
  *head = rec;
  rec->parked = 1;
  spin_unlock(lk);
}

static void fd_park_unlink(PasFdWait *rec) {
  if (!rec->parked)
    return;
  int *lk = fd_lock_of(rec->fd);
  spin_lock(lk);
  if (rec->fprev)
    rec->fprev->fnext = rec->fnext;
  else
    *fd_slot(rec->fd, 0) = rec->fnext;
  if (rec->fnext)
    rec->fnext->fprev = rec->fprev;
  rec->fprev = rec->fnext = NULL;
  rec->parked = 0;
  spin_unlock(lk);
}

/* On rec's home thread: rec's wait is over with bits (its own event, or 8
 * for its deadline). Readiness also wakes the peers parked on the same
 * descriptor and direction (each retires itself on its own thread). Only
 * the first of these reports counts for a record: its res and signal are
 * written under the descriptor's stripe lock. */
static void fd_fire(PasFdWait *rec, int64_t bits) {
  int *lk = fd_lock_of(rec->fd);
  spin_lock(lk);
  if (!rec->w.signaled) {
    rec->res = bits;
    wake(&rec->w, WAKE_NORMAL);
  }
  if (bits != 8 && rec->parked) {
    for (PasFdWait *o = *fd_slot(rec->fd, 0); o; o = o->fnext)
      if (o != rec && o->write == rec->write && !o->w.signaled) {
        o->res = bits;
        wake(&o->w, WAKE_NORMAL);
      }
  }
  spin_unlock(lk);
}

/* A task found fd ready by trying its syscall, without parking (an accept
 * that succeeded at once): the waits parked on fd in that direction are
 * woken as if their readiness had fired. Their own registrations may never
 * report it: the kernel checks readiness again when it delivers an event,
 * and the syscall has just consumed it. Without this, a waiter beside a
 * busy acceptor could sleep through every connection. */
void __pas_fd_wake_peers(int32_t fd, int32_t want_write) {
  PasFdWait **head = fd_slot(fd, 0);
  if (!head || !__atomic_load_n(head, __ATOMIC_ACQUIRE))
    return;
  int *lk = fd_lock_of(fd);
  spin_lock(lk);
  for (PasFdWait *o = *head; o; o = o->fnext)
    if (o->write == (want_write ? 1 : 0) && !o->w.signaled) {
      o->res = want_write ? 2 : 1;
      wake(&o->w, WAKE_NORMAL);
    }
  spin_unlock(lk);
}

/* On c's thread: c gets its poller (and its poke). */
static void ctx_poller(PasContext *c) {
  if (c->pfd >= 0)
    return;
#if defined(_WIN32)
  /* the poke: a loopback UDP socket connected to itself, level-triggered
     readable while a byte waits */
  HANDLE p = epoll_create1(0);
  SOCKET e = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a;
  int alen = sizeof a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  u_long nb = 1;
  if (!p || e == INVALID_SOCKET || bind(e, (struct sockaddr *)&a, sizeof a) != 0 ||
      getsockname(e, (struct sockaddr *)&a, &alen) != 0 ||
      connect(e, (struct sockaddr *)&a, sizeof a) != 0 ||
      ioctlsocket(e, FIONBIO, &nb) != 0)
    __pas_runtime_error("the netasync poller could not start");
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.ptr = PAS_WAKE_MARK;
  epoll_ctl(p, EPOLL_CTL_ADD, e, &ev);
  c->wakefd = (int)(intptr_t)e;
  c->ep = p;
  c->pfd = 0;
  return;
#elif defined(__linux__)
  int p = epoll_create1(EPOLL_CLOEXEC);
  int e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (p < 0 || e < 0)
    __pas_runtime_error("the netasync poller could not start");
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.ptr = PAS_WAKE_MARK;
  epoll_ctl(p, EPOLL_CTL_ADD, e, &ev);
  c->wakefd = e;
#else
  int p = kqueue();
  if (p < 0)
    __pas_runtime_error("the netasync poller could not start");
  struct kevent k;
  EV_SET(&k, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, PAS_WAKE_MARK);
  kevent(p, &k, 1, NULL, 0, NULL);
#endif
#ifndef _WIN32
  c->pfd = p;
#endif
}

/* c's entry for descriptor fd (the table grows on demand). */
static uint8_t *ctx_fdent(PasContext *c, int fd) {
  if (fd >= c->fdcap) {
    int n = c->fdcap ? c->fdcap : 1024;
    while (n <= fd)
      n *= 2;
    uint8_t *t = realloc(c->fdtab, (size_t)n);
    if (!t)
      __pas_runtime_error("out of memory waiting on a socket");
    memset(t + c->fdcap, 0, (size_t)(n - c->fdcap));
    c->fdtab = t;
    c->fdcap = n;
  }
  return &c->fdtab[fd];
}

/* The deadline heap: the earliest deadline first, records know their slot. */
static void dheap_set(PasContext *c, int i, PasFdWait *rec) {
  c->dheap[i] = rec;
  rec->hidx = i;
}

static void dheap_up(PasContext *c, int i) {
  PasFdWait *rec = c->dheap[i];
  while (i > 0) {
    int parent = (i - 1) / 2;
    if (c->dheap[parent]->deadline_us <= rec->deadline_us)
      break;
    dheap_set(c, i, c->dheap[parent]);
    i = parent;
  }
  dheap_set(c, i, rec);
}

static void dheap_down(PasContext *c, int i) {
  PasFdWait *rec = c->dheap[i];
  for (;;) {
    int l = 2 * i + 1, best = i;
    int64_t bd = rec->deadline_us;
    if (l < c->dcount && c->dheap[l]->deadline_us < bd) {
      best = l;
      bd = c->dheap[l]->deadline_us;
    }
    if (l + 1 < c->dcount && c->dheap[l + 1]->deadline_us < bd)
      best = l + 1;
    if (best == i)
      break;
    dheap_set(c, i, c->dheap[best]);
    i = best;
  }
  dheap_set(c, i, rec);
}

static void dheap_push(PasContext *c, PasFdWait *rec) {
  if (c->dcount == c->dcap) {
    int n = c->dcap ? 2 * c->dcap : 64;
    PasFdWait **h = realloc(c->dheap, (size_t)n * sizeof *h);
    if (!h)
      __pas_runtime_error("out of memory waiting on a socket");
    c->dheap = h;
    c->dcap = n;
  }
  dheap_set(c, c->dcount++, rec);
  dheap_up(c, rec->hidx);
}

static void dheap_remove(PasContext *c, PasFdWait *rec) {
  int i = rec->hidx;
  PasFdWait *last = c->dheap[--c->dcount];
  if (i < c->dcount) {
    dheap_set(c, i, last);
    dheap_up(c, i);
    dheap_down(c, last->hidx);
  }
  rec->hidx = -1;
}

/* On rec's home thread: the record is done for its task (poll backends). A fired
 * one-shot registration is already gone (or, on epoll, disabled until the
 * next MOD re-arms it); one that did not fire is dropped; a dup is closed,
 * which drops its registration. The caller's fd itself is never closed. */
static void fd_retire(PasFdWait *rec) {
  if (rec->state != 0)
    return;
  rec->state = 1;
  PasContext *c = rec->ctx;
  fd_park_unlink(rec);
  if (rec->has_deadline) {
    dheap_remove(c, rec);
    rec->has_deadline = 0;
  }
  if (!rec->armed)
    return;
  rec->armed = 0;
  if (rec->direct) {
    uint8_t *e = ctx_fdent(c, rec->fd);
    *e &= (uint8_t)~(rec->write ? FDT_WRITE : FDT_READ);
    if (!rec->fired) {
#ifdef PAS_EPOLL
      epoll_ctl(EPH(c), EPOLL_CTL_DEL, SOCK(rec->fd), NULL);
      *e &= (uint8_t)~FDT_EPOLL_REG;
#else
      struct kevent kn;
      EV_SET(&kn, rec->fd, rec->write ? EVFILT_WRITE : EVFILT_READ, EV_DELETE,
             0, 0, NULL);
      kevent(c->pfd, &kn, 1, NULL, 0, NULL);
#endif
    }
  } else if (rec->arm_fd >= 0) {
    sock_close(rec->arm_fd);
  }
  rec->arm_fd = -1;
}

/* On c's thread: records whose deadline passed get their NW_TIMEOUT wake.
 * (io_uring pairs its waits with a linked TIMEOUT op.) */
static void ctx_fire_deadlines(PasContext *c) {
  if (c->dcount == 0)
    return;
  int64_t now = fd_now_us();
  while (c->dcount > 0 && c->dheap[0]->deadline_us <= now) {
    PasFdWait *rec = c->dheap[0];
    dheap_remove(c, rec);
    rec->has_deadline = 0;
    if (rec->state != 0)
      continue;
    fd_fire(rec, 8);   /* NW_TIMEOUT, unless readiness came first */
    fd_retire(rec);
  }
}

/* On c's thread, from loop_until (which set c->sleeping under qmu, so a post
 * from now on pokes): blocks in c's poller until an event, a poke, its
 * earliest wait deadline or deadline_ms (a CLOCK_MONOTONIC time in ms, 0 =
 * none), then delivers what fired. */
static void ctx_poll_once(PasContext *c, int64_t deadline_ms) {
  int64_t wait_ms = -1;
  if (c->dcount > 0) {
    wait_ms = (c->dheap[0]->deadline_us - fd_now_us() + 999) / 1000;
    if (wait_ms < 0)
      wait_ms = 0;
  }
  if (deadline_ms) {
    int64_t left = deadline_ms - now_ms();
    if (left < 0)
      left = 0;
    if (wait_ms < 0 || left < wait_ms)
      wait_ms = left;
  }
  if (wait_ms > 3600000)
    wait_ms = 3600000;
#ifdef PAS_EPOLL
  struct epoll_event evs[256];
  int n = epoll_wait(EPH(c), evs, 256, (int)wait_ms);
#else
  struct kevent evs[256];
  struct timespec ts, *tsp = NULL;
  if (wait_ms >= 0) {
    ts.tv_sec = wait_ms / 1000;
    ts.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
    tsp = &ts;
  }
  int n = kevent(c->pfd, NULL, 0, evs, 256, tsp);
#endif
  pthread_mutex_lock(&c->qmu);
  c->sleeping = 0;
  pthread_mutex_unlock(&c->qmu);
  for (int i = 0; i < n; i++) {
#ifdef PAS_EPOLL
    void *data = evs[i].data.ptr;
    if (data == PAS_WAKE_MARK) {
#ifdef _WIN32
      char v[16];
      while (recv(SOCK(c->wakefd), v, sizeof v, 0) > 0)
        ;
#else
      uint64_t v;
      ssize_t r = read(c->wakefd, &v, sizeof v);
      (void)r;
#endif
      continue;
    }
    int64_t bits = fd_bits_epoll(evs[i].events);
#else
    void *data = evs[i].udata;
    if (data == PAS_WAKE_MARK || (evs[i].flags & EV_ERROR))
      continue;
    int64_t bits = fd_bits_kqueue(&evs[i]);
#endif
    PasFdWait *rec = data;
    if (rec->state != 0)
      continue;
    rec->fired = 1;   /* its one-shot registration is consumed */
    fd_fire(rec, bits);
    fd_retire(rec);
  }
  ctx_fire_deadlines(c);
}

#ifdef __linux__
/* Under the lock: process one CQE of the ring (defined below the submit
 * helper; declared here because the full-ring rescue inside it reaps). */
static void uring_reap_cqe(struct pas_uring_cqe *cqe);
/* Fills and publishes the SQE of rec, then submits (under the lock; the
 * caller holds it). The slot stays occupied until a completion is reaped. */
static void uring_submit_rec(PasFdWait *rec) {
  struct pas_uring_sqe *sqe;
  unsigned tail, head, idx;
  int tries = 0;
  for (;;) {
    head = __atomic_load_n(ur_cq_head, __ATOMIC_ACQUIRE);
    tail = __atomic_load_n(ur_sq_tail, __ATOMIC_RELAXED);
    /* a record with a deadline publishes two linked SQEs */
    unsigned need = rec->has_deadline ? 2 : 1;
    if (tail - head + need <= *ur_sq_mask)
      break;
    /* full: reap what completed already; drop the lock around the yield so
     * the poller can go on reaping for the other threads */
    unsigned h2 = head;
    unsigned ct = __atomic_load_n(ur_cq_tail, __ATOMIC_ACQUIRE);
    while (h2 != ct) {
      uring_reap_cqe(&ur_cqes[h2 & *ur_sq_mask]);
      h2++;
    }
    __atomic_store_n(ur_cq_head, h2, __ATOMIC_RELEASE);
    if (tail - h2 + need <= *ur_sq_mask)
      break;
    if (tries++ > 1000)
      __pas_runtime_error("netasync: too many concurrent socket waits");
    UNLOCK();
    sched_yield();
    LOCK();
  }
  idx = tail & *ur_sq_mask;
  sqe = &ur_sqes[idx];
  memset(sqe, 0, sizeof *sqe);
  sqe->user_data = (uint64_t)(uintptr_t)rec;
  switch (rec->op) {
    case FD_OP_RECV:
      sqe->opcode = IORING_OP_RECV;
      sqe->fd = rec->fd;
      sqe->addr = (uint64_t)(uintptr_t)rec->buf;
      sqe->len = (uint32_t)rec->len;
      break;
    case FD_OP_SEND:
      sqe->opcode = IORING_OP_SEND;
      sqe->fd = rec->fd;
      sqe->addr = (uint64_t)(uintptr_t)rec->buf;
      sqe->len = (uint32_t)rec->len;
      sqe->msg_flags = MSG_NOSIGNAL;
      break;
    case FD_OP_POLLREAD:
      sqe->opcode = IORING_OP_POLL_ADD;
      sqe->fd = rec->fd;
      sqe->msg_flags = POLLIN;   /* (the poll_events field) */
      break;
    default:
      sqe->opcode = IORING_OP_POLL_ADD;
      sqe->fd = rec->fd;
      sqe->msg_flags = POLLOUT;
      break;
  }
  __atomic_store_n(ur_sq_tail, tail + 1, __ATOMIC_RELEASE);
  if (rec->has_deadline) {
    /* the wait's own op is left unlinked: an independent TIMEOUT is
     * submitted next to it, and either completes first (the pair's
     * pending/left logic in the reaper handles the remnant; no link) */
    struct pas_uring_sqe *sqe2 = &ur_sqes[(tail + 1) & *ur_sq_mask];
    memset(sqe2, 0, sizeof *sqe2);
    sqe2->user_data = (uint64_t)(uintptr_t)rec;
    sqe2->opcode = IORING_OP_TIMEOUT;
    rec->ts.tv_sec = (int64_t)rec->deadline_ms / 1000;
    rec->ts.tv_nsec = (int64_t)(rec->deadline_ms % 1000) * 1000000;
    sqe2->addr = (uint64_t)(uintptr_t)&rec->ts;
    sqe2->len = 1;
    __atomic_store_n(ur_sq_tail, tail + 2, __ATOMIC_RELEASE);
    int r = syscall(SYS_io_uring_enter, fd_poll_fd, 2, 0, 0, NULL, 0);
    (void)r;
    return;
  }
  int r = syscall(SYS_io_uring_enter, fd_poll_fd, 1, 0, 0, NULL, 0);
  (void)r;
}

/* A wait cancelled on the io_uring backend: for a poll op, ask the kernel
 * to stop it (best effort — the abandoned record never needs its
 * completion); recv/send complete on their own. */
static void uring_cancel_rec(PasFdWait *rec) {
  unsigned tail, head;
  if (rec->op != FD_OP_POLLREAD && rec->op != FD_OP_POLLWRITE)
    return;
  head = __atomic_load_n(ur_cq_head, __ATOMIC_ACQUIRE);
  tail = __atomic_load_n(ur_sq_tail, __ATOMIC_RELAXED);
  if (tail - head >= *ur_sq_mask)
    return;                    /* full: the poll completes on its own */
  struct pas_uring_sqe *sqe = &ur_sqes[tail & *ur_sq_mask];
  memset(sqe, 0, sizeof *sqe);
  sqe->opcode = IORING_OP_POLL_REMOVE;
  sqe->addr = (uint64_t)(uintptr_t)rec;
  __atomic_store_n(ur_sq_tail, tail + 1, __ATOMIC_RELEASE);
  int r = syscall(SYS_io_uring_enter, fd_poll_fd, 1, 0, 0, NULL, 0);
  (void)r;
}

/* Under the lock: one CQE reaped. The first completion of a record stores
 * the result and wakes its task; a record armed with a linked timeout has
 * a pair of completions, so the pending count falls with every member and
 * the last one frees the record unless the task already left it (then the
 * reaper frees). A cancelled record is marked freed at its cancellation
 * and every later member is skipped here. */
static void uring_reap_cqe(struct pas_uring_cqe *cqe) {
  PasFdWait *rec = (PasFdWait *)(uintptr_t)cqe->user_data;
  if (!rec || rec->freed)
    return;          /* NULL: a poll removal; freed: abandoned or done */
  if (rec->pending > 0)
    rec->pending--;
  if (!rec->seen) {
    rec->seen = 1;
    rec->res = cqe->res;
    rec->freed = rec->pending == 0;
    fd_unlink(rec);
    wake(&rec->w, WAKE_NORMAL);
  } else if (rec->pending == 0) {
    rec->freed = 1;
    if (rec->left)
      free(rec);
  }
}
#endif

/* Called under the lock: submit the kernel interest for rec. On the poll
 * backends the record arms its own dup of fd: an fd may have several
 * records parked at once (see the section comment), and the kernel would
 * otherwise merge or overwrite registrations keyed by the fd alone — epoll
 * drops a second ADD with EEXIST, and a kqueue knote carries one udata, so
 * a second EV_ADD would re-address the knote the first record parked on. */
/* Arms rec in its context's poller: 0, or -1 when the descriptor cannot be
 * waited on (closed, not pollable, out of descriptors for a dup). */
static int fd_arm(PasFdWait *rec) {
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    uring_submit_rec(rec);
    return 0;
  }
#endif
  PasContext *c = rec->ctx;
  uint8_t dir = rec->write ? FDT_WRITE : FDT_READ;
  uint8_t *e = ctx_fdent(c, rec->fd);
#ifdef PAS_EPOLL
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = (rec->write ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
  ev.data.ptr = rec;
  if (!(*e & (FDT_READ | FDT_WRITE))) {
    int r;
    if (*e & FDT_EPOLL_REG) {
      /* re-arm the registration a fired wait left disabled; the fd may
       * have been closed and its number reused since: then add */
      r = epoll_ctl(EPH(c), EPOLL_CTL_MOD, SOCK(rec->fd), &ev);
      if (r < 0 && errno == ENOENT)
        r = epoll_ctl(EPH(c), EPOLL_CTL_ADD, SOCK(rec->fd), &ev);
    } else {
      r = epoll_ctl(EPH(c), EPOLL_CTL_ADD, SOCK(rec->fd), &ev);
      if (r < 0 && errno == EEXIST)
        r = epoll_ctl(EPH(c), EPOLL_CTL_MOD, SOCK(rec->fd), &ev);
    }
    if (r < 0) {
      *e &= (uint8_t)~FDT_EPOLL_REG;
      return -1;
    }
    *e |= FDT_EPOLL_REG | dir;
    rec->direct = 1;
    rec->arm_fd = rec->fd;
    rec->armed = 1;
    return 0;
  }
  int afd = sock_dup(rec->fd);
  if (afd < 0)
    return -1;
  if (epoll_ctl(EPH(c), EPOLL_CTL_ADD, SOCK(afd), &ev) < 0) {
    sock_close(afd);
    return -1;
  }
#else
  struct kevent kn;
  if (!(*e & dir)) {
    EV_SET(&kn, rec->fd, rec->write ? EVFILT_WRITE : EVFILT_READ,
           EV_ADD | EV_ONESHOT, 0, 0, rec);
    if (kevent(c->pfd, &kn, 1, NULL, 0, NULL) < 0)
      return -1;
    *e |= dir;
    rec->direct = 1;
    rec->arm_fd = rec->fd;
    rec->armed = 1;
    return 0;
  }
  int afd = sock_dup(rec->fd);
  if (afd < 0)
    return -1;
  EV_SET(&kn, afd, rec->write ? EVFILT_WRITE : EVFILT_READ,
         EV_ADD | EV_ONESHOT, 0, 0, rec);
  if (kevent(c->pfd, &kn, 1, NULL, 0, NULL) < 0) {
    close(afd);
    return -1;
  }
#endif
  rec->direct = 0;
  rec->arm_fd = afd;
  rec->armed = 1;
  return 0;
}

/* Parks the current task on rec (linked); on return the lock is released and
 * rec is disposed of (freed by the task, or abandoned when cancelled on
 * io_uring): *out carries the io_uring completion (recv/send), otherwise
 * the mapped bits (poll backends) or the raw completion (io_uring poll
 * waits). Returns 0 completed, 1 cancelled. */
#ifdef __linux__
/* io_uring: under LOCK, rec submitted; the reaper wakes under LOCK too, so
 * WAITING is stored before any completion can be seen. */
static int fd_park(PasFdWait *rec, int64_t *out) {
  PasContext *c = context();
  PasTask *me = c->current;
  Observation o;
  observe(me, &o);
  me->wake = WAKE_NORMAL;
  __atomic_store_n(&me->state, TS_WAITING, __ATOMIC_SEQ_CST);
  UNLOCK();
  suspend(me);
  LOCK();
  unobserve(&o);
  int cancelled = me->wake == WAKE_CANCEL;
  {
    int64_t res = rec->res; /* the completion */
    if (cancelled) {
      /* no free here: a completion may follow the task's rise (or never
       * come) — the record is abandoned, marked freed so completions and
       * submissions skip it */
      rec->state = 1;
      rec->freed = 1;
      fd_waits_abandoned++;
      uring_cancel_rec(rec);   /* best effort: stop a poll still pending */
      fd_unlink(rec);
      UNLOCK();
      *out = res;
      return 1;
    }
    fd_unlink(rec);
    UNLOCK();
    *out = res;
    if (rec->freed)
      free(rec);       /* every member of the pair is reaped: all ours */
    else
      rec->left = 1;   /* the linked timeout may still complete: the
                          reaper that takes pending to 0 frees it */
    return 0;
  }
}
#endif

#ifdef __linux__
/* Parks the current task on one io_uring op (RECV or SEND): submits it,
 * waits for the completion, and retries the transient results (a readiness
 * race, or a redelivered signal) after re-parking on the fd. The result is
 * the op's completion: what recv/send would return, or negative (-errno);
 * a cancelled wait raises. */
static int64_t fd_uring_op(int op, int32_t fd, void *buf, int32_t len) {
  PasTask *me = context()->current;
  for (;;) {
    int64_t res;
    LOCK();
    if (cancelled_now(me->token)) {
      UNLOCK();
      raise_cancelled();
    }
    PasFdWait *rec = malloc(sizeof *rec);
    if (!rec)
      __pas_runtime_error("out of memory waiting on a socket");
    memset(rec, 0, sizeof *rec);
    rec->fd = fd;
    rec->arm_fd = -1;
    rec->op = op;
    rec->buf = buf;
    rec->len = len;
    rec->w.next = NULL;
    rec->w.task = me;
    rec->w.thread = NULL;
    rec->w.signaled = 0;
    rec->next = fd_waits;
    fd_waits = rec;
    fd_arm(rec);                    /* submits the op */
    int cancelled = fd_park(rec, &res);
    if (cancelled)
      raise_cancelled();            /* the record was abandoned above */
    if (res >= 0)
      return res;
    if (res == -EINTR)
      continue;
    if (res != -EAGAIN && res != -EWOULDBLOCK)
      return res;                   /* a failure to report to the caller */
    /* lost the readiness race: re-park until readable/writable */
    __pas_task_wait_fd(fd, op == FD_OP_SEND ? 1 : 0);
  }
}
#endif

#ifdef __linux__
static void *fd_poller_main(void *arg);
#endif

#ifdef __linux__
static void *fd_poller_main(void *arg) {
  (void)arg;
  if (fd_backend == FD_BACKEND_URING) {
    for (;;) {
      /* cheap check without the lock: anything to reap? */
      unsigned head = __atomic_load_n(ur_cq_head, __ATOMIC_ACQUIRE);
      unsigned tail = __atomic_load_n(ur_cq_tail, __ATOMIC_ACQUIRE);
      if (head == tail) {
        /* nothing: block until the next completion (this also submits
         * what pending SQEs tasks left published in the ring) */
        syscall(SYS_io_uring_enter, fd_poll_fd, 0, 1,
                IORING_ENTER_GETEVENTS, NULL, 0);
        continue;
      }
      LOCK();
      /* reap fresh state (tasks may have reaped under the lock meanwhile) */
      head = __atomic_load_n(ur_cq_head, __ATOMIC_RELAXED);
      tail = __atomic_load_n(ur_cq_tail, __ATOMIC_ACQUIRE);
      while (head != tail) {
        uring_reap_cqe(&ur_cqes[head & *ur_sq_mask]);
        head++;
      }
      __atomic_store_n(ur_cq_head, head, __ATOMIC_RELEASE);
      UNLOCK();
    }
  }
  return NULL;
}
#endif

/* The wait records a cancelled io_uring wait abandoned on purpose (a
 * completion may follow the task's rise, or never come): the record leaks
 * by design, ~72 bytes each, bounded by the number of cancellations. The
 * poll backends retire their records cleanly, so this stays 0 there. Leak
 * tests compare this counter, not the allocator. For unit netasync. */
int64_t __pas_net_waits_abandoned(void) {
  return fd_waits_abandoned;
}

/* The poller backend in place: 0 kevent, 1 epoll, 2 io_uring. For benchmark
 * reports and logs. For unit netasync. */
int32_t __pas_net_fd_backend(void) {
  return fd_backend;
}

/* Marks a descriptor blocking (on 0) or non-blocking, through the C runtime
 * (plain C can call the variadic fcntl; calls emitted by p3 threads cannot).
 * For unit netasync. */
void __pas_fd_nonblock(int32_t fd, int32_t on) {
#ifdef _WIN32
  u_long v = on ? 1 : 0;
  ioctlsocket(SOCK(fd), FIONBIO, &v);
  return;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return;
#ifdef __linux__
  if (on)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
#else
  if (on)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
#endif
  fcntl(fd, F_SETFL, flags);
#endif
}

/* Called once, from unit netasync's initialization (plain context). */
void __pas_fd_poller_start(void) {
  LOCK();
  fd_poller_start();
  UNLOCK();
}

/* (the backend is chosen once; fd_poll_started is published last) */
static void fd_poller_start_once(void) {
  if (__atomic_load_n(&fd_poll_started, __ATOMIC_ACQUIRE))
    return;
  LOCK();
  fd_poller_start();
  UNLOCK();
}

static void fd_poller_start(void) {
  if (fd_poll_started)
    return;
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  fd_backend = FD_BACKEND_EPOLL;   /* wepoll: each thread makes its own */
#elif defined(__linux__)
  fd_backend = FD_BACKEND_EPOLL;
  /* Per-thread epoll is the default: readiness reaches the task on its own
   * thread, in batches. The shared io_uring ring (one poller thread hands
   * every completion to the pool) stays available with P3_NET_URING=1. */
  const char *uenv = getenv("P3_NET_URING");
  if (uenv && uenv[0] == '1') { /* io_uring; falls back to epoll when the kernel has none */
    struct pas_uring_params params;
    memset(&params, 0, sizeof params);
    int fd = syscall(SYS_io_uring_setup, PAS_URING_ENTRIES, &params);
    if (fd >= 0 && params.sq_off.array && params.cq_off.cqes) {
      size_t sq_len = params.sq_off.array +
                      (size_t)params.sq_entries * sizeof(unsigned);
      size_t cq_len = params.cq_off.cqes +
                      (size_t)params.cq_entries * sizeof(struct pas_uring_cqe);
      size_t sqe_len = (size_t)params.sq_entries * sizeof(struct pas_uring_sqe);
      ur_sq_ring = mmap(NULL, sq_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
      ur_cq_ring = mmap(NULL, cq_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
      ur_sqes = mmap(NULL, sqe_len, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
      if (ur_sq_ring != MAP_FAILED && ur_cq_ring != MAP_FAILED &&
          ur_sqes != MAP_FAILED) {
        ur_sq_head = (unsigned *)((char *)ur_sq_ring + params.sq_off.head);
        ur_sq_tail = (unsigned *)((char *)ur_sq_ring + params.sq_off.tail);
        ur_sq_mask = (unsigned *)((char *)ur_sq_ring + params.sq_off.ring_mask);
        ur_sq_array = (unsigned *)((char *)ur_sq_ring + params.sq_off.array);
        ur_cq_head = (unsigned *)((char *)ur_cq_ring + params.cq_off.head);
        ur_cq_tail = (unsigned *)((char *)ur_cq_ring + params.cq_off.tail);
        ur_cq_mask = (unsigned *)((char *)ur_cq_ring + params.cq_off.ring_mask);
        ur_cqes = (struct pas_uring_cqe *)((char *)ur_cq_ring +
                                           params.cq_off.cqes);
        /* the SQ array is userspace's job: map each index to itself (the
         * kernel reads SQEs through it — an all-zero array resubmits the
         * first SQE over and over and drops the rest) */
        for (unsigned i = 0; i < params.sq_entries; i++)
          ur_sq_array[i] = i;
        *ur_sq_tail = 0;
        fd_poll_fd = fd;
        fd_backend = FD_BACKEND_URING;
      } else {
        if (ur_sq_ring != MAP_FAILED)
          munmap(ur_sq_ring, sq_len);
        if (ur_cq_ring != MAP_FAILED)
          munmap(ur_cq_ring, cq_len);
        if (ur_sqes != MAP_FAILED)
          munmap(ur_sqes, sqe_len);
        close(fd);
      }
    }
  }
  if (fd_backend != FD_BACKEND_URING) {
    __atomic_store_n(&fd_poll_started, 1, __ATOMIC_RELEASE);
    return;   /* epoll: each thread makes its own poller (ctx_poller) */
  }
  pthread_attr_t a2;
  pthread_attr_init(&a2);
  pthread_attr_setdetachstate(&a2, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&fd_poller_thread, &a2, fd_poller_main, NULL) != 0)
    __pas_runtime_error("the netasync poller thread could not start");
  pthread_attr_destroy(&a2);
#else
  fd_backend = FD_BACKEND_KQUEUE;   /* each thread makes its own kqueue */
#endif
  __atomic_store_n(&fd_poll_started, 1, __ATOMIC_RELEASE);
}

/* Parks the current task until fd is readable (want_write 0) or writable
 * (want_write 1), and reports what fired: its integer result is the bitmask
 * 1 = readable, 2 = writable, 4 = the descriptor is in an error state
 * (POLLERR/POLLHUP revents on the poll backends, a negative POLL_ADD
 * completion on io_uring), and 8 = the deadline expired first (only
 * __pas_task_wait_fd2). Outside a task, it blocks this thread with
 * poll(). A cancelled token woke the task with EOperationCancelled
 * (raised here; on io_uring the record is abandoned and leaks, unlike a
 * deadline, whose pair completes and frees normally). */
int32_t __pas_task_wait_fd(int32_t fd, int32_t want_write) {
  return __pas_task_wait_fd2(fd, want_write, 0);
}

#ifdef __linux__
/* The io_uring wait: a POLL_ADD (and a linked timeout for a deadline) on
 * the shared ring, under LOCK like the rest of the ring's state. */
static int32_t wait_fd_uring(PasTask *me, int32_t fd, int32_t want_write,
                             int32_t deadline_ms) {
  LOCK();
  PasFdWait *rec = malloc(sizeof *rec);
  if (!rec)
    __pas_runtime_error("out of memory waiting on a socket");
  memset(rec, 0, sizeof *rec);
  rec->fd = fd;
  rec->arm_fd = -1;
  rec->ctx = context();
  rec->hidx = -1;
  rec->write = want_write ? 1 : 0;
  rec->op = want_write ? FD_OP_POLLWRITE : FD_OP_POLLREAD;
  rec->pending = 1;   /* one completion; a deadline makes it two */
  if (deadline_ms > 0) {
    rec->has_deadline = 1;
    rec->deadline_ms = deadline_ms;
    rec->deadline_us = fd_now_us() + (int64_t)deadline_ms * 1000;
    rec->pending = 2;
  }
  rec->w.task = me;
  rec->next = fd_waits;
  fd_waits = rec;
  fd_arm(rec);
  int64_t fired;
  int cancelled = fd_park(rec, &fired);   /* (returns unlocked) */
  if (cancelled)
    raise_cancelled();
  /* raw POLL_ADD revents come in the completion (negative on failure);
   * a deadline's pair carries -ECANCELLED / -ETIME, both a timeout */
  int32_t bits = 0;
  if (fired < 0) {
    if (-fired == ECANCELLED || -fired == ETIME)
      bits = 8;
    else
      bits = 4;
  } else {
    if (fired & 1)
      bits |= 1;               /* POLLIN */
    if (fired & 4)
      bits |= 2;               /* POLLOUT */
    if (fired & (8 | 16 | 32))
      bits |= 4;               /* POLLERR | POLLHUP | POLLNVAL */
  }
  return bits;
}
#endif

int32_t __pas_task_wait_fd2(int32_t fd, int32_t want_write, int32_t deadline_ms) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (!me) {
    pollfd_t pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = SOCK(fd);
    pf.events = want_write ? POLLOUT : POLLIN;
    int64_t remain = deadline_ms;
    for (;;) {
      if (deadline_ms > 0 && remain <= 0)
        return 8;
      int poll_ms = 1000;
      if (deadline_ms > 0 && remain < 1000)
        poll_ms = (int)remain;
      int r = sock_poll(&pf, 1, poll_ms);
      if (r > 0) {
        int32_t bits = 0;
        if (pf.revents & POLLIN)
          bits |= 1;
        if (pf.revents & POLLOUT)
          bits |= 2;
        if (pf.revents & (POLLERR | POLLHUP | POLLNVAL))
          bits |= 4;
        return bits;
      }
      if (deadline_ms > 0)
        remain -= poll_ms;
    }
  }
  if (cancelled_now(me->token))
    raise_cancelled();
  fd_poller_start_once();
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING)
    return wait_fd_uring(me, fd, want_write, deadline_ms);
#endif
  /* poll backends: everything here is this thread's (its poller, deadline
   * heap and descriptor table) but the descriptor's peer list */
  ctx_poller(c);
  PasFdWait *rec = malloc(sizeof *rec);
  if (!rec)
    __pas_runtime_error("out of memory waiting on a socket");
  memset(rec, 0, sizeof *rec);
  rec->fd = fd;
  rec->arm_fd = -1;
  rec->ctx = c;
  rec->hidx = -1;
  rec->write = want_write ? 1 : 0;
  rec->w.task = me;
  if (deadline_ms > 0) {
    rec->has_deadline = 1;
    rec->deadline_us = fd_now_us() + (int64_t)deadline_ms * 1000;
    dheap_push(c, rec);
  }
  /* peers first, then the kernel: once armed, a readiness can be seen
   * and consumed on another thread (an accept) before this thread's poller
   * looks — the firing peer's fan-out is then this wait's only wake, so it
   * must already find it (an early wake just signals it: park sees that) */
  fd_park_link(rec);
  if (fd_arm(rec) < 0) {
    /* not waitable (closed, not pollable, no descriptor for a dup):
     * report the error state instead of parking forever */
    fd_retire(rec);
    free(rec);
    return 4;
  }
  Observation o;
  observe(me, &o);
  int cancelled = park(me, woken, &rec->w, 1);
  unobserve(&o);
  fd_retire(rec);   /* (its stripe lock orders the res write before this read) */
  int64_t bits = rec->res;
  free(rec);
  if (cancelled)
    raise_cancelled();
  return (int32_t)bits;
}

/* Parks the current task until recv(fd, buf, len) completes; the result: the
 * count read, 0 at the end of the connection, or negative (-errno). Outside
 * a task: polls, then recvs. */
int64_t __pas_task_recv(int32_t fd, void *buf, int32_t len) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (!me) {
    pollfd_t pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = SOCK(fd);
    pf.events = POLLIN;
    for (;;) {
      while (sock_poll(&pf, 1, 1000) <= 0)
        ;
      int64_t n = recv(SOCK(fd), buf, len, 0);
      int err = n < 0 ? net_err() : 0;
      if (n < 0 && err == NET_EINTR)
        continue;
      if (n < 0 && err == NET_EAGAIN)
        continue;
      return n < 0 ? -(int64_t)err : n;
    }
  }
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    /* native async recv: the completion is the recv result */
    return fd_uring_op(FD_OP_RECV, fd, buf, len);
  }
#endif
  /* poll backends: try the recv first — data that is already there (a
   * pipelined request, a reply that beat the task back) costs no park —
   * and park on readiness (raises on cancellation) only on EAGAIN */
  for (;;) {
    int64_t n = recv(SOCK(fd), buf, len, 0);
    if (n > 0)
      return n;
    if (n == 0)
      return 0;
    int err = net_err();
    if (err == NET_EINTR)
      continue;
    if (err == NET_EAGAIN) {
      __pas_task_wait_fd(fd, 0);
      continue;
    }
    return -(int64_t)err;
  }
}

/* Same parking protocol for send(fd, buf, len). */
int64_t __pas_task_send(int32_t fd, void *buf, int32_t len) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (!me) {
    int64_t n;
    for (;;) {
      n = send(SOCK(fd), buf, len, MSG_NOSIGNAL);
      if (n >= 0)
        return n;
      int err = net_err();
      if (err == NET_EINTR)
        continue;
      if (err == NET_EAGAIN) {
        pollfd_t pf;
        memset(&pf, 0, sizeof pf);
        pf.fd = SOCK(fd);
        pf.events = POLLOUT;
        sock_poll(&pf, 1, 1000);
        continue;
      }
      return -(int64_t)err;
    }
  }
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    /* native async send: the completion is the send result */
    return fd_uring_op(FD_OP_SEND, fd, buf, len);
  }
#endif
  for (;;) {
    int64_t n = send(SOCK(fd), buf, len, MSG_NOSIGNAL);
    if (n >= 0)
      return n;
    int err = net_err();
    if (err == NET_EINTR)
      continue;
    if (err == NET_EAGAIN) {
      __pas_task_wait_fd(fd, 0 + 1);
      continue;
    }
    return -(int64_t)err;
  }
}


/*===- Mutexes and wait queues (for unit threading) -------------------------===*/

typedef struct PasWaitQueue {
  int lk;
  int nwait;          /* waiters in the list: a signal with none is skipped */
  PasWaiter *waiters;
} PasWaitQueue;

void *__pas_mutex_new(void) {
  pthread_mutex_t *m = malloc(sizeof *m);
  if (!m)
    __pas_runtime_error("out of memory for a mutex");
  pthread_mutex_init(m, NULL);
  return m;
}

void __pas_mutex_free(void *m) {
  if (m) {
    pthread_mutex_destroy(m);
    free(m);
  }
}

/* Sequentially consistent atomics on 32- and 64-bit integers: unit threading's
   TAtomic<Integer> and TAtomic<Int64> use them instead of a mutex. */
int32_t __pas_atomic_load32(int32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void __pas_atomic_store32(int32_t *p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
int32_t __pas_atomic_xchg32(int32_t *p, int32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
int32_t __pas_atomic_cas32(int32_t *p, int32_t expected, int32_t v) {
  return __atomic_compare_exchange_n(p, &expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
int32_t __pas_atomic_add32(int32_t *p, int32_t d) { return __atomic_add_fetch(p, d, __ATOMIC_SEQ_CST); }
int64_t __pas_atomic_load64(int64_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void __pas_atomic_store64(int64_t *p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
int64_t __pas_atomic_xchg64(int64_t *p, int64_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
int32_t __pas_atomic_cas64(int64_t *p, int64_t expected, int64_t v) {
  return __atomic_compare_exchange_n(p, &expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
int64_t __pas_atomic_add64(int64_t *p, int64_t d) { return __atomic_add_fetch(p, d, __ATOMIC_SEQ_CST); }

void __pas_mutex_lock(void *m) { pthread_mutex_lock(m); }
void __pas_mutex_unlock(void *m) { pthread_mutex_unlock(m); }

void *__pas_waitq_new(void) {
  PasWaitQueue *q = calloc(1, sizeof *q);
  if (!q)
    __pas_runtime_error("out of memory for a wait queue");
  return q;
}

void __pas_waitq_free(void *q) { free(q); }

static int woken(void *arg) {
  return __atomic_load_n(&((PasWaiter *)arg)->signaled, __ATOMIC_SEQ_CST);
}

/* Under q->lk: the first waiter not signaled yet gets the signal. */
static void waitq_signal_locked(PasWaitQueue *q) {
  for (PasWaiter *w = q->waiters; w; w = w->next)
    if (!w->signaled) {
      wake(w, WAKE_NORMAL);
      return;
    }
}

/* Like a condition variable: releases mutex m, waits on q until woken, then
 * takes m again. A task waits (cancellable: then EOperationCancelled, with
 * m taken); a thread outside any task runs its tasks meanwhile. */
void __pas_waitq_wait(void *qp, void *m) {
  PasWaitQueue *q = qp;
  PasContext *c = context();
  PasTask *me = c->current;
  if (me && cancelled_now(me->token))
    raise_cancelled();
  PasWaiter w = {NULL, me, me ? NULL : c, 0};
  spin_lock(&q->lk);
  add_waiter(&q->waiters, &w);
  __atomic_add_fetch(&q->nwait, 1, __ATOMIC_SEQ_CST);
  spin_unlock(&q->lk);
  int cancelled = 0;
  if (!me) {
    pthread_mutex_unlock(m);
    loop_until(c, woken, &w, 0);
  } else {
    Observation o;
    observe(me, &o);
    pthread_mutex_unlock(m);
    cancelled = park(me, woken, &w, 1);
    unobserve(&o);
  }
  spin_lock(&q->lk);
  remove_waiter(&q->waiters, &w);
  __atomic_sub_fetch(&q->nwait, 1, __ATOMIC_SEQ_CST);
  if (cancelled && w.signaled)
    waitq_signal_locked(q); /* (a signal taken on the way out: pass it on) */
  spin_unlock(&q->lk);
  pthread_mutex_lock(m);
  if (cancelled)
    raise_cancelled();
}

/* Callers hold the mutex that the waiters released in __pas_waitq_wait, and a
 * waiter counts itself in before it releases it: nwait = 0 here means nobody
 * waits, and the spin lock and the list walk can be skipped. */
void __pas_waitq_signal(void *qp) {
  PasWaitQueue *q = qp;
  if (__atomic_load_n(&q->nwait, __ATOMIC_SEQ_CST) == 0)
    return;
  spin_lock(&q->lk);
  waitq_signal_locked(q);
  spin_unlock(&q->lk);
}

void __pas_waitq_broadcast(void *qp) {
  PasWaitQueue *q = qp;
  spin_lock(&q->lk);
  for (PasWaiter *w = q->waiters; w; w = w->next)
    wake(w, WAKE_NORMAL);
  spin_unlock(&q->lk);
}

/*===- WhenAll / WhenAny -----------------------------------------------------===*/

/* WhenAll's task: awaits each of n tasks, then its result is a dynamic
 * array of their results (elem: their type). */
typedef struct {
  PasTask **tasks;
  int64_t n;
  const PasDynInfo *elem;
} WhenArgs;

static void when_all_entry(PasTask *self) {
  WhenArgs *a = __pas_task_args(self);
  for (int64_t i = 0; i < a->n; ++i)
    __pas_task_await(a->tasks[i]);
  void **result = __pas_task_result(self);
  if (a->elem) {
    char *arr = __pas_dyn_new(a->elem, a->n);
    for (int64_t i = 0; i < a->n; ++i) {
      memcpy(arr + i * a->elem->elem_size, __pas_task_result(a->tasks[i]),
             (size_t)a->elem->elem_size);
      if (a->elem->addref)
        a->elem->addref(arr + i * a->elem->elem_size);
    }
    *result = arr;
  }
}

static int any_ended(void *arg) {
  WhenArgs *a = arg;
  for (int64_t i = 0; i < a->n; ++i)
    if (task_ended(a->tasks[i]))
      return 1;
  return 0;
}

static void when_any_entry(PasTask *self) {
  WhenArgs *a = __pas_task_args(self);
  int32_t *result = __pas_task_result(self);
  PasContext *c = context();
  PasTask *me = c->current;
  for (;;) {
    for (int64_t i = 0; i < a->n; ++i)
      if (task_ended(a->tasks[i])) {
        *result = (int32_t)i;
        return;
      }
    if (cancelled_now(me->token))
      raise_cancelled();
    PasWaiter ws[64];
    int64_t k = a->n < 64 ? a->n : 64;
    for (int64_t i = 0; i < k; ++i) {
      memset(&ws[i], 0, sizeof ws[i]);
      ws[i].task = me;
      spin_lock(&a->tasks[i]->lk);
      add_waiter(&a->tasks[i]->waiters, &ws[i]);
      spin_unlock(&a->tasks[i]->lk);
    }
    Observation o;
    observe(me, &o);
    int cancelled = park(me, any_ended, a, 1);
    unobserve(&o);
    for (int64_t i = 0; i < k; ++i) {
      spin_lock(&a->tasks[i]->lk);
      remove_waiter(&a->tasks[i]->waiters, &ws[i]);
      spin_unlock(&a->tasks[i]->lk);
    }
    if (cancelled)
      raise_cancelled();
  }
}

static void when_fini(void *args) {
  WhenArgs *a = args;
  for (int64_t i = 0; i < a->n; ++i)
    __pas_task_release(a->tasks[i]);
  free(a->tasks);
}

static PasTask *when_new(PasTask *const *tasks, int64_t n, const PasDynInfo *result,
                         void (*entry)(PasTask *), const PasDynInfo *elem) {
  PasTask *t = __pas_task_new(result, sizeof(WhenArgs), entry, when_fini);
  WhenArgs *a = __pas_task_args(t);
  a->n = n;
  a->elem = elem;
  a->tasks = malloc((size_t)(n ? n : 1) * sizeof(PasTask *));
  for (int64_t i = 0; i < n; ++i) {
    if (!tasks[i])
      __pas_runtime_error("WhenAll/WhenAny of a nil task");
    a->tasks[i] = tasks[i];
    __pas_task_addref(tasks[i]);
  }
  __pas_task_start(t);
  return t;
}

/* WhenAll(tasks): result_info is that of the dynamic array, elem that of
 * the tasks' results (NULL: no results, no array). */
PasTask *__pas_when_all(PasTask *const *tasks, int64_t n, const PasDynInfo *result_info,
                        const PasDynInfo *elem) {
  return when_new(tasks, n, result_info, when_all_entry, elem);
}

PasTask *__pas_when_any(PasTask *const *tasks, int64_t n, const PasDynInfo *int_info) {
  return when_new(tasks, n, int_info, when_any_entry, NULL);
}

/*===- Iterators (generators) ------------------------------------------------===*/
/* An iterator routine's call is a generator: its body runs on a stack of
 * its own, from the first __pas_gen_next until it yields (a value in its
 * slot) or ends; each __pas_gen_next resumes it. No scheduler: the
 * consumer and the generator switch directly, on the consumer's thread.
 * Dropping the last reference to a suspended generator resumes it once
 * more with `stopping` set: its yield raises EIteratorStop (a TObject, not
 * an Exception), so its finally parts run, and the stop ends it. */

enum { GS_NEW, GS_RUNNING, GS_SUSPENDED, GS_DONE };

typedef struct PasGen {
  int64_t refs;
  int state;
  int stopping;
  int has_value;                  /* the slot holds a value (to release) */
  const PasDynInfo *info;         /* the values' type */
  void (*entry)(struct PasGen *);
  void (*args_fini)(void *args);
  int64_t value_size;
  PasCtx ctx, back;               /* its own; the consumer's, while it runs */
  void *stack;
  const void *back_bottom;        /* (the consumer's stack, for ASan) */
  size_t back_size;
  void *fake_stack;
  PasExcFrame *exc_top;           /* its exception frames, while switched out */
  void *exception;                /* it failed: the exception, until raised */
  const char *exc_where;
  struct PasGen *outer;           /* the generator running before it */
  char data[];                    /* the value, then the arguments */
} PasGen;

/* The generator whose body is running on this thread (yield's). */
_Thread_local PasGen *__pas_gen_current;

extern void *const __pas_iteratorstop_vmt[7];

PasGen *__pas_gen_new(const PasDynInfo *info, int64_t args_size,
                      void (*entry)(PasGen *), void (*args_fini)(void *)) {
  int64_t vs = round16(info->elem_size);
  PasGen *g = calloc(1, sizeof(PasGen) + (size_t)vs + (size_t)args_size);
  if (!g)
    __pas_runtime_error("out of memory for an iterator");
  g->refs = 1;
  g->info = info;
  g->entry = entry;
  g->args_fini = args_fini;
  g->value_size = vs;
  return g;
}

void *__pas_gen_args(PasGen *g) { return g->data + g->value_size; }
void *__pas_gen_value(PasGen *g) { return g->data; }

static void gen_main(void) {
  PasGen *g = __pas_gen_current;
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(NULL, &g->back_bottom, &g->back_size);
#endif
  PasExcFrame f;
  memset(&f, 0, sizeof f);
  __pas_exc_push(&f);
  if (_setjmp(*(jmp_buf *)(void *)f.buf) == 0) {
    g->entry(g);
    __pas_exc_pop(&f);
  } else if (f.object && *(void *const **)f.object == __pas_iteratorstop_vmt) {
    __pas_free_object(f.object); /* stopped: an ordinary end */
  } else {
    g->exception = f.object;
    g->exc_where = f.where;
  }
  if (g->args_fini)
    g->args_fini(__pas_gen_args(g));
  g->args_fini = NULL;
  g->state = GS_DONE;
  g->exc_top = NULL;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(NULL, g->back_bottom, g->back_size);
#endif
#ifdef PAS_FAST_CTX
  void *dead;
  __pas_ctx_switch(&dead, g->back.sp); /* (its stack is freed there) */
#else
  setcontext(&g->back); /* (its stack is freed there) */
#endif
  __pas_runtime_error("an iterator resumed after it ended");
}

/* Runs g until it yields or ends. */
static void gen_resume(PasGen *g) {
  if (g->state == GS_NEW) {
    g->stack = new_stack();
#ifdef PAS_FAST_CTX
    ctx_make(&g->ctx, g->stack, stack_size(), gen_main);
#else
    getcontext(&g->ctx);
    g->ctx.uc_stack.ss_sp = g->stack;
    g->ctx.uc_stack.ss_size = stack_size();
    g->ctx.uc_link = NULL;
    makecontext(&g->ctx, gen_main, 0);
#endif
  }
  PasExcFrame *saved = __pas_exc_top;
  __pas_exc_top = g->exc_top;
  g->outer = __pas_gen_current;
  __pas_gen_current = g;
  g->state = GS_RUNNING;
#ifdef PAS_ASAN
  void *fake = NULL;
  __sanitizer_start_switch_fiber(&fake, g->stack, stack_size());
#endif
#ifdef PAS_FAST_CTX
  __pas_ctx_switch(&g->back.sp, g->ctx.sp);
#else
  swapcontext(&g->back, &g->ctx);
#endif
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(fake, NULL, NULL);
#endif
  __pas_gen_current = g->outer;
  __pas_exc_top = saved;
  if (g->state == GS_DONE && g->stack) {
    free_stack(g->stack);
    g->stack = NULL;
  }
}

/* The next value: 1 (in the slot), or 0 at the end; raises what the body
 * raised. */
int32_t __pas_gen_next(PasGen *g) {
  if (!g)
    __pas_runtime_error("use of a nil iterator");
  if (g->state == GS_RUNNING)
    __pas_runtime_error("an iterator asked for its next value while it runs");
  if (g->state == GS_DONE)
    return 0;
  gen_resume(g);
  if (g->exception) {
    void *e = g->exception;
    g->exception = NULL;
    __pas_raise(e, g->exc_where ? g->exc_where : "");
  }
  return g->state == GS_SUSPENDED;
}

/* In an iterator's body, before a value is stored: its slot. */
void *__pas_gen_slot(void) {
  PasGen *g = __pas_gen_current;
  if (!g)
    __pas_runtime_error("yield outside an iterator");
  g->has_value = 1;
  return g->data;
}

static void raise_stop(void) __attribute__((noreturn));
static void raise_stop(void) {
  __pas_raise_message(__pas_iteratorstop_vmt, "iterator stopped");
}

/* In an iterator's body, its value stored: back to the consumer until the
 * next value is asked for. */
void __pas_gen_yield(void) {
  PasGen *g = __pas_gen_current;
  if (!g)
    __pas_runtime_error("yield outside an iterator");
  if (g->stopping)
    raise_stop();
  g->state = GS_SUSPENDED;
  g->exc_top = __pas_exc_top;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(&g->fake_stack, g->back_bottom, g->back_size);
#endif
#ifdef PAS_FAST_CTX
  __pas_ctx_switch(&g->ctx.sp, g->back.sp);
#else
  swapcontext(&g->ctx, &g->back);
#endif
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(g->fake_stack, &g->back_bottom, &g->back_size);
#endif
  if (g->stopping)
    raise_stop();
}

void __pas_gen_addref(PasGen *g) {
  if (g)
    __atomic_fetch_add(&g->refs, 1, __ATOMIC_RELAXED);
}

void __pas_gen_release(PasGen *g) {
  if (!g || __atomic_sub_fetch(&g->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  if (g->state == GS_SUSPENDED) { /* its finally parts run */
    g->stopping = 1;
    g->refs = 1; /* (alive while it unwinds) */
    gen_resume(g);
    if (g->exception) { /* (raised while it unwound: dropped) */
      __pas_free_object(g->exception);
      g->exception = NULL;
    }
  }
  if (g->state == GS_NEW && g->args_fini)
    g->args_fini(__pas_gen_args(g));
  if (g->has_value && g->info->fini)
    g->info->fini(g->data);
  free(g);
}

void __pas_gen_assign(PasGen **dst, PasGen *src) {
  __pas_gen_addref(src);
  PasGen *old = *dst;
  *dst = src;
  __pas_gen_release(old);
}

void __pas_gen_clear(PasGen **p) {
  PasGen *old = *p;
  *p = NULL;
  __pas_gen_release(old);
}
