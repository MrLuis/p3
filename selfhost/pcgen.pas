unit pcgen;
{ Code generation: a checked program and its units to textual LLVM IR; the
  port of src/codegen/CodeGen.cpp, whose header describes the lowering:

    integer types -> iN, real -> double, char -> i8, enum -> i32,
    boolean -> i1 in registers and i8 in memory, set -> i256,
    array -> [N x T], record -> a named struct (variants overlay a buffer)
    program variables -> internal globals @pas.<name>
    routines -> internal functions @pas.<outer>.<name> whose locals,
      parameters and result live in a frame struct; nested routines reach
      enclosing frames through static links (frame field 0)

  Values are TV (pcir): the IR type and the text of the value. Wherever
  more than one argument emits instructions, they are evaluated one at a
  time into locals, so the IR does not depend on the order in which the
  compiler compiling this one evaluates arguments.

  Not yet: debug information (-g). }

interface

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcir;

{ Appends the module for prog and its units (dependencies first). }
procedure generateModule(prog: PDecl; const units: TPtrList; m: TLangMode;
                         rangeChecksOn, overflowChecksOn: boolean;
                         const sourceName: string; var outText: TBuf);

implementation

type
  PCounter = ^TCounter;
  TCounter = record
    n: longint;
  end;

  PLargeRange = ^TLargeRange;
  TLargeRange = record
    lo, hi: int64;
    target: PName;
  end;

var
  mode: TLangMode;
  optRange, optOverflow: boolean;
  progDecl: PDecl;
  curRoutine: PDecl;         { nil while emitting main }
  curFrame: TV;
  lowered: TIntMap;          { type id -> PIRType }
  stringConsts: TIntMap;     { name id of the bytes -> PName of the global }
  symCounts: TIntMap;        { symbol name id -> PCounter }
  routinesToEmit: TPtrList;  { of PDecl }
  loopLabels: TPtrList;      { of PName: continue and break targets, in pairs }
  exitLabel: string;
  mainJmpBuf: string;        { '' if the program block has no jmp_buf }
  routinePairT, jmpBufT: PIRType;

{ --- Small helpers --- }

function imin(a, b: int64): int64;
begin
  if a < b then
    imin := a
  else
    imin := b;
end;

function i64c(v: int64): TV;
begin
  i64c := irConstInt(irI64, v);
end;

function i32c(v: int64): TV;
begin
  i32c := irConstInt(irI32, v);
end;

function i8c(v: int64): TV;
begin
  i8c := irConstInt(irI8, v);
end;

function ptrV(const s: string): TV;
begin
  ptrV := irV(irPtrT, s);
end;

function isSignedOrdinal(t: PType): boolean;
begin
  isSignedOrdinal := isSignedIntegerLike(t);
end;

function isBoolBase(t: PType): boolean;
begin
  isBoolBase := getBase(t)^.kind = tyBoolean;
end;

function isCharBase(t: PType): boolean;
begin
  isCharBase := getBase(t)^.kind = tyChar;
end;

function intBits(t: PType): longint;
begin
  intBits := getBase(t)^.bits;
end;

function canon(r: PDecl): PDecl;
begin
  if r^.canonical <> nil then
    canon := r^.canonical
  else
    canon := r;
end;

{ A symbol name not used before: base, else base.1, base.2, ... }
function uniqueSym(const base: string): string;
var
  key: PName;
  c: PCounter;
begin
  key := intern(base);
  c := PCounter(mapGet(symCounts, key^.id));
  if c = nil then
  begin
    c := PCounter(arenaAlloc(sizeof(TCounter)));
    c^.n := 0;
    mapPut(symCounts, key^.id, c);
    exit(base);
  end;
  c^.n := c^.n + 1;
  uniqueSym := base + '.' + intToStr(c^.n);
end;

{ --- Runtime and intrinsic calls --- }

function rtAttrs(const name: string): string;
begin
  if name = '__pas_runtime_error' then
    rtAttrs := 'noreturn cold nounwind'
  else if (name = '__pas_halt') or (name = '_longjmp') then
    rtAttrs := 'noreturn nounwind'
  else if name = '_setjmp' then
    rtAttrs := 'returns_twice nounwind'
  else
    rtAttrs := 'nounwind';
end;

function rtCall(ret: PIRType; const name, params: string; const args: TBuf): TV;
begin
  irDeclare(name, 'declare ' + ret^.text + ' @' + name + '(' + params + ') ' +
            rtAttrs(name));
  rtCall := irCall(ret, '@' + name, args);
end;

function rt0(ret: PIRType; const name: string): TV;
var
  a: TBuf;
begin
  irArgsStart(a);
  rt0 := rtCall(ret, name, '', a);
  bufFree(a);
end;

function rt1(ret: PIRType; const name, params: string; const x: TV): TV;
var
  a: TBuf;
begin
  irArgsStart(a);
  irArgsAdd(a, x);
  rt1 := rtCall(ret, name, params, a);
  bufFree(a);
end;

function rt2(ret: PIRType; const name, params: string; const x, y: TV): TV;
var
  a: TBuf;
begin
  irArgsStart(a);
  irArgsAdd(a, x);
  irArgsAdd(a, y);
  rt2 := rtCall(ret, name, params, a);
  bufFree(a);
end;

function rt3(ret: PIRType; const name, params: string; const x, y, z: TV): TV;
var
  a: TBuf;
begin
  irArgsStart(a);
  irArgsAdd(a, x);
  irArgsAdd(a, y);
  irArgsAdd(a, z);
  rt3 := rtCall(ret, name, params, a);
  bufFree(a);
end;

function rt4(ret: PIRType; const name, params: string; const x, y, z, w: TV): TV;
var
  a: TBuf;
begin
  irArgsStart(a);
  irArgsAdd(a, x);
  irArgsAdd(a, y);
  irArgsAdd(a, z);
  irArgsAdd(a, w);
  rt4 := rtCall(ret, name, params, a);
  bufFree(a);
end;

{ the same for void calls }
procedure rtv0(const name: string);
var
  ignored: TV;
begin
  ignored := rt0(irVoidT, name);
end;

procedure rtv1(const name, params: string; const x: TV);
var
  ignored: TV;
begin
  ignored := rt1(irVoidT, name, params, x);
end;

procedure rtv2(const name, params: string; const x, y: TV);
var
  ignored: TV;
begin
  ignored := rt2(irVoidT, name, params, x, y);
end;

procedure rtv3(const name, params: string; const x, y, z: TV);
var
  ignored: TV;
begin
  ignored := rt3(irVoidT, name, params, x, y, z);
end;

procedure rtv4(const name, params: string; const x, y, z, w: TV);
var
  ignored: TV;
begin
  ignored := rt4(irVoidT, name, params, x, y, z, w);
end;

procedure emitMemCpy(const dst, src, n: TV);
begin
  rtv4('llvm.memcpy.p0.p0.i64', 'ptr, ptr, i64, i1', dst, src, n, irConstBool(false));
end;

procedure emitMemMove(const dst, src, n: TV);
begin
  rtv4('llvm.memmove.p0.p0.i64', 'ptr, ptr, i64, i1', dst, src, n, irConstBool(false));
end;

procedure emitMemSet(const dst, v, n: TV);
begin
  rtv4('llvm.memset.p0.i64', 'ptr, i8, i64, i1', dst, v, n, irConstBool(false));
end;

{ a double -> double intrinsic or C function }
function mathCall(const name: string; const x: TV): TV;
begin
  mathCall := rt1(irDoubleT, name, 'double', x);
end;

{ --- Types --- }

function memType(t: PType): PIRType; forward;

procedure shiftFieldOffsets(fl: PFieldList; delta: int64);
var
  i: longint;
  f: PDecl;
  vp: PVariantPart;
begin
  for i := 0 to fl^.fixed.count - 1 do
  begin
    f := PDecl(listGet(fl^.fixed, i));
    f^.cgOffset := f^.cgOffset + delta;
  end;
  vp := fl^.variant;
  if vp <> nil then
  begin
    if vp^.tag <> nil then
    begin
      f := vp^.tag;
      f^.cgOffset := f^.cgOffset + delta;
    end;
    for i := 0 to vp^.variants.count - 1 do
      shiftFieldOffsets(PVariant(listGet(vp^.variants, i))^.fields, delta);
  end;
end;

{ The struct for a record field list; records each field's byte offset
  from the start of the record. Variants are laid out as their own
  structs, all overlaying one buffer. }
