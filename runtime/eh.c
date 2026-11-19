/*===- Table-based exceptions: ABI skeleton (optimization2 item 8a) --------===
 *
 * The ABI that invoke/landingpad lowering will use: an exception header
 * attached to a Pascal object, the Itanium call-site/action/type LSDA
 * subset, and the personality routine. Generated code does not use it yet;
 * __pas_eh_selftest decodes a synthetic LSDA so the encoding and catch
 * matching are tested first.
 */
#include "pascalrt.h"

#if defined(__unix__) || defined(__APPLE__)

#include <stdint.h>
#include <string.h>

enum {
  DW_EH_PE_omit = 0xff,
  DW_EH_PE_absptr = 0x00,
  DW_EH_PE_uleb128 = 0x01
};

typedef struct PasLSDA {
  const unsigned char *callSites;
  const unsigned char *actionTable;
  const unsigned char *typeTable;
  const unsigned char *end;
  uintptr_t lpStart;
  unsigned typeEncoding;
} PasLSDA;

static uint64_t readULEB(const unsigned char **pp, const unsigned char *end) {
  uint64_t v = 0;
  unsigned shift = 0;
  while (*pp < end && shift < 64) {
    unsigned char b = *(*pp)++;
    v |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80))
      break;
    shift += 7;
  }
  return v;
}

static int64_t readSLEB(const unsigned char **pp, const unsigned char *end) {
  uint64_t v = 0;
  unsigned shift = 0;
  unsigned char b = 0;
  while (*pp < end && shift < 64) {
    b = *(*pp)++;
    v |= (uint64_t)(b & 0x7f) << shift;
    shift += 7;
    if (!(b & 0x80))
      break;
  }
  if (shift < 64 && (b & 0x40))
    v |= ~(uint64_t)0 << shift;
  return (int64_t)v;
}

/* The standard LSDA prologue: LPStart, TType + its offset, call-site
 * encoding, call-site table length, call-site table, action table. */
static int lsdaInit(PasLSDA *l, const unsigned char *data,
                    const unsigned char *end, uintptr_t lpStart) {
  const unsigned char *p = data;
  if (p >= end)
    return 0;
  unsigned lpEnc = *p++;
  if (lpEnc == DW_EH_PE_omit) {
    l->lpStart = lpStart;
  } else if (lpEnc == DW_EH_PE_absptr && p + sizeof(uintptr_t) <= end) {
    memcpy(&l->lpStart, p, sizeof(uintptr_t));
    p += sizeof(uintptr_t);
  } else {
    return 0;
  }
  if (p >= end)
    return 0;
  l->typeEncoding = *p++;
  if (l->typeEncoding != DW_EH_PE_omit) {
    uint64_t off = readULEB(&p, end);
    if (p + off > end)
      return 0;
    l->typeTable = p + off;
  } else {
    l->typeTable = NULL;
  }
  if (p >= end)
    return 0;
  ++p; /* call-site encoding */
  uint64_t n = readULEB(&p, end);
  if (p + n > end)
    return 0;
  l->callSites = p;
  l->actionTable = p + n;
  l->end = end;
  return l->actionTable <= end;
}

static int readPointer(const unsigned char **pp, const unsigned char *end,
                       unsigned encoding, uintptr_t *out) {
  if (encoding != DW_EH_PE_absptr || *pp + sizeof(uintptr_t) > end)
    return 0;
  memcpy(out, *pp, sizeof(uintptr_t));
  *pp += sizeof(uintptr_t);
  return 1;
}

static int lsdaCallSite(const PasLSDA *l, uintptr_t ip, uintptr_t *landing,
                        const unsigned char **action) {
  const unsigned char *p = l->callSites;
  while (p < l->actionTable) {
    uint64_t start = readULEB(&p, l->end);
    uint64_t len = readULEB(&p, l->end);
    uint64_t pad = readULEB(&p, l->end);
    uint64_t act = readULEB(&p, l->end);
    if (ip >= l->lpStart + start && ip < l->lpStart + start + len) {
      *landing = l->lpStart + pad;
      *action = act ? l->actionTable + act - 1 : NULL;
      return 1;
    }
  }
  return 0;
}