function layoutFieldList(fl: PFieldList; const name: string; baseOffset: int64): PIRType;
var
  elems, fields: TPtrList;
  vp: PVariantPart;
  i: longint;
  maxSize, maxAlign, bufferOffset: int64;
  vt, st: PIRType;
  f: PDecl;
  tagType: PType;
begin
  listInit(elems);
  listInit(fields);
  for i := 0 to fl^.fixed.count - 1 do
  begin
    f := PDecl(listGet(fl^.fixed, i));
    listAdd(elems, memType(f^.fieldType));
    listAdd(fields, f);
  end;
  vp := fl^.variant;
  if (vp <> nil) and (vp^.tag <> nil) then
  begin
    tagType := vp^.tag^.fieldType;
    if tagType = nil then
      tagType := vp^.tagType;
    listAdd(elems, memType(tagType));
    listAdd(fields, vp^.tag);
  end;
  maxSize := 0;
  maxAlign := 1;
  if vp <> nil then
  begin
    for i := 0 to vp^.variants.count - 1 do
    begin
      vt := layoutFieldList(PVariant(listGet(vp^.variants, i))^.fields,
                            name + '.variant', 0);
      if vt^.size > maxSize then
        maxSize := vt^.size;
      if vt^.align > maxAlign then
        maxAlign := vt^.align;
    end;
    listAdd(elems, irArray((maxSize + maxAlign - 1) div maxAlign,
                           irInt(longint(maxAlign * 8))));
  end;
  st := irStructNamed(name, elems);
  for i := 0 to fields.count - 1 do
  begin
    f := PDecl(listGet(fields, i));
    f^.cgOffset := baseOffset + irFieldOffset(st, i);
  end;
  if vp <> nil then
  begin
    bufferOffset := baseOffset + irFieldOffset(st, fields.count);
    for i := 0 to vp^.variants.count - 1 do
      shiftFieldOffsets(PVariant(listGet(vp^.variants, i))^.fields, bufferOffset);
  end;
  listFree(elems);
  listFree(fields);
  layoutFieldList := st;
end;

function lowerType(t: PType): PIRType;
var
  bd: TOrdinalBounds;
  name: string;
begin
  case t^.kind of
    tyInteger: lowerType := irInt(t^.bits);
    tyReal: lowerType := irDoubleT;
    tyBoolean, tyChar: lowerType := irI8;
    tyEnum: lowerType := irI32;
    tySubrange: lowerType := memType(t^.host);
    tyStringLit: lowerType := irArray(t^.strLength, irI8);
    tyArray:
      begin
        bd := getOrdinalBounds(t^.indexType);
        lowerType := irArray(bd.hi - bd.lo + 1, memType(t^.elementType));
      end;
    tyRecord:
      begin
        if t^.declName = nil then
          name := 'record'
        else
          name := 'record.' + nameStr(t^.declName);
        lowerType := layoutFieldList(t^.fieldList, name, 0);
      end;
    tyPointer, tyNil, tyFile: lowerType := irPtrT;
    tySet: lowerType := irI256;
    tyShortString: lowerType := irArray(t^.maxLength + 1, irI8);
    tyRoutine: lowerType := routinePairT;
  else
    lowerType := irI8;
  end;
end;

{ The in-memory representation of a Pascal type. }
function memType(t: PType): PIRType;
var
  l: PIRType;
begin
  l := PIRType(mapGet(lowered, t^.id));
  if l <> nil then
    exit(l);
  l := lowerType(t);
  mapPut(lowered, t^.id, l);
  memType := l;
end;

{ The register representation: booleans are i1. }
function regType(t: PType): PIRType;
begin
  if isBoolBase(t) then
    regType := irI1
  else
    regType := memType(t);
end;

function sizeOf_(t: PType): int64;
begin
  sizeOf_ := memType(t)^.size;
end;

{ --- Scalars in memory --- }

function loadScalar(const p: TV; t: PType): TV;
var
  raw: TV;
begin
  raw := irLoad(memType(t), p);
  if isBoolBase(t) then
    loadScalar := irCast('trunc', raw, irI1)
  else
    loadScalar := raw;
end;

procedure storeScalar(const p, v: TV; t: PType);
var
  x: TV;
begin
  x := v;
  if isBoolBase(t) then
    x := irCast('zext', v, irI8);
  irStore(x, p);
end;

procedure copyAggregate(const dst, src: TV; t: PType);
begin
  emitMemCpy(dst, src, i64c(sizeOf_(t)));
end;

{ The address of a private constant holding bytes (no NUL), shared by
  equal byte strings. }
function getStringConstant(p: PCharArray; start, count: int64): TV;
var
  key, g: PName;
begin
  key := internChars(p, start, count);
  g := PName(mapGet(stringConsts, key^.id));
  if g = nil then
  begin
    g := intern(irStringConstant(p, start, count, false));
    mapPut(stringConsts, key^.id, g);
  end;
  getStringConstant := ptrV(nameStr(g));
end;

function getNameConstant(n: PName): TV;
begin
  getNameConstant := getStringConstant(n^.chars, 0, n^.len);
end;

{ A NUL-terminated copy of the bytes (C strings). }
function globalCString(p: PCharArray; start, count: int64): TV;
begin
  globalCString := ptrV(irStringConstant(p, start, count, true));
end;

{ --- Runtime checks --- }

procedure bufAddWhere(var b: TBuf; loc: TLoc);
var
  fid, line, col: longint;
begin
  if loc = noLoc then
    exit;
  smPresumed(loc, fid, line, col);
  bufAdd(b, ' at ');
  bufAdd(b, smFileName(fid));
  bufAdd(b, ':' + intToStr(line) + ':' + intToStr(col));
end;

{ __pas_runtime_error("<msg> at file:line:col"); unreachable. Callers start
  a new block afterwards. }
procedure emitRuntimeError(const msg: string; loc: TLoc);
var
  b: TBuf;
begin
  bufInit(b);
  bufAdd(b, msg);
  bufAddWhere(b, loc);
  rtv1('__pas_runtime_error', 'ptr', globalCString(b.data, 0, b.len));
  irUnreachable;
  bufFree(b);
end;

{ if cond then __pas_runtime_error("<msg> at file:line:col") }
procedure emitRuntimeCheck(const cond: TV; const msg: string; loc: TLoc);
var
  errL, okL: string;
begin
  if cond.v = 'false' then
    exit; { provably fine }
  errL := irNewLabel('rt.error');
  okL := irNewLabel('rt.ok');
  irCondBr(cond, errL, okL);
  irStartBlock(errL);
  emitRuntimeError(msg, loc);
  irStartBlock(okL);
end;

function rangeChecks(loc: TLoc): boolean;
begin
  rangeChecks := checkEnabled(swRange, loc, optRange);
end;

function overflowChecks(loc: TLoc): boolean;
begin
  overflowChecks := checkEnabled(swOverflow, loc, optOverflow);
end;