/* The first catch filter matching obj (1-based type index), or 0. */
static int lsdaCatch(const PasLSDA *l, const unsigned char *p, void *obj) {
  while (p && p < l->end) {
    const unsigned char *q = p;
    int64_t filter = readSLEB(&q, l->end);
    int64_t next = readSLEB(&q, l->end);
    if (filter > 0 && l->typeTable) {
      const unsigned char *tp = l->typeTable;
      uintptr_t type = 0;
      for (int64_t i = 1; i < filter; ++i)
        if (!readPointer(&tp, l->end, l->typeEncoding, &type))
          return 0;
      if (readPointer(&tp, l->end, l->typeEncoding, &type) &&
          (type == 0 || __pas_class_is(obj, (void *const *)type)))
        return (int)filter;
    }
    if (!next)
      break;
    p += next;
  }
  return 0;
}

static int lsdaHasCleanup(const PasLSDA *l, const unsigned char *p) {
  while (p && p < l->end) {
    const unsigned char *q = p;
    int64_t filter = readSLEB(&q, l->end);
    int64_t next = readSLEB(&q, l->end);
    if (filter == 0)
      return 1;
    if (!next)
      break;
    p += next;
  }
  return 0;
}

_Unwind_Reason_Code __pas_personality(int version, _Unwind_Action actions,
                                      uint64_t exceptionClass,
                                      struct _Unwind_Exception *unwind,
                                      struct _Unwind_Context *context) {
  (void)exceptionClass;
  if (version != 1)
    return _URC_FATAL_PHASE1_ERROR;
  const unsigned char *data =
      (const unsigned char *)_Unwind_GetLanguageSpecificData(context);
  if (!data)
    return _URC_CONTINUE_UNWIND;
  uintptr_t region = (uintptr_t)_Unwind_GetRegionStart(context);
  PasLSDA l;
  /* LSDA records are compiler-generated and unframed; 64 KiB is a safe
   * bound for this stage's small tables. */
  if (!lsdaInit(&l, data, data + 0x10000, region))
    return _URC_CONTINUE_UNWIND;
  uintptr_t landing = 0;
  const unsigned char *action = NULL;
  if (!lsdaCallSite(&l, (uintptr_t)_Unwind_GetIP(context), &landing, &action) ||
      !landing)
    return _URC_CONTINUE_UNWIND;
  PasUnwindException *e = (PasUnwindException *)unwind;
  int catch = (e->object && action) ? lsdaCatch(&l, action, e->object) : 0;
  if (actions & _UA_SEARCH_PHASE)
    return catch ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
  if (actions & _UA_CLEANUP_PHASE) {
    if (catch || (action && lsdaHasCleanup(&l, action))) {
      _Unwind_SetGR(context, __builtin_eh_return_data_regno(0),
                    (uintptr_t)unwind);
      _Unwind_SetGR(context, __builtin_eh_return_data_regno(1),
                    (uintptr_t)catch);
      _Unwind_SetIP(context, landing);
      return _URC_INSTALL_CONTEXT;
    }
  }
  return _URC_CONTINUE_UNWIND;
}

void *__pas_unwind_object(struct _Unwind_Exception *unwind) {
  return unwind ? ((PasUnwindException *)unwind)->object : NULL;
}

/* One call site (1..16), one catch action for __pas_exception_vmt, and the
 * type table. The action table starts at byte 9; the type table at byte 11
 * is 8 bytes after the byte that follows the TType offset. */
int32_t __pas_eh_selftest(void) {
  unsigned char lsda[11 + sizeof(uintptr_t)];
  uintptr_t vmt = (uintptr_t)__pas_exception_vmt;
  memset(lsda, 0, sizeof lsda);
  lsda[0] = DW_EH_PE_omit;
  lsda[1] = DW_EH_PE_absptr;
  lsda[2] = 8;
  lsda[3] = DW_EH_PE_uleb128;
  lsda[4] = 4;
  lsda[5] = 1;
  lsda[6] = 16;
  lsda[7] = 32;
  lsda[8] = 1;
  lsda[9] = 1;
  lsda[10] = 0;
  memcpy(lsda + 11, &vmt, sizeof vmt);
  PasLSDA l;
  if (!lsdaInit(&l, lsda, lsda + sizeof lsda,
                (uintptr_t)__pas_eh_selftest))
    return 0;
  uintptr_t landing = 0;
  const unsigned char *action = NULL;
  if (!lsdaCallSite(&l, (uintptr_t)__pas_eh_selftest + 8, &landing, &action))
    return 0;
  if (!landing || !action)
    return 0;
  void *object[2] = {(void *)vmt, NULL};
  return lsdaCatch(&l, action, object) == 1 ? 1 : 0;
}

#endif /* __unix__ || __APPLE__ */