{ An ordinal register value of type src as the register type of dst
  (extended by src's signedness, or truncated). No range check. }
function convertOrdinal(const v: TV; src, dst: PType): TV;
var
  d: PIRType;
begin
  d := regType(dst);
  if irSameType(v.t, d) then
    exit(v);
  if d^.bits = 1 then
    exit(irICmp('ne', v, irConstInt(v.t, 0)));
  if d^.bits < v.t^.bits then
    convertOrdinal := irCast('trunc', v, d)
  else if isSignedOrdinal(src) then
    convertOrdinal := irCast('sext', v, d)
  else
    convertOrdinal := irCast('zext', v, d);
end;

{ An ordinal register value widened to i64. }
function toI64(const v: TV; t: PType): TV;
begin
  if v.t^.bits = 64 then
    exit(v);
  if isSignedOrdinal(t) then
    toI64 := irCast('sext', v, irI64)
  else
    toI64 := irCast('zext', v, irI64);
end;

function toDouble(const v: TV; t: PType): TV;
begin
  if t^.kind = tyReal then
    exit(v);
  if isSignedOrdinal(t) then
    toDouble := irCast('sitofp', v, irDoubleT)
  else
    toDouble := irCast('uitofp', v, irDoubleT);
end;

{ x < lo or x > hi, for an i64 x }
function outsideRange(const x: TV; lo, hi: int64): TV;
var
  below, above: TV;
begin
  below := irICmp('slt', x, i64c(lo));
  above := irICmp('sgt', x, i64c(hi));
  outsideRange := irBin('or', below, above);
end;

{ Checks that ordinal value v (of type src) lies within target's bounds. }
procedure emitRangeCheckMsg(const v: TV; src, target: PType; loc: TLoc;
                            const msg: string);
var
  tb, fb: TOrdinalBounds;
begin
  if not rangeChecks(loc) or not isOrdinal(target) then
    exit;
  tb := getOrdinalBounds(target);
  fb := getOrdinalBounds(src);
  if (fb.lo >= tb.lo) and (fb.hi <= tb.hi) then
    exit; { every value of src fits }
  emitRuntimeCheck(outsideRange(toI64(v, src), tb.lo, tb.hi), msg, loc);
end;

procedure emitRangeCheck(const v: TV; src, target: PType; loc: TLoc);
begin
  emitRangeCheckMsg(v, src, target, loc, 'value out of range');
end;

function hexChar(v: longint): char;
begin
  if v < 10 then
    hexChar := chr(ord('0') + v)
  else
    hexChar := chr(ord('A') + v - 10);
end;

{ The i256 with bits lo..hi set (or all others, if invert). }
function setMaskConst(lo, hi: int64; invert: boolean): TV;
var
  s: string;
  k, bit, nib, idx: longint;
begin
  s := 'u0x';
  for k := 63 downto 0 do
  begin
    nib := 0;
    for bit := 3 downto 0 do
    begin
      idx := 4 * k + bit;
      nib := nib * 2;
      if ((idx >= lo) and (idx <= hi)) <> invert then
        nib := nib + 1;
    end;
    s := s + hexChar(nib);
  end;
  setMaskConst := irV(irI256, s);
end;

{ Converts a value for assignment or parameter passing: integer to real,
  and range checks of ordinal values (and set members) against the target. }
function convertForAssign(const v: TV; src, dst: PType; loc: TLoc): TV;
var
  tb, fb: TOrdinalBounds;
  outside: TV;
begin
  if (dst^.kind = tyReal) and (src^.kind <> tyReal) then
    exit(toDouble(v, src));
  if isOrdinal(dst) and isOrdinal(src) then
  begin
    emitRangeCheck(v, src, dst, loc);
    exit(convertOrdinal(v, src, dst));
  end;
  if (dst^.kind = tySet) and (dst^.setElement <> nil) and rangeChecks(loc) then
  begin
    tb := getOrdinalBounds(dst^.setElement);
    if isEmptySet(src) then
      exit(v);
    fb := getOrdinalBounds(getBase(src^.setElement));
    if (fb.lo >= tb.lo) and (fb.hi <= tb.hi) then
      exit(v);
    outside := irBin('and', v, setMaskConst(tb.lo, tb.hi, true));
    emitRuntimeCheck(irICmp('ne', outside, irZero(irI256)),
                     'set member out of range', loc);
  end;
  convertForAssign := v;
end;

{ l op r ('add', 'sub' or 'mul') with an overflow check if enabled. }
function emitCheckedArith(const op: string; signed: boolean; const l, r: TV;
                          loc: TLoc): TV;
var
  name: string;
  fields: TPtrList;
  pairT: PIRType;
  res, ovf: TV;
begin
  if not overflowChecks(loc) then
    exit(irBin(op, l, r));
  if signed then
    name := 'llvm.s' + op + '.with.overflow.' + l.t^.text
  else
    name := 'llvm.u' + op + '.with.overflow.' + l.t^.text;
  listInit(fields);
  listAdd(fields, l.t);
  listAdd(fields, irI1);
  pairT := irStructLit(fields);
  listFree(fields);
  res := rt2(pairT, name, l.t^.text + ', ' + l.t^.text, l, r);
  ovf := irExtract(res, 1, irI1);
  emitRuntimeCheck(ovf, 'integer overflow', loc);
  emitCheckedArith := irExtract(res, 0, l.t);
end;

{ --- Variables and frames --- }

function isByRef(v: PDecl): boolean;
begin
  isByRef := (v^.varKind = vkVarParam) or
             ((v^.varKind = vkConstParam) and isStructured(v^.varType));
end;

function isParamDecl(v: PDecl): boolean;
begin
  isParamDecl := v^.varKind in [vkValueParam, vkVarParam, vkConstParam];
end;

function mangle(r: PDecl): string;
var
  s: string;
  p: PDecl;
begin
  s := nameStr(r^.ident.name);
  p := r^.parent;
  while p <> nil do
  begin
    s := nameStr(p^.ident.name) + '.' + s;
    p := p^.parent;
  end;
  mangle := 'pas.' + s;
end;

function frameType(r: PDecl): PIRType;
begin
  frameType := PIRType(r^.cgFrame);
end;

{ The frame of routine target, reached from the current routine through
  static links. }
function framePtrFor(target: PDecl): TV;
var
  frame, slot: TV;
  r: PDecl;
begin
  frame := curFrame;
  r := curRoutine;
  while (r <> target) and (r <> nil) do
  begin
    slot := irStructGEP(frameType(r), frame, 0);
    frame := irLoad(irPtrT, slot);
    r := r^.parent;
  end;
  framePtrFor := frame;
end;

{ The variable holding the PasFile of input / output. }
function standardFile(const name: string): TV;
begin
  irDeclare('__pas_' + name, '@__pas_' + name + ' = external global ptr');
  standardFile := ptrV('@__pas_' + name);
end;

function loadFile(const varAddr: TV): TV;
begin
  loadFile := irLoad(irPtrT, varAddr);
end;

function outputFile: TV;
begin
  outputFile := loadFile(standardFile('output'));
end;

function inputFile: TV;
begin
  inputFile := loadFile(standardFile('input'));
end;

function createTemp(t: PIRType): TV;
begin
  createTemp := irAlloca(t, 'tmp');
end;

function varAddress(v: PDecl): TV;
var
  owner: PDecl;
  slot: TV;
begin
  owner := v^.owner;
  if owner = nil then
  begin
    if v^.loc = noLoc then
      exit(standardFile(nameStr(v^.ident.name)));
    exit(ptrV(nameStr(v^.cgName)));
  end;
  slot := irStructGEP(frameType(owner), framePtrFor(owner), v^.cgSlot);
  if isByRef(v) then
    exit(irLoad(irPtrT, slot));
  varAddress := slot;
end;

{ The address of field f of record type rt at recAddr. The record is laid
  out first: it may be reached only through pointers so far. }
function fieldAddress(const recAddr: TV; rt: PType; f: PDecl): TV;
var
  ignored: PIRType;
begin
  ignored := memType(rt);
  fieldAddress := irByteGEP(recAddr, f^.cgOffset);
end;

{ Creates (init) or releases the PasFile objects of all files in an object
  of type t at addr: files, arrays of files and file fields of records
  (fixed parts). name: the external file name for a program parameter. }
procedure emitFileLifetime(const addr: TV; t: PType; init: boolean; const name: TV);
var
  size: int64;
  f, i, elem, cond: TV;
  bd: TOrdinalBounds;
  pre, loopL, nextL, doneL, nextName: string;
  j: longint;
  fd: PDecl;
  textFlag: longint;
begin
  if not containsFile(t) then
    exit;
  case t^.kind of
    tyFile:
      begin
        if not init then
        begin
          rtv1('__pas_file_free', 'ptr', loadFile(addr));
          exit;
        end;
        if t^.isText then
        begin
          size := 1;
          textFlag := 1;
        end
        else
        begin
          size := sizeOf_(t^.fileElement);
          textFlag := 0;
        end;
        f := rt3(irPtrT, '__pas_file_new', 'i64, i32, ptr', i64c(size),
                 i32c(textFlag), name);
        irStore(f, addr);
      end;
    tyArray:
      begin
        bd := getOrdinalBounds(t^.indexType);
        pre := irCurrentBlock;
        loopL := irNewLabel('files.loop');
        nextL := irNewLabel('files.next');
        doneL := irNewLabel('files.done');
        irBr(loopL);
        irStartBlock(loopL);
        nextName := irTmp;
        i := irPhi2(irI64, '0', pre, nextName, nextL);
        elem := irArrayGEP(memType(t), addr, i);
        emitFileLifetime(elem, t^.elementType, init, irNull);
        irBr(nextL);
        irStartBlock(nextL);
        irBinNamed(nextName, 'add', i, i64c(1));
        cond := irICmp('eq', irV(irI64, nextName), i64c(bd.hi - bd.lo + 1));
        irCondBr(cond, doneL, loopL);
        irStartBlock(doneL);
      end;
    tyRecord:
      for j := 0 to t^.fieldList^.fixed.count - 1 do
      begin
        fd := PDecl(listGet(t^.fieldList^.fixed, j));
        emitFileLifetime(fieldAddress(addr, t, fd), fd^.fieldType, init, irNull);
      end;
  else
    ;
  end;
end;

{ The argv position of a program parameter (other than input/output) named
  name: 1, 2, ... for distinct names in order; 0 if none. }
function programParamIndex(name: PName): longint;
var
  i, j, n: longint;
  id, prev: PIdent;
  seen: boolean;
begin
  programParamIndex := 0;
  n := 0;
  for i := 0 to progDecl^.progParams.count - 1 do
  begin
    id := PIdent(listGet(progDecl^.progParams, i));
    if nameEquals(id^.name, 'input') or nameEquals(id^.name, 'output') then
      continue;
    seen := false;
    for j := 0 to i - 1 do
    begin
      prev := PIdent(listGet(progDecl^.progParams, j));
      if prev^.name = id^.name then
        seen := true;
    end;
    if seen then
      continue;
    n := n + 1;
    if id^.name = name then
      exit(n);
  end;
end;

{ Creates the PasFile objects of the file variables among decls; program
  parameters are bound to their command-line arguments. }
procedure initFileVars(const decls: TPtrList; isProgram: boolean);
var
  i, idx: longint;
  v: PDecl;
  name: TV;
begin
  for i := 0 to decls.count - 1 do
  begin
    v := PDecl(listGet(decls, i));
    if (v^.kind <> dkVar) or not containsFile(v^.varType) then
      continue;
    name := irNull;
    if isProgram then
    begin
      idx := programParamIndex(v^.ident.name);
      if idx > 0 then
        name := rt2(irPtrT, '__pas_program_param', 'i32, ptr', i32c(idx),
                    globalCString(v^.ident.spelling^.chars, 0, v^.ident.spelling^.len));
    end;
    emitFileLifetime(varAddress(v), v^.varType, true, name);
  end;
end;

{ --- Expressions: forward declarations --- }

function visitExpr(e: PNode): TV; forward;
function emitAddress(e: PNode): TV; forward;
procedure visitStmt(s: PNode); forward;

function emitI32(e: PNode): TV;
begin
  emitI32 := irCast('trunc', toI64(visitExpr(e), e^.typ), irI32);
end;

{ --- Short strings (Turbo/FPC): values are pointers to length-prefixed
  bytes --- }

function getPStrConstant(p: PCharArray; start, count: int64): TV;
var
  b: TBuf;
  n: int64;
begin
  bufInit(b);
  n := imin(count, 255);
  bufAddChar(b, chr(n));
  bufAddChars(b, p, start, n);
  getPStrConstant := getStringConstant(b.data, 0, b.len);
  bufFree(b);
end;

function newStringTemp(maxLen: int64): TV;
begin
  newStringTemp := irAlloca(irArray(maxLen + 1, irI8), 'str');
end;

{ Any string-like expression as a pointer to a length-prefixed string. }
function emitPStr(e: PNode): TV;
var
  t: PType;
  d: PDecl;
  tmp, c, src: TV;
  n: int64;
begin
  t := e^.typ;
  if t^.kind = tyShortString then
    exit(visitExpr(e)); { designators give their address, results a temp }
  if e^.kind = nkStringLit then
    exit(getPStrConstant(e^.strValue^.chars, 0, e^.strValue^.len));
  if e^.kind = nkNameRef then
  begin
    d := e^.refDecl;
    if (d^.kind = dkConst) and (d^.constValue.kind = ckString) then
      exit(getPStrConstant(d^.constValue.strVal^.chars, 0, d^.constValue.strVal^.len));
  end;
  if isCharBase(t) then
  begin
    tmp := newStringTemp(1);
    irStore(i8c(1), tmp);
    c := visitExpr(e);
    irStore(c, irByteGEP(tmp, 1));
    exit(tmp);
  end;
  { packed array [1..n] of char }
  n := imin(getStringLength(t), 255);
  tmp := newStringTemp(n);
  irStore(i8c(n), tmp);
  src := emitAddress(e);
  emitMemCpy(irByteGEP(tmp, 1), src, i64c(n));
  emitPStr := tmp;
end;

procedure emitStringAssign(const dst: TV; t: PType; value: PNode);
var
  src: TV;
begin
  src := emitPStr(value);
  rtv3('__pas_str_assign', 'ptr, i32, ptr', dst, i32c(t^.maxLength), src);
end;

function emitStringFunction(e: PNode; k: TBuiltinKind): TV;
var
  a0, a1, a2: PNode;
  t: PType;
  c, lo, hi, isLower, dec, s0, s1, tmp, n1, n2, ignored: TV;
  i: longint;
begin
  a0 := PNode(listGet(e^.args, 0));
  case k of
    bkLength:
      begin
        t := a0^.typ;
        if t^.kind = tyShortString then
        begin
          s0 := emitPStr(a0);
          exit(irCast('zext', irLoad(irI8, s0), irI32));
        end;
        if isCharBase(t) then
        begin
          ignored := visitExpr(a0);
          exit(i32c(1));
        end;
        exit(i32c(getStringLength(t)));
      end;
    bkUpCase:
      begin
        c := visitExpr(a0);
        lo := irICmp('uge', c, i8c(ord('a')));
        hi := irICmp('ule', c, i8c(ord('z')));
        isLower := irBin('and', lo, hi);
        dec := irBin('sub', c, i8c(32));
        exit(irSelect(isLower, dec, c));
      end;
    bkPos:
      begin
        a1 := PNode(listGet(e^.args, 1));
        s0 := emitPStr(a0);
        s1 := emitPStr(a1);
        exit(rt2(irI32, '__pas_str_pos', 'ptr, ptr', s0, s1));
      end;
    bkCopy:
      begin
        a1 := PNode(listGet(e^.args, 1));
        a2 := PNode(listGet(e^.args, 2));
        tmp := newStringTemp(255);
        s0 := emitPStr(a0);
        n1 := emitI32(a1);
        n2 := emitI32(a2);
        rtv4('__pas_str_copy', 'ptr, ptr, i32, i32', tmp, s0, n1, n2);
        exit(tmp);
      end;
  else
    begin
      { concat }
      tmp := newStringTemp(255);
      s0 := emitPStr(a0);
      rtv3('__pas_str_assign', 'ptr, i32, ptr', tmp, i32c(255), s0);
      for i := 1 to e^.args.count - 1 do
      begin
        s1 := emitPStr(PNode(listGet(e^.args, i)));
        rtv3('__pas_str_concat', 'ptr, ptr, ptr', tmp, tmp, s1);
      end;
      emitStringFunction := tmp;
    end;
  end;
end;

{ --- Sets --- }

{ An ordinal value as a bit index (i256), checked to be in 0..255. }
function setIndex(const v: TV; t: PType; loc: TLoc): TV;
var
  x, masked: TV;
  bd: TOrdinalBounds;
begin
  x := toI64(v, t);
  bd := getOrdinalBounds(t);
  if rangeChecks(loc) and ((bd.lo < 0) or (bd.hi > 255)) then
    emitRuntimeCheck(irICmp('ugt', x, i64c(255)), 'set element out of range 0..255', loc);
  masked := irBin('and', x, i64c(255));
  setIndex := irCast('zext', masked, irI256);
end;

function emitSetCtor(e: PNode): TV;
var
  res, one, lo, hi, empty, loI, hiI, isTop, shifted, below, hiMask, loBits,
  loMask, both, range_: TV;
  i: longint;
  el: PSetElement;
  lt, ht: PType;
begin
  res := irZero(irI256);
  one := irConstInt(irI256, 1);
  for i := 0 to e^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(e^.setElems, i));
    lt := el^.lo^.typ;
    lo := visitExpr(el^.lo);
    if el^.hi = nil then
    begin
      loI := setIndex(lo, lt, el^.lo^.b);
      shifted := irBin('shl', one, loI);
      res := irBin('or', res, shifted);
      continue;
    end;
    hi := visitExpr(el^.hi);
    ht := el^.hi^.typ;
    { bits lo..hi, or none if lo > hi }
    if isSignedOrdinal(lt) then
      empty := irICmp('sgt', lo, hi)
    else
      empty := irICmp('ugt', lo, hi);
    loI := setIndex(lo, lt, el^.lo^.b);
    hiI := setIndex(hi, ht, el^.hi^.b);
    isTop := irICmp('eq', hiI, irConstInt(irI256, 255));
    shifted := irBin('add', hiI, one);
    shifted := irBin('shl', one, shifted);
    below := irBin('sub', shifted, one);
    hiMask := irSelect(isTop, irConstInt(irI256, -1), below);
    loBits := irBin('shl', one, loI);
    loBits := irBin('sub', loBits, one);
    loMask := irBin('xor', loBits, irConstInt(irI256, -1));
    both := irBin('and', hiMask, loMask);
    range_ := irSelect(empty, irZero(irI256), both);
    res := irBin('or', res, range_);
  end;
  emitSetCtor := res;
end;

{ x in s:  0 <= x <= 255 and bit x of s is set }
function emitIn(e: PNode): TV;
var
  x, s, inRange, idx, shifted, bit: TV;
begin
  x := toI64(visitExpr(e^.lhs), e^.lhs^.typ);
  s := visitExpr(e^.rhs);
  inRange := irICmp('ule', x, i64c(255));
  idx := irBin('and', x, i64c(255));
  idx := irCast('zext', idx, irI256);
  shifted := irBin('lshr', s, idx);
  bit := irCast('trunc', shifted, irI1);
  emitIn := irBin('and', inRange, bit);
end;

{ --- Addresses --- }

function emitAddress(e: PNode): TV;
var
  d: PDecl;
  base, idx, x, bad, offset: TV;
  bt, idxTy: PType;
  bd: TOrdinalBounds;
  w: PWithContext;
begin
  case e^.kind of
    nkNameRef:
      begin
        d := e^.refDecl;
        case d^.kind of
          dkVar: emitAddress := varAddress(d);
          dkRoutine: emitAddress := visitExpr(e); { a record or string result }
          dkField:
            begin
              w := e^.refWith;
              emitAddress := fieldAddress(ptrV(nameStr(w^.cgAddr)), w^.rec^.typ, d);
            end;
        else
          emitAddress := getNameConstant(d^.constValue.strVal);
        end;
      end;
    nkStringLit: emitAddress := getNameConstant(e^.strValue);
    nkParen: emitAddress := emitAddress(e^.inner);
    nkCall, nkIndirectCall:
      { a record or string function result: a temporary holding it }
      emitAddress := visitExpr(e);
    nkIndex:
      begin
        bt := e^.base^.typ;
        idxTy := e^.index^.typ;
        if bt^.kind = tyShortString then
        begin
          base := emitAddress(e^.base);
          idx := visitExpr(e^.index);
          x := toI64(idx, idxTy);
          if rangeChecks(e^.index^.b) then
          begin
            bad := outsideRange(x, 0, bt^.maxLength);
            emitRuntimeCheck(bad, 'string index out of bounds', e^.index^.b);
          end;
          exit(irByteGEPv(base, x));
        end;
        if bt^.kind = tyStringLit then
        begin
          { a string constant (Turbo/FPC): its characters are 1..N }
          base := emitAddress(e^.base);
          x := toI64(visitExpr(e^.index), idxTy);
          if rangeChecks(e^.index^.b) then
          begin
            bad := outsideRange(x, 1, bt^.strLength);
            emitRuntimeCheck(bad, 'string index out of bounds', e^.index^.b);
          end;
          offset := irBin('sub', x, i64c(1));
          exit(irByteGEPv(base, offset));
        end;
        base := emitAddress(e^.base);
        idx := visitExpr(e^.index);
        emitRangeCheckMsg(idx, idxTy, bt^.indexType, e^.index^.b,
                          'array index out of bounds');
        bd := getOrdinalBounds(bt^.indexType);
        offset := irBin('sub', toI64(idx, idxTy), i64c(bd.lo));
        emitAddress := irArrayGEP(memType(bt), base, offset);
      end;
    nkField:
      begin
        base := emitAddress(e^.base);
        emitAddress := fieldAddress(base, e^.base^.typ, e^.fieldDecl);
      end;
    nkDeref:
      begin
        if e^.inner^.typ^.kind = tyFile then { the buffer variable f^ }
          exit(rt1(irPtrT, '__pas_buffer', 'ptr', loadFile(emitAddress(e^.inner))));
        base := visitExpr(e^.inner);
        if rangeChecks(e^.inner^.b) then
          emitRuntimeCheck(irICmp('eq', base, irNull), 'dereference of a nil pointer',
                           e^.inner^.b);
        emitAddress := base;
      end;
  else
    emitAddress := irNull; { Sema allows no other designators }
  end;
end;

{ --- Calls --- }

function retTypeOf(result_: PType): PIRType;
begin
  if result_ = nil then
    retTypeOf := irVoidT
  else
    retTypeOf := regType(result_);
end;

function staticLinkFor(r: PDecl): TV;
begin
  if r^.parent <> nil then
    staticLinkFor := framePtrFor(r^.parent)
  else
    staticLinkFor := irNull;
end;

function routinePairOf(r: PDecl): TV;
var
  pair, link: TV;
begin
  r := canon(r);
  pair := irInsert(irUndef(routinePairT), ptrV(nameStr(r^.cgName)), 0);
  link := staticLinkFor(r);
  routinePairOf := irInsert(pair, link, 1);
end;

{ The routine value passed for a procedural parameter. }
function emitRoutineValue(arg: PNode): TV;
var
  d: PDecl;
begin
  d := arg^.refDecl;
  if d^.kind = dkVar then { forwarding a parameter }
    exit(irLoad(memType(d^.varType), varAddress(d)));
  emitRoutineValue := routinePairOf(d);
end;

{ A string constant passed for a pchar parameter: a NUL-terminated copy. }
function cStringFor(paramTy: PType; arg: PNode; var v: TV): boolean;
var
  d: PDecl;
  c: char;
begin
  cStringFor := false;
  if (paramTy^.kind <> tyPointer) or paramTy^.untyped or (paramTy^.pointee = nil) or
     (paramTy^.pointee^.kind <> tyChar) then
    exit;
  if arg^.kind = nkStringLit then
  begin
    v := globalCString(arg^.strValue^.chars, 0, arg^.strValue^.len);
    exit(true);
  end;
  if arg^.kind <> nkNameRef then
    exit;
  d := arg^.refDecl;
  if (d = nil) or (d^.kind <> dkConst) then
    exit;
  if d^.constValue.kind = ckString then
  begin
    v := globalCString(d^.constValue.strVal^.chars, 0, d^.constValue.strVal^.len);
    exit(true);
  end;
  if (d^.constValue.kind = ckOrdinal) and (getBase(d^.constType)^.kind = tyChar) then
  begin
    c := chr(d^.constValue.intVal and 255);
    v := ptrV(irStringConstant(PCharArray(@c), 0, 1, true));
    exit(true);
  end;
end;

function emitCallTo(ret: PIRType; const callee: string; const link: TV;
                    hasLink: boolean; const params, args: TPtrList): TV;
var
  buf: TBuf;
  i: longint;
  p: PDecl;
  arg: PNode;
  v, tmp, res: TV;
  pt: PType;
begin
  irArgsStart(buf);
  if hasLink then { external (C) routines have none }
    irArgsAdd(buf, link);
  for i := 0 to args.count - 1 do
  begin
    p := PDecl(listGet(params, i));
    arg := PNode(listGet(args, i));
    pt := p^.varType;
    if cStringFor(pt, arg, v) then
      irArgsAdd(buf, v)
    else if pt^.kind = tyRoutine then
      irArgsAdd(buf, emitRoutineValue(arg))
    else if (p^.varKind = vkConstParam) and (pt^.kind = tyShortString) then
      irArgsAdd(buf, emitPStr(arg)) { read-only: no copy }
    else if (pt^.kind = tyShortString) and (p^.varKind <> vkVarParam) then
    begin
      { the callee copies maxLength+1 bytes: pass a full-size temporary }
      tmp := newStringTemp(pt^.maxLength);
      emitStringAssign(tmp, pt, arg);
      irArgsAdd(buf, tmp);
    end
    else if (p^.varKind = vkVarParam) or isStructured(pt) then
      irArgsAdd(buf, emitAddress(arg))
    else
      irArgsAdd(buf, convertForAssign(visitExpr(arg), arg^.typ, pt, arg^.b));
  end;
  res := irCall(ret, callee, buf);
  bufFree(buf);
  { a string or record result comes back by value; expressions use its
    address }
  if ret^.kind in [ikArray, ikStruct] then
  begin
    tmp := createTemp(ret);
    irStore(res, tmp);
    exit(tmp);
  end;
  emitCallTo := res;
end;

function emitCall(r: PDecl; const args: TPtrList): TV;
var
  rt: PIRType;
begin
  r := canon(r);
  if r^.isFunction then
    rt := regType(r^.resultType)
  else
    rt := irVoidT;
  if r^.isExternal then
    exit(emitCallTo(rt, nameStr(r^.cgName), irNull, false, r^.params, args));
  emitCall := emitCallTo(rt, nameStr(r^.cgName), staticLinkFor(r), true, r^.params, args);
end;

{ Calls the routine value pair of type rt; nilLoc: where to report a nil
  procedural value (noLoc: no check). }
function emitCallThrough(const pair: TV; rt: PType; const args: TPtrList; nilLoc: TLoc): TV;
var
  fn, link: TV;
begin
  fn := irExtract(pair, 0, irPtrT);
  if (nilLoc <> noLoc) and rangeChecks(nilLoc) then
    emitRuntimeCheck(irICmp('eq', fn, irNull), 'call through a nil procedural value',
                     nilLoc);
  link := irExtract(pair, 1, irPtrT);
  emitCallThrough := emitCallTo(retTypeOf(rt^.routineResult), fn.v, link, true,
                                rt^.routineParams, args);
end;

{ A call through a procedural parameter or variable. }
function emitIndirectCall(v: PDecl; const args: TPtrList): TV;
var
  pair: TV;
  loc: TLoc;
begin
  pair := irLoad(memType(v^.varType), varAddress(v));
  { parameters always hold a routine; variables may be nil }
  loc := v^.loc;
  if isParamDecl(v) then
    loc := noLoc;
  emitIndirectCall := emitCallThrough(pair, v^.varType, args, loc);
end;

{ A procedural value of type t (Turbo/FPC): @routine, nil, or a procedural
  variable, field or element. }
function emitRoutinePair(e: PNode; t: PType): TV;
begin
  if e^.kind = nkNil then
    exit(irZero(memType(t)));
  if e^.kind = nkAddrOf then
    exit(routinePairOf(e^.inner^.refDecl));
  emitRoutinePair := irLoad(memType(t), emitAddress(e));
end;

{ --- Operators --- }

function emitShortCircuit(e: PNode): TV;
var
  isAnd: boolean;
  l, r: TV;
  lhsEnd, rhsEnd, rhsL, endL, shortValue: string;
begin
  isAnd := e^.bop = boAnd;
  l := visitExpr(e^.lhs);
  lhsEnd := irCurrentBlock;
  if isAnd then
  begin
    rhsL := irNewLabel('and.rhs');
    endL := irNewLabel('and.end');
    irCondBr(l, rhsL, endL);
    shortValue := 'false';
  end
  else
  begin
    rhsL := irNewLabel('or.rhs');
    endL := irNewLabel('or.end');
    irCondBr(l, endL, rhsL);
    shortValue := 'true';
  end;
  irStartBlock(rhsL);
  r := visitExpr(e^.rhs);
  rhsEnd := irCurrentBlock;
  irBr(endL);
  irStartBlock(endL);
  emitShortCircuit := irPhi2(irI1, shortValue, lhsEnd, r.v, rhsEnd);
end;

function signedMin(t: PIRType): TV;
begin
  if t^.bits = 64 then
    signedMin := irV(t, '-9223372036854775808')
  else
    signedMin := irConstInt(t, int64(1) shl (t^.bits - 1));
end;

function emitDiv(const l, r: TV; signed: boolean; loc: TLoc): TV;
var
  c: int64;
  isConst: boolean;
  a, b: TV;
begin
  isConst := irIsConstInt(r, c);
  if not isConst or (c = 0) then
    emitRuntimeCheck(irICmp('eq', r, irConstInt(r.t, 0)), 'division by zero', loc);
  if not signed then
    exit(irBin('udiv', l, r));
  { the minimum div -1 overflows (and traps on x86) }
  if not isConst or (c = -1) then
  begin
    a := irICmp('eq', l, signedMin(l.t));
    b := irICmp('eq', r, irConstInt(r.t, -1));
    emitRuntimeCheck(irBin('and', a, b), 'integer overflow in ''div''', loc);
  end;
  emitDiv := irBin('sdiv', l, r);
end;

function emitMod(const l, r: TV; signed: boolean; loc: TLoc): TV;
var
  c: int64;
  isConst: boolean;
  rem, fixed, neg, isMinus, safeR: TV;
begin
  isConst := irIsConstInt(r, c);
  if mode = lmISO then
  begin
    { ISO 7185 6.7.2.2: an error if j <= 0; the result is i - k*j with
      0 <= result < j }
    if not isConst or (c <= 0) then
      emitRuntimeCheck(irICmp('sle', r, irConstInt(r.t, 0)),
                       '''mod'' by zero or a negative number', loc);
    rem := irBin('srem', l, r);
    fixed := irBin('add', rem, r);
    neg := irICmp('slt', rem, irConstInt(rem.t, 0));
    exit(irSelect(neg, fixed, rem));
  end;
  { Turbo/FPC: the sign follows the dividend (like C %) }
  if not isConst or (c = 0) then
    emitRuntimeCheck(irICmp('eq', r, irConstInt(r.t, 0)), 'division by zero', loc);
  if not signed then
    exit(irBin('urem', l, r));
  { x mod -1 = 0, but the minimum srem -1 traps on x86: divide by 1 }
  isMinus := irICmp('eq', r, irConstInt(r.t, -1));
  safeR := irSelect(isMinus, irConstInt(r.t, 1), r);
  emitMod := irBin('srem', l, safeR);
end;

function compareInts(op: TBinaryOp; const l, r: TV; signed: boolean): TV;
var
  p: string;
begin
  case op of
    boEq: p := 'eq';
    boNe: p := 'ne';
    boLt: if signed then p := 'slt' else p := 'ult';
    boLe: if signed then p := 'sle' else p := 'ule';
    boGt: if signed then p := 'sgt' else p := 'ugt';
  else
    if signed then p := 'sge' else p := 'uge';
  end;
  compareInts := irICmp(p, l, r);
end;

function emitCompare(e: PNode): TV;
var
  op: TBinaryOp;
  lt, rt: PType;
  l, r, cmp, notR, notL: TV;
  p: string;
begin
  op := e^.bop;
  lt := e^.lhs^.typ;
  rt := e^.rhs^.typ;
  if op = boIn then
    exit(emitIn(e));
  if lt^.kind = tySet then
  begin
    l := visitExpr(e^.lhs);
    r := visitExpr(e^.rhs);
    case op of
      boEq: exit(irICmp('eq', l, r));
      boNe: exit(irICmp('ne', l, r));
      boLe:
        begin
          { l is a subset of r }
          notR := irBin('xor', r, irConstInt(irI256, -1));
          exit(irICmp('eq', irBin('and', l, notR), irZero(irI256)));
        end;
    else
      begin
        { l is a superset of r }
        notL := irBin('xor', l, irConstInt(irI256, -1));
        exit(irICmp('eq', irBin('and', r, notL), irZero(irI256)));
      end;
    end;
  end;
  if needsStringCompare(lt, rt) then
  begin
    { Turbo/FPC string comparison }
    l := emitPStr(e^.lhs);
    r := emitPStr(e^.rhs);
    cmp := rt2(irI32, '__pas_str_compare', 'ptr, ptr', l, r);
    exit(compareInts(op, cmp, i32c(0), true));
  end;
  if (lt^.kind = tyPointer) or (lt^.kind = tyNil) then
  begin
    l := visitExpr(e^.lhs);
    r := visitExpr(e^.rhs);
    if op = boEq then
      exit(irICmp('eq', l, r));
    exit(irICmp('ne', l, r));
  end;
  if isStringType(lt) then
  begin
    { equal-length strings compare lexicographically (unsigned chars) }
    l := emitAddress(e^.lhs);
    r := emitAddress(e^.rhs);
    cmp := rt3(irI32, 'memcmp', 'ptr, ptr, i64', l, r, i64c(getStringLength(lt)));
    exit(compareInts(op, cmp, i32c(0), true));
  end;
  if (lt^.kind = tyReal) or (rt^.kind = tyReal) then
  begin
    l := toDouble(visitExpr(e^.lhs), lt);
    r := toDouble(visitExpr(e^.rhs), rt);
    case op of
      boEq: p := 'oeq';
      boNe: p := 'une';
      boLt: p := 'olt';
      boLe: p := 'ole';
      boGt: p := 'ogt';
    else
      p := 'oge';
    end;
    exit(irFCmp(p, l, r));
  end;
  l := visitExpr(e^.lhs);
  r := visitExpr(e^.rhs);
  if e^.operandType <> nil then
  begin
    { integers of possibly different types }
    l := convertOrdinal(l, lt, e^.operandType);
    r := convertOrdinal(r, rt, e^.operandType);
    exit(compareInts(op, l, r, isSignedOrdinal(e^.operandType)));
  end;
  emitCompare := compareInts(op, l, r, isSignedOrdinal(lt));
end;

{ --- Built-in functions --- }

function emitFileQuery(k: TBuiltinKind; fileArg: PNode): TV;
var
  f, r: TV;
begin
  if fileArg <> nil then
    f := loadFile(emitAddress(fileArg))
  else
    f := inputFile;
  if k = bkEof then
    r := rt1(irI32, '__pas_eof', 'ptr', f)
  else
    r := rt1(irI32, '__pas_eoln', 'ptr', f);
  emitFileQuery := irICmp('ne', r, i32c(0));
end;

{ T(x): ordinal conversions keep the low bits (extending by the source);
  pointer casts change only the type. }
function emitTypecast(e: PNode): TV;
var
  arg: PNode;
  v: TV;
begin
  arg := PNode(listGet(e^.args, 0));
  v := visitExpr(arg);
  if isOrdinal(e^.typ) then
    exit(convertOrdinal(v, arg^.typ, e^.typ));
  emitTypecast := v;
end;

function emitRoundTrunc(e: PNode; const arg: TV; at: PType; isRound: boolean): TV;
var
  x, low, high, bad: TV;
  resT: PIRType;
  is64: boolean;
  lo, hi: double;
  msg: TBuf;
  b: TBuf;
  errL, okL: string;
begin
  if at^.kind <> tyReal then
    exit(convertOrdinal(arg, at, e^.typ));
  { ISO: round() rounds halves away from zero, like llvm.round; Turbo/FPC
    round halves to even }
  x := arg;
  if isRound then
  begin
    if mode = lmISO then
      x := mathCall('llvm.round.f64', arg)
    else
      x := mathCall('llvm.roundeven.f64', arg);
  end;
  resT := regType(e^.typ);
  if rangeChecks(e^.b) then
  begin
    { the result must lie in the integer (or int64) range; the bounds are
      exact doubles. NaN fails too (unordered comparisons are true). }
    is64 := resT^.bits = 64;
    if is64 then
    begin
      lo := -9223372036854775808.0;
      hi := 9223372036854775808.0;
      low := irFCmp('ult', x, irConstDouble(lo));
    end
    else
    begin
      lo := -2147483649.0;
      hi := 2147483648.0;
      low := irFCmp('ule', x, irConstDouble(lo));
    end;
    high := irFCmp('uge', x, irConstDouble(hi));
    bad := irBin('or', low, high);
    bufInit(msg);
    if isRound then
      bufAdd(msg, '''round''')
    else
      bufAdd(msg, '''trunc''');
    bufAdd(msg, ' result does not fit in ');
    bufAddTypeName(msg, e^.typ);
    { emitRuntimeCheck with a message of any length }
    errL := irNewLabel('rt.error');
    okL := irNewLabel('rt.ok');
    irCondBr(bad, errL, okL);
    irStartBlock(errL);
    bufInit(b);
    bufAddChars(b, msg.data, 0, msg.len);
    bufAddWhere(b, e^.b);
    rtv1('__pas_runtime_error', 'ptr', globalCString(b.data, 0, b.len));
    irUnreachable;
    bufFree(b);
    bufFree(msg);
    irStartBlock(okL);
  end;
  emitRoundTrunc := irCast('fptosi', x, resT);
end;

function emitBuiltinCall(e: PNode; k: TBuiltinKind): TV;
var
  argE: PNode;
  at, t: PType;
  arg, tmp, p, x, one, bad: TV;
  v: int64;
  bd: TOrdinalBounds;
  loc: TLoc;
  isSucc: boolean;
  r: PIRType;
begin
  case k of
    bkSizeOf: { the argument is not evaluated }
      exit(i64c(sizeOf_(PNode(listGet(e^.args, 0))^.typ)));
    bkLow, bkHigh:
      begin
        t := PNode(listGet(e^.args, 0))^.typ;
        if t^.kind = tyShortString then
        begin
          if k = bkHigh then
            v := t^.maxLength
          else
            v := 0;
        end
        else
        begin
          if t^.kind = tyArray then
            t := t^.indexType;
          bd := getOrdinalBounds(t);
          if k = bkHigh then
            v := bd.hi
          else
            v := bd.lo;
        end;
        exit(irConstInt(regType(e^.typ), v));
      end;
    bkAssigned:
      begin
        argE := PNode(listGet(e^.args, 0));
        if argE^.typ^.kind = tyRoutine then
        begin
          tmp := irLoad(memType(argE^.typ), emitAddress(argE));
          p := irExtract(tmp, 0, irPtrT);
        end
        else
          p := visitExpr(argE);
        exit(irICmp('ne', p, irNull));
      end;
    bkParamCount:
      exit(rt0(irI32, '__pas_paramcount'));
    bkParamStr:
      begin
        tmp := newStringTemp(255);
        x := emitI32(PNode(listGet(e^.args, 0)));
        rtv2('__pas_paramstr', 'ptr, i32', tmp, x);
        exit(tmp);
      end;
    bkLength, bkCopy, bkPos, bkConcat, bkUpCase:
      exit(emitStringFunction(e, k));
    bkEof, bkEoln:
      exit(emitFileQuery(k, PNode(listGet(e^.args, 0))));
  else
    ;
  end;

  argE := PNode(listGet(e^.args, 0));
  at := argE^.typ;
  arg := visitExpr(argE);
  loc := e^.b;
  case k of
    bkAbs:
      begin
        if at^.kind = tyReal then
          exit(mathCall('llvm.fabs.f64', arg));
        arg := convertOrdinal(arg, at, e^.typ);
        if not isSignedOrdinal(e^.typ) then
          exit(arg);
        if overflowChecks(loc) then
          emitRuntimeCheck(irICmp('eq', arg, signedMin(arg.t)),
                           'integer overflow in ''abs''', loc);
        r := arg.t;
        exit(rt2(r, 'llvm.abs.' + r^.text, r^.text + ', i1', arg, irConstBool(false)));
      end;
    bkSqr:
      begin
        if at^.kind = tyReal then
          exit(irBin('fmul', arg, arg));
        arg := convertOrdinal(arg, at, e^.typ);
        exit(emitCheckedArith('mul', isSignedOrdinal(e^.typ), arg, arg, loc));
      end;
    bkOdd:
      exit(irCast('trunc', arg, irI1));
    bkOrd:
      exit(convertOrdinal(arg, at, e^.typ));
    bkChr:
      begin
        emitRangeCheckMsg(arg, at, e^.typ, loc, '''chr'' argument out of range');
        exit(irCast('trunc', arg, irI8));
      end;
    bkSucc, bkPred:
      begin
        isSucc := k = bkSucc;
        t := getBase(at);
        bd := getOrdinalBounds(t);
        if rangeChecks(loc) then
        begin
          x := toI64(arg, t);
          if isSucc then
          begin
            bad := irICmp('eq', x, i64c(bd.hi));
            emitRuntimeCheck(bad, '''succ'' of the last value of its type', loc);
          end
          else
          begin
            bad := irICmp('eq', x, i64c(bd.lo));
            emitRuntimeCheck(bad, '''pred'' of the first value of its type', loc);
          end;
        end;
        one := irConstInt(arg.t, 1);
        if isSucc then
          exit(irBin('add', arg, one));
        exit(irBin('sub', arg, one));
      end;
    bkSin: exit(mathCall('llvm.sin.f64', toDouble(arg, at)));
    bkCos: exit(mathCall('llvm.cos.f64', toDouble(arg, at)));
    bkExp: exit(mathCall('llvm.exp.f64', toDouble(arg, at)));
    bkArctan: exit(mathCall('atan', toDouble(arg, at)));
    bkLn:
      begin
        x := toDouble(arg, at);
        if rangeChecks(loc) then
          emitRuntimeCheck(irFCmp('ule', x, irConstDouble(0.0)),
                           '''ln'' of zero or a negative number', loc);
        exit(mathCall('llvm.log.f64', x));
      end;
    bkSqrt:
      begin
        x := toDouble(arg, at);
        if rangeChecks(loc) then
          emitRuntimeCheck(irFCmp('ult', x, irConstDouble(0.0)),
                           '''sqrt'' of a negative number', loc);
        exit(mathCall('llvm.sqrt.f64', x));
      end;
    bkTrunc, bkRound:
      exit(emitRoundTrunc(e, arg, at, k = bkRound));
  else
    emitBuiltinCall := arg;
  end;
end;

{ --- Expressions (scalar values in register representation) --- }

function visitNameRef(e: PNode): TV;
var
  d: PDecl;
  noArgs: TPtrList;
begin
  d := e^.refDecl;
  if (e^.typ^.kind = tyShortString) and (d^.kind in [dkVar, dkField]) then
    exit(emitAddress(e));
  if (d^.kind = dkVar) and (d^.varType <> nil) and (d^.varType^.kind = tyRoutine) then
  begin
    if e^.typ^.kind = tyRoutine then { the value itself (Turbo/FPC) }
      exit(irLoad(memType(d^.varType), varAddress(d)));
    listInit(noArgs);
    exit(emitIndirectCall(d, noArgs)); { a parameterless functional parameter }
  end;
  if d^.kind in [dkVar, dkField] then
    exit(loadScalar(emitAddress(e), e^.typ));
  if d^.kind = dkRoutine then
  begin
    listInit(noArgs);
    exit(emitCall(d, noArgs)); { a parameterless function }
  end;
  if d^.kind = dkBuiltin then
  begin
    if d^.builtin = bkParamCount then
      exit(rt0(irI32, '__pas_paramcount'));
    exit(emitFileQuery(d^.builtin, nil)); { eof / eoln }
  end;
  if d^.constValue.kind = ckReal then
    exit(irConstDouble(d^.constValue.realVal));
  visitNameRef := irConstInt(regType(d^.constType), d^.constValue.intVal);
end;

function visitUnary(e: PNode): TV;
var
  v, r: TV;
  t: PType;
begin
  v := visitExpr(e^.operand);
  t := e^.typ;
  if isIntegerLike(t) then { a promoted operand (byte -> integer, ...) }
    v := convertOrdinal(v, e^.operand^.typ, t);
  case e^.uop of
    uoPlus: visitUnary := v;
    uoMinus:
      if t^.kind = tyReal then
      begin
        r := irV(irDoubleT, irTmp);
        irEmit(r.v + ' = fneg double ' + v.v);
        visitUnary := r;
      end
      else
        visitUnary := emitCheckedArith('sub', isSignedOrdinal(t), irConstInt(v.t, 0),
                                       v, e^.b);
  else
    visitUnary := irBin('xor', v, irConstInt(v.t, -1));
  end;
end;

function visitBinary(e: PNode): TV;
var
  op: TBinaryOp;
  lt, rt, opTy: PType;
  l, r, tmp, notR, masked: TV;
  signed: boolean;
  loc: TLoc;
begin
  op := e^.bop;
  if (op in [boAnd, boOr]) and (e^.typ^.kind = tyBoolean) then
    exit(emitShortCircuit(e));
  if e^.typ^.kind = tyShortString then
  begin
    { Turbo/FPC concatenation }
    tmp := newStringTemp(255);
    l := emitPStr(e^.lhs);
    r := emitPStr(e^.rhs);
    rtv3('__pas_str_concat', 'ptr, ptr, ptr', tmp, l, r);
    exit(tmp);
  end;
  if isRelational(op) then
    exit(emitCompare(e));

  lt := e^.lhs^.typ;
  rt := e^.rhs^.typ;
  l := visitExpr(e^.lhs);
  r := visitExpr(e^.rhs);

  if e^.typ^.kind = tySet then
  begin
    case op of
      boAdd: exit(irBin('or', l, r));
      boMul: exit(irBin('and', l, r));
    else
      begin
        notR := irBin('xor', r, irConstInt(irI256, -1));
        exit(irBin('and', l, notR));
      end;
    end;
  end;

  if e^.typ^.kind = tyReal then
  begin
    l := toDouble(l, lt);
    r := toDouble(r, rt);
    case op of
      boAdd: exit(irBin('fadd', l, r));
      boSub: exit(irBin('fsub', l, r));
      boMul: exit(irBin('fmul', l, r));
    else
      begin
        emitRuntimeCheck(irFCmp('oeq', r, irConstDouble(0.0)), 'division by zero',
                         e^.opLoc);
        exit(irBin('fdiv', l, r));
      end;
    end;
  end;

  loc := e^.opLoc;
  { integer operators: both operands in the operand type chosen by Sema }
  signed := true;
  opTy := e^.operandType;
  if opTy <> nil then
  begin
    l := convertOrdinal(l, lt, opTy);
    r := convertOrdinal(r, rt, opTy);
    signed := isSignedOrdinal(opTy);
  end;
  case op of
    { Turbo/FPC bitwise operators (booleans: xor only) }
    boAnd: visitBinary := irBin('and', l, r);
    boOr: visitBinary := irBin('or', l, r);
    boXor: visitBinary := irBin('xor', l, r);
    boShl:
      begin
        masked := irBin('and', r, irConstInt(r.t, l.t^.bits - 1));
        visitBinary := irBin('shl', l, masked);
      end;
    boShr:
      begin
        { logical, like Turbo/FPC }
        masked := irBin('and', r, irConstInt(r.t, l.t^.bits - 1));
        visitBinary := irBin('lshr', l, masked);
      end;
    boAdd: visitBinary := emitCheckedArith('add', signed, l, r, loc);
    boSub: visitBinary := emitCheckedArith('sub', signed, l, r, loc);
    boMul: visitBinary := emitCheckedArith('mul', signed, l, r, loc);
    boIntDiv: visitBinary := emitDiv(l, r, signed, loc);
    boMod: visitBinary := emitMod(l, r, signed, loc);
  else
    visitBinary := l;
  end;
end;

function visitCall(e: PNode): TV;
var
  d: PDecl;
begin
  d := e^.callDecl;
  case d^.kind of
    dkType: visitCall := emitTypecast(e);
    dkRoutine: visitCall := emitCall(d, e^.args);
    dkVar: visitCall := emitIndirectCall(d, e^.args);
  else
    visitCall := emitBuiltinCall(e, d^.builtin);
  end;
end;

function visitIndirectCall(e: PNode): TV;
var
  callee: PNode;
  pair: TV;
begin
  callee := e^.calleeExpr;
  pair := irLoad(memType(callee^.typ), emitAddress(callee));
  visitIndirectCall := emitCallThrough(pair, callee^.typ, e^.args, callee^.b);
end;

function visitExpr(e: PNode): TV;
begin
  case e^.kind of
    nkIntegerLit: visitExpr := irConstInt(regType(e^.typ), e^.intValue);
    nkRealLit: visitExpr := irConstDouble(e^.realValue);
    nkStringLit:
      { a character; longer strings are handled by address }
      visitExpr := i8c(ord(e^.strValue^.chars^[0]));
    nkNameRef: visitExpr := visitNameRef(e);
    nkParen: visitExpr := visitExpr(e^.inner);
    nkUnary: visitExpr := visitUnary(e);
    nkBinary: visitExpr := visitBinary(e);
    nkCall: visitExpr := visitCall(e);
    nkIndirectCall: visitExpr := visitIndirectCall(e);
    nkAddrOf: visitExpr := emitAddress(e^.inner);
    nkNil: visitExpr := irNull;
    nkSetCtor: visitExpr := emitSetCtor(e);
    nkDeref, nkIndex, nkField:
      if e^.typ^.kind = tyShortString then
        visitExpr := emitAddress(e)
      else
        visitExpr := loadScalar(emitAddress(e), e^.typ);
  else
    { formatted expressions and aggregates are handled by their users }
    visitExpr := irZero(irI32);
  end;
end;

{$I pcgenstmt.inc}

end.
