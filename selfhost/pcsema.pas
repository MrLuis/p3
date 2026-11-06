unit pcsema;
{ Semantic analysis of the self-hosted compiler; a port of
  src/sema/Sema.cpp. One pass over the tree in declaration order: scopes,
  constant folding, types (name equivalence), and the ISO 7185 rules of
  compatibility and assignment-compatibility, plus the Turbo/FPC
  extensions. It annotates the tree (types, declarations, constant values)
  and reports the same diagnostics as the C++ compiler. }

interface

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcscope;

{ Checks the units (dependencies first) and the program. Returns true if
  they are valid. }
function semaCheck(prog: PDecl; const units: TPtrList; m: TLangMode): boolean;

implementation

const
  pascalMaxInt = 2147483647;

type
  TConstant = record
    ty: PType;
    value: TConstValue;
  end;

  PPendingPointer = ^TPendingPointer;
  TPendingPointer = record
    spec: PTypeSpec;
    ptr: PType;
  end;

  PPendingGoto = ^TPendingGoto;
  TPendingGoto = record
    stmt: PNode;
    containers: TPtrList; { a copy of the containers at the goto }
    from: PDecl;
  end;

  TLongArray = array[0..$FFFFFFF] of longint;
  PLongArray = ^TLongArray;

var
  mode: TLangMode;
  standard, cur: PScope;
  unitInterfaces: TIntMap;    { unit name id -> PScope }
  curUnitInterface: PScope;   { while checking a unit's implementation }
  curRoutine: PDecl;          { nil in the program block }
  activeFors: TPtrList;       { of PNode (nkFor), innermost last }
  loopDepth: longint;
  pendingPointers: TPtrList;  { of PPendingPointer }
  containers: TPtrList;       { statement sequences / labeled statements }
  directSequence: pointer;
  routineBody: PNode;
  pendingGotos: TPtrList;     { of PPendingGoto }
  programLabelCount: longint; { non-local labels of the program block }
  msg: TBuf;                  { the diagnostic being built }

{ --- Small helpers --- }

function isISO: boolean;
begin
  isISO := mode = lmISO;
end;

function isError(t: PType): boolean;
begin
  isError := t^.kind = tyError;
end;

function errorType: PType;
begin
  errorType := tyErrorT;
end;

function predeclaredIdent(const s: string): TIdent;
var
  id: TIdent;
begin
  id.name := intern(s);
  id.spelling := id.name;
  id.loc := noLoc;
  predeclaredIdent := id;
end;

function labelKey(name: PName): PName;
begin
  labelKey := intern('#' + nameStr(name));
end;

function lookup(name: PName): PDecl;
begin
  lookup := scopeFind(cur, name);
end;

function constOrdinal(v: int64): TConstValue;
var
  c: TConstValue;
begin
  c.kind := ckOrdinal;
  c.intVal := v;
  c.realVal := 0;
  c.strVal := nil;
  constOrdinal := c;
end;

function constReal(v: double): TConstValue;
var
  c: TConstValue;
begin
  c.kind := ckReal;
  c.intVal := 0;
  c.realVal := v;
  c.strVal := nil;
  constReal := c;
end;

function constString(s: PName): TConstValue;
var
  c: TConstValue;
begin
  c.kind := ckString;
  c.intVal := 0;
  c.realVal := 0;
  c.strVal := s;
  constString := c;
end;

function constNone: TConstValue;
var
  c: TConstValue;
begin
  c.kind := ckNone;
  c.intVal := 0;
  c.realVal := 0;
  c.strVal := nil;
  constNone := c;
end;

function caseLabelRange(l: PCaseLabel): TRange;
begin
  if l^.hi <> nil then
    caseLabelRange := makeRange(l^.lo^.b, l^.hi^.e)
  else
    caseLabelRange := makeRange(l^.lo^.b, l^.lo^.e);
end;

function containsPtr(const l: TPtrList; p: pointer): boolean;
var
  i: longint;
begin
  for i := 0 to l.count - 1 do
    if listGet(l, i) = p then
      exit(true);
  containsPtr := false;
end;

{ --- Building diagnostics in msg --- }

procedure ms(const s: string);
begin
  bufClear(msg);
  bufAdd(msg, s);
end;

procedure ma(const s: string);
begin
  bufAdd(msg, s);
end;

procedure maN(n: PName);
begin
  if n <> nil then
    bufAddName(msg, n);
end;

{ 'type name' }
procedure maT(t: PType);
begin
  bufAddChar(msg, '''');
  bufAddTypeName(msg, t);
  bufAddChar(msg, '''');
end;

procedure maI(v: int64);
begin
  bufAddInt(msg, v);
end;

procedure maV(t: PType; v: int64);
begin
  bufAddOrdinal(msg, t, v);
end;

procedure errRR(loc: TLoc; r1, r2: TRange);
begin
  diagReportBuf(sevError, loc, msg, r1, r2);
end;

procedure errR(loc: TLoc; r: TRange);
begin
  diagReportBuf(sevError, loc, msg, r, noRange);
end;

procedure err(loc: TLoc);
begin
  diagReportBuf(sevError, loc, msg, noRange, noRange);
end;

procedure note(loc: TLoc);
begin
  diagReportBuf(sevNote, loc, msg, noRange, noRange);
end;

function kindName(d: PDecl): string;
begin
  case d^.kind of
    dkProgram: kindName := 'program';
    dkVar:
      case d^.varKind of
        vkVariable: kindName := 'variable';
        vkResult: kindName := 'function result';
      else
        kindName := 'parameter';
      end;
    dkConst: kindName := 'constant';
    dkType: kindName := 'type';
    dkField: kindName := 'field';
    dkLabel: kindName := 'label';
    dkUnit: kindName := 'unit';
    dkRoutine:
      if d^.isFunction then
        kindName := 'function'
      else
        kindName := 'procedure';
    dkBuiltin:
      if d^.isProcedure then
        kindName := 'procedure'
      else
        kindName := 'function';
  else
    kindName := 'declaration';
  end;
end;

function isPredeclared(d: PDecl): boolean;
begin
  isPredeclared := d^.loc = noLoc;
end;

{ --- "did you mean" --- }

function minOf(a, b: longint): longint;
begin
  if a < b then
    minOf := a
  else
    minOf := b;
end;

{ Levenshtein distance (llvm::StringRef::edit_distance with replacements) }
function editDistance(a, b: PName): longint;
var
  row: PLongArray;
  i, j, prev, old, m, n: longint;
begin
  m := a^.len;
  n := b^.len;
  getmem(row, int64(n + 1) * sizeof(longint));
  {$R-}
  for j := 0 to n do
    row^[j] := j;
  for i := 1 to m do
  begin
    prev := row^[0];
    row^[0] := i;
    for j := 1 to n do
    begin
      old := row^[j];
      if a^.chars^[i - 1] = b^.chars^[j - 1] then
        row^[j] := prev
      else
        row^[j] := 1 + minOf(prev, minOf(row^[j - 1], row^[j]));
      prev := old;
    end;
  end;
  editDistance := row^[n];
  {$R+}
  freemem(row);
end;

{ Is a < b, byte by byte (llvm::StringRef ordering)? }
function nameLess(a, b: PName): boolean;
var
  i, n: longint;
begin
  n := minOf(a^.len, b^.len);
  {$R-}
  for i := 0 to n - 1 do
    if a^.chars^[i] <> b^.chars^[i] then
      exit(a^.chars^[i] < b^.chars^[i]);
  {$R+}
  nameLess := a^.len < b^.len;
end;

{ The visible name closest to name: inner scopes win; within a scope the
  closest, ties to the alphabetically first. nil if none is close enough. }
function suggest(name: PName): PName;
var
  best, scopeBest, d, i: longint;
  res, scopeRes, cand: PName;
  s: PScope;
  names: TPtrList;
begin
  { one edit for short names, else a third of the length (at least two) }
  if name^.len <= 3 then
    best := 2
  else if name^.len div 3 > 2 then
    best := name^.len div 3 + 1
  else
    best := 3;
  res := nil;
  s := cur;
  listInit(names);
  while s <> nil do
  begin
    scopeBest := best;
    scopeRes := nil;
    listClear(names);
    scopeNames(s, names);
    for i := 0 to names.count - 1 do
    begin
      cand := PName(listGet(names, i));
      if ((cand^.len > 0) and (cand^.chars^[0] = '#')) or (cand = name) then
        continue;
      d := editDistance(name, cand);
      if (d < scopeBest) or
         ((d = scopeBest) and (d < best) and nameLess(cand, scopeRes)) then
      begin
        scopeBest := d;
        scopeRes := cand;
      end;
    end;
    if scopeBest < best then
    begin
      best := scopeBest;
      res := scopeRes;
    end;
    s := s^.parent;
  end;
  listFree(names);
  suggest := res;
end;

{ Appends "; did you mean 'X'?" (the name as declared), if any. }
procedure maDidYouMean(name: PName);
var
  s: PName;
  d: PDecl;
begin
  s := suggest(name);
  if s = nil then
    exit;
  d := lookup(s);
  if (d <> nil) and (d^.ident.spelling <> nil) and (d^.ident.spelling^.len > 0) then
    s := d^.ident.spelling;
  ma('; did you mean ''');
  maN(s);
  ma('''?');
end;

procedure diagUndeclared(const id: TIdent);
begin
  ms('use of undeclared identifier ''');
  maN(id.spelling);
  ma('''');
  maDidYouMean(id.name);
  errR(id.loc, identRange(id));
end;

procedure noteDeclaredHere(d: PDecl);
begin
  if not isPredeclared(d) then
  begin
    ms('''');
    maN(d^.ident.spelling);
    ma(''' declared here');
    note(d^.loc);
  end;
end;

procedure declare(d: PDecl);
var
  prev: PDecl;
begin
  prev := scopeInsert(cur, d^.ident.name, d);
  if prev <> nil then
  begin
    ms('redefinition of ''');
    maN(d^.ident.spelling);
    ma('''');
    errR(d^.ident.loc, identRange(d^.ident));
    if not isPredeclared(prev) then
    begin
      ms('previous definition is here');
      note(prev^.loc);
    end;
  end;
end;

{ --- Standard scope --- }

function isTPOnly(k: TBuiltinKind): boolean;
begin
  isTPOnly := k in [bkBreak, bkContinue, bkExit, bkInc, bkDec, bkDelete,
                    bkInsert, bkLength, bkCopy, bkPos, bkConcat, bkUpCase,
                    bkGetMem, bkFreeMem, bkReallocMem, bkFillChar, bkMove,
                    bkHalt, bkSizeOf, bkLow, bkHigh, bkAssigned, bkParamCount,
                    bkParamStr];
end;

procedure stdType(const n: string; t: PType);
var
  d: PDecl;
begin
  d := newDecl(dkType, predeclaredIdent(n), noLoc);
  d^.declType := t;
  scopeInsert(standard, d^.ident.name, d);
end;

procedure stdConst(const n: string; t: PType; v: int64);
var
  d: PDecl;
begin
  d := newDecl(dkConst, predeclaredIdent(n), noLoc);
  d^.constType := t;
  d^.constValue := constOrdinal(v);
  scopeInsert(standard, d^.ident.name, d);
end;

procedure declareStandardScope;
var
  pchar: PType;
  k: TBuiltinKind;
  d: PDecl;
  f: longint;
begin
  stdType('integer', tyIntegerT);
  stdType('real', tyRealT);
  stdType('boolean', tyBooleanT);
  stdType('char', tyCharT);
  stdType('text', tyTextT);
  if not isISO then
  begin
    stdType('pointer', tyUntypedPointerT);
    { longint is integer (32-bit), and double is real }
    stdType('longint', tyIntegerT);
    stdType('int64', tyInt64T);
    stdType('cardinal', tyCardinalT);
    stdType('longword', tyCardinalT);
    stdType('byte', tyByteT);
    stdType('word', tyWordT);
    stdType('shortint', tyShortIntT);
    stdType('smallint', tySmallIntT);
    stdType('double', tyRealT);
    { pchar: a pointer to char, for C strings }
    pchar := newPointerType(tyCharT);
    setDeclName(pchar, intern('pchar'));
    stdType('pchar', pchar);
  end;
  { the standard files }
  for f := 1 to 2 do
  begin
    if f = 1 then
      d := newDecl(dkVar, predeclaredIdent('input'), noLoc)
    else
      d := newDecl(dkVar, predeclaredIdent('output'), noLoc);
    d^.varKind := vkVariable;
    d^.varType := tyTextT;
    scopeInsert(standard, d^.ident.name, d);
  end;
  stdConst('false', tyBooleanT, 0);
  stdConst('true', tyBooleanT, 1);
  stdConst('maxint', tyIntegerT, pascalMaxInt);
  for k := low(TBuiltinKind) to high(TBuiltinKind) do
    if not isTPOnly(k) or not isISO then
    begin
      d := newDecl(dkBuiltin, predeclaredIdent(builtinNames[k]), noLoc);
      d^.builtin := k;
      d^.isProcedure := builtinIsProcedure[k];
      scopeInsert(standard, d^.ident.name, d);
    end;
end;

{ --- Integer types (Turbo/FPC rules; ISO has only integer) --- }

function promoteInteger(t: PType): PType;
var
  i: PType;
begin
  i := getBase(t);
  if i^.bits < 32 then
    promoteInteger := tyIntegerT
  else
    promoteInteger := i;
end;

function fold(e: PNode; var c: TConstant): boolean; forward;

function isCardinalConstant(e: PNode): boolean;
var
  c: TConstant;
begin
  isCardinalConstant := fold(e, c) and (c.value.kind = ckOrdinal) and
                        (c.value.intVal >= 0) and (c.value.intVal <= 4294967295);
end;

{ The type integer operands are converted to for an operator: int64 if
  either is int64; cardinal if both are unsigned (or the other is a
  constant that fits); int64 when cardinal meets a signed value; integer
  otherwise. }
function integerOperandType(le: PNode; l: PType; re: PNode; r: PType): PType;
var
  pl, pr, otherTy: PType;
  other: PNode;
begin
  pl := promoteInteger(l);
  pr := promoteInteger(r);
  if (pl = tyInt64T) or (pr = tyInt64T) then
    exit(tyInt64T);
  if (pl = tyCardinalT) and (pr = tyCardinalT) then
    exit(tyCardinalT);
  if (pl = tyCardinalT) or (pr = tyCardinalT) then
  begin
    if pl = tyCardinalT then
    begin
      other := re;
      otherTy := r;
    end
    else
    begin
      other := le;
      otherTy := l;
    end;
    if not isSignedIntegerLike(otherTy) or isCardinalConstant(other) then
      exit(tyCardinalT);
    exit(tyInt64T);
  end;
  integerOperandType := tyIntegerT;
end;

function visitExpr(e: PNode): PType; forward;

{ Type-checks e once (later calls return the cached type). }
function check(e: PNode): PType;
var
  t: PType;
begin
  if e^.typ <> nil then
    exit(e^.typ);
  t := visitExpr(e);
  e^.typ := t;
  check := t;
end;

{ --- Type relations (ISO 7185 6.4.5 / 6.4.6) --- }

function congruent(const a: TPtrList; aResult: PType; const b: TPtrList;
                   bResult: PType): boolean; forward;

function areCompatible(a, b: PType): boolean;
begin
  if a = b then
    exit(true);
  if ((a^.kind = tyPointer) and (b^.kind = tyNil)) or
     ((a^.kind = tyNil) and (b^.kind = tyPointer)) then
    exit(true);
  if (a^.kind = tyPointer) and (b^.kind = tyPointer) and
     (a^.untyped or b^.untyped) then
    exit(true); { Turbo/FPC untyped pointer }
  if (a^.kind = tySet) and (b^.kind = tySet) then
    exit(isEmptySet(a) or isEmptySet(b) or
         haveSameOrdinalHost(a^.setElement, b^.setElement));
  if isOrdinal(a) and isOrdinal(b) then
    exit(haveSameOrdinalHost(a, b));
  if isStringType(a) and isStringType(b) then
    exit(getStringLength(a) = getStringLength(b));
  areCompatible := false;
end;

{ Can a value of type value be assigned to a variable of type target? }
function isAssignable(target, value: PType): boolean;
begin
  if containsFile(target) or containsFile(value) then
    exit(false); { files cannot be copied }
  if target = value then
    exit(true);
  if (target^.kind = tyReal) and isIntegerLike(value) then
    exit(true);
  if (target^.kind = tyPointer) and ((value^.kind = tyNil) or (value^.kind = tyPointer)) then
    exit(areCompatible(target, value));
  if (target^.kind = tyShortString) and isStringish(value) then
    exit(true); { truncated to the target's length at run time }
  if (target^.kind = tySet) and (value^.kind = tySet) then
    exit(areCompatible(target, value));
  { procedural values (Turbo/FPC): congruent headings, or nil }
  if target^.kind = tyRoutine then
  begin
    if value^.kind = tyNil then
      exit(true);
    exit((value^.kind = tyRoutine) and
         congruent(target^.routineParams, target^.routineResult,
                   value^.routineParams, value^.routineResult));
  end;
  if isOrdinal(target) and isOrdinal(value) then
    exit(haveSameOrdinalHost(target, value));
  if isStringType(target) and isStringType(value) and
     (target^.kind <> tyStringLit) then
    exit(getStringLength(target) = getStringLength(value));
  isAssignable := false;
end;

{ Can e be used where a variable is required? }
function isVariableReference(e: PNode): boolean;
var
  d: PDecl;
begin
  case e^.kind of
    nkNameRef:
      begin
        d := e^.refDecl;
        isVariableReference := (d <> nil) and (d^.kind in [dkVar, dkField]);
      end;
    nkIndex, nkField:
      isVariableReference := isVariableReference(e^.base);
    nkDeref:
      isVariableReference := true; { p^ is a variable even for a call's result }
  else
    isVariableReference := false;
  end;
end;

{ The const parameter a designator is part of (x, x[i], x.f; not x^). }
function constParamRoot(e: PNode): PDecl;
begin
  while true do
    case e^.kind of
      nkIndex, nkField: e := e^.base;
      nkParen: e := e^.inner;
    else
      begin
        if (e^.kind = nkNameRef) and (e^.refDecl <> nil) and
           (e^.refDecl^.kind = dkVar) and (e^.refDecl^.varKind = vkConstParam) then
          exit(e^.refDecl);
        exit(nil);
      end;
    end;
end;

{ Reports a change to a const parameter; false if reported. }
function checkModifiable(e: PNode): boolean;
var
  v: PDecl;
begin
  v := constParamRoot(e);
  if v = nil then
    exit(true);
  ms('cannot modify constant parameter ''');
  maN(v^.ident.spelling);
  ma('''');
  errR(e^.b, nodeRange(e));
  noteDeclaredHere(v);
  checkModifiable := false;
end;

{ --- Constant folding --- }

{ ISO 7185 6.3 constant: [sign] (number | constant-identifier) | string }
function isISOConstantForm(e: PNode): boolean;
begin
  if e^.kind = nkUnary then
  begin
    if e^.uop = uoNot then
      exit(false);
    e := e^.operand;
    exit(e^.kind in [nkIntegerLit, nkRealLit, nkNameRef]);
  end;
  isISOConstantForm := e^.kind in [nkIntegerLit, nkRealLit, nkStringLit, nkNameRef];
end;

function asReal(const c: TConstant): double;
begin
  if c.value.kind = ckReal then
    asReal := c.value.realVal
  else
    asReal := c.value.intVal;
end;

{ T(v): integer types wrap to their width (byte(300) = 44); other ordinal
  types keep the value. }
function castOrdinalValue(v: int64; t: PType): int64;
var
  i: PType;
  mask, u: int64;
begin
  i := getBase(t);
  if (i^.kind <> tyInteger) or (i^.bits = 64) then
    exit(v);
  mask := (int64(1) shl i^.bits) - 1;
  u := v and mask;
  if i^.isSigned and (((u shr (i^.bits - 1)) and 1) <> 0) then
    u := u or not mask; { sign-extend }
  castOrdinalValue := u;
end;

function mkConst(t: PType; const v: TConstValue; var c: TConstant): boolean;
begin
  c.ty := t;
  c.value := v;
  mkConst := true;
end;

{ IEEE comparisons without trapping on NaN (Free Pascal traps on invalid
  operations) }
function realCompare(op: TBinaryOp; x, y: double): boolean;
begin
  if realIsNaN(x) or realIsNaN(y) then
    exit(op = boNe);
  case op of
    boEq: realCompare := x = y;
    boNe: realCompare := x <> y;
    boLt: realCompare := x < y;
    boLe: realCompare := x <= y;
    boGt: realCompare := x > y;
  else
    realCompare := x >= y;
  end;
end;

{ Rounds x to an integer value: trunc, ISO round (halves away from zero) or
  Turbo/FPC round (halves to even). false if x is not finite or the result
  is outside int64. }
function roundReal(x: double; isRound: boolean; var v: int64): boolean;
var
  t: int64;
  f: double;
begin
  roundReal := false;
  if realIsNaN(x) or realIsInf(x) then
    exit;
  if (x >= 9223372036854775808.0) or (x < -9223372036854775808.0) then
    exit; { also out of range once rounded: integral at this size }
  if abs(x) >= 4503599627370496.0 then { 2^52: x is integral }
  begin
    v := trunc(x);
    exit(true);
  end;
  t := trunc(x);
  f := x - t; { exact }
  if isRound then
  begin
    if (abs(f) > 0.5) or ((abs(f) = 0.5) and (isISO or odd(t))) then
    begin
      if x < 0 then
        t := t - 1
      else
        t := t + 1;
    end;
  end;
  v := t;
  roundReal := true;
end;

{ Folds an already type-checked expression; false if it is not constant
  (without diagnosing). }
function fold(e: PNode; var c: TConstant): boolean;
var
  t: PType;
  l, r, v: TConstant;
  isReal, wide: boolean;
  a, b: int64;
  x, y: double;
  d: PDecl;
  bk: TBuiltinKind;
  at: PType;
  bd: TOrdinalBounds;
  iv: int64;
  cmp: boolean;
begin
  fold := false;
  t := e^.typ;
  if (t = nil) or isError(t) then
    exit;
  case e^.kind of
    nkIntegerLit:
      exit(mkConst(t, constOrdinal(e^.intValue), c));
    nkRealLit:
      exit(mkConst(t, constReal(e^.realValue), c));
    nkStringLit:
      begin
        if e^.strValue^.len = 1 then
          exit(mkConst(t, constOrdinal(ord(e^.strValue^.chars^[0])), c));
        exit(mkConst(t, constString(e^.strValue), c));
      end;
    nkNameRef:
      begin
        d := e^.refDecl;
        if (d = nil) or (d^.kind <> dkConst) or (d^.constValue.kind = ckNone) then
          exit;
        exit(mkConst(d^.constType, d^.constValue, c));
      end;
    nkParen:
      exit(fold(e^.inner, c));
    nkUnary:
      begin
        if not fold(e^.operand, v) then
          exit;
        case e^.uop of
          uoPlus: exit(mkConst(t, v.value, c));
          uoMinus:
            begin
              if v.value.kind = ckReal then
                exit(mkConst(t, constReal(-v.value.realVal), c));
              {$Q-}{$R-}
              iv := 0 - v.value.intVal;
              {$Q+}{$R+}
              exit(mkConst(t, constOrdinal(iv), c));
            end;
        else { not: boolean, or bitwise in the operand's type }
          begin
            if getBase(t)^.kind = tyBoolean then
              exit(mkConst(t, constOrdinal(ord(v.value.intVal = 0)), c));
            exit(mkConst(t, constOrdinal(castOrdinalValue(not v.value.intVal, t)), c));
          end;
        end;
      end;
    nkBinary:
      begin
        if not fold(e^.lhs, l) or not fold(e^.rhs, r) or
           (l.value.kind = ckString) or (r.value.kind = ckString) then
          exit;
        isReal := (l.value.kind = ckReal) or (r.value.kind = ckReal);
        a := l.value.intVal;
        b := r.value.intVal;
        x := asReal(l);
        y := asReal(r);
        { wrap around in 64 bits; results that do not fit their type are
          reported by evaluateConstant }
        wide := isIntegerLike(t) and (getBase(t)^.bits = 64);
        {$Q-}{$R-}
        case e^.bop of
          boAdd:
            if isReal then
              exit(mkConst(t, constReal(realAdd(x, y)), c))
            else
              exit(mkConst(t, constOrdinal(a + b), c));
          boSub:
            if isReal then
              exit(mkConst(t, constReal(realSub(x, y)), c))
            else
              exit(mkConst(t, constOrdinal(a - b), c));
          boMul:
            if isReal then
              exit(mkConst(t, constReal(realMul(x, y)), c))
            else
              exit(mkConst(t, constOrdinal(a * b), c));
          boRealDiv:
            begin
              if not realIsNaN(y) and (y = 0) then
                exit;
              exit(mkConst(t, constReal(realDiv(x, y)), c));
            end;
          boIntDiv:
            begin
              if (b = 0) or ((a = low(int64)) and (b = -1)) then
                exit;
              exit(mkConst(t, constOrdinal(a div b), c));
            end;
          boMod:
            begin
              if b <= 0 then
                exit;
              exit(mkConst(t, constOrdinal(((a mod b) + b) mod b), c));
            end;
          boAnd:
            if getBase(t)^.kind = tyBoolean then
              exit(mkConst(t, constOrdinal(ord((a <> 0) and (b <> 0))), c))
            else
              exit(mkConst(t, constOrdinal(a and b), c));
          boOr:
            if getBase(t)^.kind = tyBoolean then
              exit(mkConst(t, constOrdinal(ord((a <> 0) or (b <> 0))), c))
            else
              exit(mkConst(t, constOrdinal(a or b), c));
          boXor:
            exit(mkConst(t, constOrdinal(a xor b), c));
          boShl:
            if wide then
              exit(mkConst(t, constOrdinal(a shl (b and 63)), c))
            else
              exit(mkConst(t, constOrdinal(longint(cardinal(a) shl (b and 31))), c));
          boShr:
            if wide then
              exit(mkConst(t, constOrdinal(a shr (b and 63)), c))
            else
              exit(mkConst(t, constOrdinal(longint(cardinal(a) shr (b and 31))), c));
          boEq, boNe, boLt, boLe, boGt, boGe:
            begin
              if isReal then
                cmp := realCompare(e^.bop, x, y)
              else
                case e^.bop of
                  boEq: cmp := a = b;
                  boNe: cmp := a <> b;
                  boLt: cmp := a < b;
                  boLe: cmp := a <= b;
                  boGt: cmp := a > b;
                else
                  cmp := a >= b;
                end;
              exit(mkConst(t, constOrdinal(ord(cmp)), c));
            end;
        else
          exit; { in }
        end;
        {$Q+}{$R+}
      end;
    nkCall:
      begin
        d := e^.callDecl;
        if (d <> nil) and (d^.kind = dkType) then
        begin { T(x) }
          if not fold(PNode(listGet(e^.args, 0)), v) or
             (v.value.kind <> ckOrdinal) or not isOrdinal(t) then
            exit;
          exit(mkConst(t, constOrdinal(castOrdinalValue(v.value.intVal, t)), c));
        end;
        if (d = nil) or (d^.kind <> dkBuiltin) or (e^.args.count <> 1) then
          exit;
        bk := d^.builtin;
        if (bk = bkLow) or (bk = bkHigh) then
        begin
          at := PNode(listGet(e^.args, 0))^.typ;
          if at^.kind = tyShortString then
          begin
            if bk = bkHigh then
              exit(mkConst(t, constOrdinal(at^.maxLength), c));
            exit(mkConst(t, constOrdinal(0), c));
          end;
          if at^.kind = tyArray then
            at := at^.indexType;
          bd := getOrdinalBounds(at);
          if bk = bkHigh then
            exit(mkConst(t, constOrdinal(bd.hi), c));
          exit(mkConst(t, constOrdinal(bd.lo), c));
        end;
        if not fold(PNode(listGet(e^.args, 0)), v) or (v.value.kind = ckString) then
          exit;
        {$Q-}{$R-}
        case bk of
          bkOrd, bkChr: exit(mkConst(t, constOrdinal(v.value.intVal), c));
          bkSucc: exit(mkConst(t, constOrdinal(v.value.intVal + 1), c));
          bkPred: exit(mkConst(t, constOrdinal(v.value.intVal - 1), c));
          bkOdd: exit(mkConst(t, constOrdinal(v.value.intVal and 1), c));
          bkAbs:
            if v.value.kind = ckReal then
              exit(mkConst(t, constReal(abs(v.value.realVal)), c))
            else if v.value.intVal < 0 then
              exit(mkConst(t, constOrdinal(0 - v.value.intVal), c))
            else
              exit(mkConst(t, constOrdinal(v.value.intVal), c));
          bkSqr:
            if v.value.kind = ckReal then
              exit(mkConst(t, constReal(realMul(v.value.realVal, v.value.realVal)), c))
            else
              exit(mkConst(t, constOrdinal(v.value.intVal * v.value.intVal), c));
          bkTrunc, bkRound:
            begin
              if not roundReal(asReal(v), bk = bkRound, iv) then
                exit;
              exit(mkConst(t, constOrdinal(iv), c));
            end;
        else
          exit;
        end;
        {$Q+}{$R+}
      end;
  else
    exit;
  end;
end;

{ Checks and folds a constant expression in a declaration. Reports
  non-constants and, in ISO mode, expressions beyond the ISO syntax. }
function evaluateConstant(e: PNode; const what: string; var c: TConstant): boolean;
var
  t: PType;
  bd: TOrdinalBounds;
begin
  evaluateConstant := false;
  t := check(e);
  if isError(t) then
    exit;
  if not fold(e, c) then
  begin
    ms(what + ' must be a constant');
    errR(e^.b, nodeRange(e));
    exit;
  end;
  if isISO and not isISOConstantForm(e) then
  begin
    ms('constant expressions are a Turbo/Free Pascal extension; use --mode=tp');
    errR(e^.b, nodeRange(e));
  end;
  if (c.value.kind = ckOrdinal) and isIntegerLike(c.ty) then
  begin
    bd := getOrdinalBounds(getBase(c.ty));
    if (c.value.intVal < bd.lo) or (c.value.intVal > bd.hi) then
    begin
      { Turbo/FPC: integer constant expressions widen to int64 }
      if not isISO and isSignedIntegerLike(getBase(c.ty)) then
      begin
        c.ty := tyInt64T;
        exit(true);
      end;
      ms('constant value is out of range for ');
      bufAddTypeName(msg, getBase(c.ty));
      errR(e^.b, nodeRange(e));
      exit;
    end;
  end;
  evaluateConstant := true;
end;

{ --- Types --- }

function checkLabel(l: PCaseLabel; selTy: PType; const what: string): boolean; forward;
function resolveType(ts: PTypeSpec; declName: PName): PType; forward;

function addField(f: PDecl; var all: TPtrList; var seen: TIntMap): boolean;
var
  prev: PDecl;
begin
  prev := PDecl(mapGet(seen, f^.ident.name^.id));
  if prev <> nil then
  begin
    ms('duplicate field name ''');
    maN(f^.ident.spelling);
    ma('''');
    errR(f^.loc, identRange(f^.ident));
    ms('previous field is here');
    note(prev^.loc);
    exit(false);
  end;
  mapPut(seen, f^.ident.name^.id, f);
  listAdd(all, f);
  addField := true;
end;

function resolveFieldList(fl: PFieldList; var all: TPtrList; var seen: TIntMap): boolean;
var
  ok: boolean;
  i, j: longint;
  f: PDecl;
  vp: PVariantPart;
  tagTy: PType;
  v: PVariant;
begin
  ok := true;
  for i := 0 to fl^.fixed.count - 1 do
  begin
    f := PDecl(listGet(fl^.fixed, i));
    f^.fieldType := resolveType(f^.fieldSpec, nil);
    if isError(f^.fieldType) then
      ok := false;
    if not addField(f, all, seen) then
      ok := false;
  end;
  vp := fl^.variant;
  if vp = nil then
    exit(ok);
  tagTy := resolveType(vp^.tagSpec, nil);
  if not isError(tagTy) and not isOrdinal(tagTy) then
  begin
    ms('variant tag type must be an ordinal type, not ');
    maT(tagTy);
    errR(vp^.tagSpec^.b, makeRange(vp^.tagSpec^.b, vp^.tagSpec^.e));
    tagTy := errorType;
  end;
  vp^.tagType := tagTy;
  if isError(tagTy) then
    ok := false;
  if vp^.tag <> nil then
  begin
    vp^.tag^.fieldType := tagTy;
    if not addField(vp^.tag, all, seen) then
      ok := false;
  end;
  for i := 0 to vp^.variants.count - 1 do
  begin
    v := PVariant(listGet(vp^.variants, i));
    for j := 0 to v^.labels.count - 1 do
      if not checkLabel(PCaseLabel(listGet(v^.labels, j)), tagTy, 'variant label') then
        ok := false;
    if not resolveFieldList(v^.fields, all, seen) then
      ok := false;
  end;
  resolveFieldList := ok;
end;

function specRange(ts: PTypeSpec): TRange;
begin
  specRange := makeRange(ts^.b, ts^.e);
end;

function buildType(ts: PTypeSpec): PType;
var
  d, cd: PDecl;
  t, elem, idx, res: PType;
  i: longint;
  id: PIdent;
  lo, hi, mc: TConstant;
  host: PType;
  ok: boolean;
  indexes, all: TPtrList;
  pp: PPendingPointer;
  bd: TOrdinalBounds;
  seen: TIntMap;
  p: PDecl;
begin
  buildType := errorType;
  case ts^.kind of
    tsNamed:
      begin
        d := lookup(ts^.tname.name);
        if d = nil then
        begin
          ms('unknown type name ''');
          maN(ts^.tname.spelling);
          ma('''');
          maDidYouMean(ts^.tname.name);
          errR(ts^.tname.loc, identRange(ts^.tname));
          exit;
        end;
        if d^.kind = dkType then
        begin
          if d^.declType <> nil then
            buildType := d^.declType;
          exit;
        end;
        ms('''');
        maN(ts^.tname.spelling);
        ma(''' is a ' + kindName(d) + ', not a type');
        errR(ts^.tname.loc, identRange(ts^.tname));
        noteDeclaredHere(d);
      end;
    tsEnum:
      begin
        t := newEnumType;
        for i := 0 to ts^.enumNames.count - 1 do
        begin
          id := PIdent(listGet(ts^.enumNames, i));
          cd := newDecl(dkConst, id^, id^.loc);
          cd^.constType := t;
          cd^.constValue := constOrdinal(t^.enumValues.count);
          listAdd(t^.enumValues, cd);
          declare(cd);
        end;
        buildType := t;
      end;
    tsSubrange:
      begin
        { both bounds are evaluated before checking either }
        ok := evaluateConstant(ts^.lo, 'subrange bound', lo);
        if not evaluateConstant(ts^.hi, 'subrange bound', hi) or not ok then
          exit;
        if not isOrdinal(lo.ty) or not isOrdinal(hi.ty) then
        begin
          ms('subrange bounds must be of an ordinal type');
          errR(ts^.b, specRange(ts));
          exit;
        end;
        if not haveSameOrdinalHost(lo.ty, hi.ty) then
        begin
          ms('subrange bounds have different types (');
          maT(getBase(lo.ty));
          ma(' and ');
          maT(getBase(hi.ty));
          ma(')');
          errR(ts^.b, specRange(ts));
          exit;
        end;
        if lo.value.intVal > hi.value.intVal then
        begin
          ms('subrange lower bound ');
          maV(lo.ty, lo.value.intVal);
          ma(' is greater than upper bound ');
          maV(hi.ty, hi.value.intVal);
          errR(ts^.b, specRange(ts));
          exit;
        end;
        { integer bounds of different types: the wider host }
        host := getBase(lo.ty);
        if (host^.kind = tyInteger) and (getBase(hi.ty)^.bits > host^.bits) then
          host := getBase(hi.ty);
        buildType := newSubrangeType(host, lo.value.intVal, hi.value.intVal);
      end;
    tsArray:
      begin
        elem := resolveType(ts^.element, nil);
        listInit(indexes);
        ok := not isError(elem);
        for i := 0 to ts^.indexes.count - 1 do
        begin
          idx := resolveType(PTypeSpec(listGet(ts^.indexes, i)), nil);
          if isError(idx) then
            ok := false
          else if not isOrdinal(idx) then
          begin
            ms('array index type must be an ordinal type, not ');
            maT(idx);
            errR(PTypeSpec(listGet(ts^.indexes, i))^.b,
                 specRange(PTypeSpec(listGet(ts^.indexes, i))));
            ok := false;
          end
          else if (idx^.kind = tyInteger) and (idx^.bits >= 32) then
          begin
            ms('array index type ');
            maT(idx);
            ma(' is too large; use a subrange such as 1..100');
            errR(PTypeSpec(listGet(ts^.indexes, i))^.b,
                 specRange(PTypeSpec(listGet(ts^.indexes, i))));
            ok := false;
          end;
          listAdd(indexes, idx);
        end;
        if not ok then
          exit;
        { array [A, B] of E = array [A] of array [B] of E }
        t := elem;
        for i := indexes.count - 1 downto 0 do
          t := newArrayType(PType(listGet(indexes, i)), t, ts^.arrPacked);
        buildType := t;
      end;
    tsPointer:
      begin
        { resolved at the end of the type section: ^T binds to a T declared
          later in the same block even if an outer T is visible }
        t := newPointerType(nil);
        pp := PPendingPointer(arenaAlloc(sizeof(TPendingPointer)));
        pp^.spec := ts;
        pp^.ptr := t;
        listAdd(pendingPointers, pp);
        buildType := t;
      end;
    tsRoutine:
      begin
        for i := 0 to ts^.rtParams.count - 1 do
        begin
          p := PDecl(listGet(ts^.rtParams, i));
          p^.varType := resolveType(p^.varSpec, nil);
        end;
        res := nil;
        if ts^.rtIsFunction then
        begin
          res := resolveType(ts^.rtResult, nil);
          if not isError(res) and not isValidResultType(res) then
          begin
            ms('function result type must be a simple or pointer type, not ');
            maT(res);
            errR(ts^.rtResult^.b, specRange(ts^.rtResult));
            res := errorType;
          end;
        end;
        buildType := newRoutineType(ts^.rtParams, res);
      end;
    tsString:
      begin
        if ts^.maxLen = nil then
          exit(shortStringType(255));
        if not evaluateConstant(ts^.maxLen, 'string length', mc) then
          exit;
        if not isIntegerLike(mc.ty) or (mc.value.intVal < 1) or
           (mc.value.intVal > 255) then
        begin
          ms('string length must be an integer in 1..255');
          errR(ts^.maxLen^.b, nodeRange(ts^.maxLen));
          exit;
        end;
        buildType := shortStringType(longint(mc.value.intVal));
      end;
    tsFile:
      begin
        elem := resolveType(ts^.elemSpec, nil);
        if isError(elem) then
          exit;
        if containsFile(elem) then
        begin
          ms('the components of a file cannot be files');
          errR(ts^.elemSpec^.b, specRange(ts^.elemSpec));
          exit;
        end;
        buildType := newFileType(elem);
      end;
    tsSet:
      begin
        elem := resolveType(ts^.elemSpec, nil);
        if isError(elem) then
          exit;
        if not isOrdinal(elem) then
        begin
          ms('set element type must be an ordinal type, not ');
          maT(elem);
          errR(ts^.elemSpec^.b, specRange(ts^.elemSpec));
          exit;
        end;
        bd := getOrdinalBounds(elem);
        if (bd.lo < 0) or (bd.hi > 255) then
        begin
          ms('set element type ');
          maT(elem);
          ma(' has values outside 0..255; use a subrange');
          errR(ts^.elemSpec^.b, specRange(ts^.elemSpec));
          exit;
        end;
        buildType := newSetType(elem);
      end;
    tsRecord:
      begin
        listInit(all);
        mapInit(seen);
        if not resolveFieldList(ts^.fields, all, seen) then
          exit;
        buildType := newRecordType(ts^.fields, all, ts^.recPacked);
      end;
  end;
end;

function resolveType(ts: PTypeSpec; declName: PName): PType;
var
  t: PType;
begin
  if ts^.resolved <> nil then
    exit(ts^.resolved);
  t := buildType(ts);
  if (declName <> nil) and not isError(t) and not isBuiltinType(t) then
    setDeclName(t, declName);
  ts^.resolved := t;
  resolveType := t;
end;

{ --- Declarations --- }

procedure declareConst(c: PDecl);
var
  v: TConstant;
begin
  if evaluateConstant(c^.constInit, 'constant value', v) then
  begin
    c^.constType := v.ty;
    c^.constValue := v.value;
  end
  else
  begin
    c^.constType := errorType;
    c^.constValue := constNone;
  end;
  declare(c);
end;

procedure declareType(td: PDecl);
begin
  td^.declType := resolveType(td^.typeSpec, td^.ident.spelling);
  declare(td);
end;

{ Is t an array of char (any index type) that a string constant of length
  n initializes (Turbo/FPC)? }
function isCharArrayOfLength(t: PType; n: int64): boolean;
var
  bd: TOrdinalBounds;
begin
  if (t^.kind <> tyArray) or (t^.elementType^.kind <> tyChar) then
    exit(false);
  bd := getOrdinalBounds(t^.indexType);
  isCharArrayOfLength := bd.hi - bd.lo + 1 = n;
end;

{ Checks the initial value of a typed constant of type t: a constant
  expression (folded into folded) or, for arrays and records, an aggregate
  whose elements are checked recursively. }
function checkConstInit(init: PNode; t: PType; var folded: TConstValue): boolean;
var
  want: int64;
  ok, charArray: boolean;
  i: longint;
  el: PAggElement;
  f: PDecl;
  seen: TIntMap;
  bd: TOrdinalBounds;
  c: TConstant;
  recordForm: boolean;
begin
  checkConstInit := false;
  if init^.kind = nkAggregate then
  begin
    init^.typ := t;
    recordForm := (init^.aggElems.count > 0) and
                  identValid(PAggElement(listGet(init^.aggElems, 0))^.field);
    if t^.kind = tyArray then
    begin
      bd := getOrdinalBounds(t^.indexType);
      want := bd.hi - bd.lo + 1;
      if recordForm or (init^.aggElems.count <> want) then
      begin
        ms('initializer for ');
        maT(t);
        ma(' must list ');
        maI(want);
        ma(' element');
        if want <> 1 then
          ma('s');
        ma(', separated by '',''');
        errR(init^.b, nodeRange(init));
        exit;
      end;
      ok := true;
      for i := 0 to init^.aggElems.count - 1 do
      begin
        el := PAggElement(listGet(init^.aggElems, i));
        if not checkConstInit(el^.value, t^.elementType, el^.folded) then
          ok := false;
      end;
      exit(ok);
    end;
    if t^.kind = tyRecord then
    begin
      if t^.fieldList^.variant <> nil then
      begin
        ms('typed constants of records with variant parts are not supported');
        errR(init^.b, nodeRange(init));
        exit;
      end;
      if not recordForm then
      begin
        ms('initializer for record ');
        maT(t);
        ma(' must name its fields: (field: value; ...)');
        errR(init^.b, nodeRange(init));
        exit;
      end;
      ok := true;
      mapInit(seen);
      for i := 0 to init^.aggElems.count - 1 do
      begin
        el := PAggElement(listGet(init^.aggElems, i));
        f := findField(t, el^.field.name);
        if f = nil then
        begin
          ms('no field named ''');
          maN(el^.field.spelling);
          ma(''' in ');
          maT(t);
          errR(el^.field.loc, identRange(el^.field));
          ok := false;
          continue;
        end;
        if mapHas(seen, el^.field.name^.id) then
        begin
          ms('field ''');
          maN(el^.field.spelling);
          ma(''' is initialized twice');
          errR(el^.field.loc, identRange(el^.field));
          ok := false;
          continue;
        end;
        mapPut(seen, el^.field.name^.id, f);
        if not checkConstInit(el^.value, f^.fieldType, el^.folded) then
          ok := false;
      end;
      exit(ok);
    end;
    if (init^.aggElems.count = 1) and not recordForm then { (1 + 2) }
      exit(checkConstInit(PAggElement(listGet(init^.aggElems, 0))^.value, t, folded));
    ms('a parenthesized list cannot initialize ');
    maT(t);
    errR(init^.b, nodeRange(init));
    exit;
  end;

  if not evaluateConstant(init, 'typed constant value', c) then
    exit;
  charArray := (c.value.kind = ckString) and isCharArrayOfLength(t, c.value.strVal^.len);
  if not charArray and not (isSimple(t) or (t^.kind = tyShortString) or isStringType(t)) then
  begin
    ms('typed constants of type ');
    maT(t);
    ma(' need a parenthesized initializer');
    errR(init^.b, nodeRange(init));
    exit;
  end;
  if not charArray and not isAssignable(t, c.ty) then
  begin
    ms('cannot initialize ');
    maT(t);
    ma(' with a value of type ');
    maT(c.ty);
    errR(init^.b, nodeRange(init));
    exit;
  end;
  if (c.value.kind = ckOrdinal) and isOrdinal(t) then
  begin
    bd := getOrdinalBounds(t);
    if (c.value.intVal < bd.lo) or (c.value.intVal > bd.hi) then
    begin
      ms('value ');
      maV(c.ty, c.value.intVal);
      ma(' is out of range for ');
      maT(t);
      errR(init^.b, nodeRange(init));
      exit;
    end;
  end;
  folded := c.value;
  checkConstInit := true;
end;

procedure declareVar(v: PDecl);
var
  folded: TConstValue;
begin
  v^.varType := resolveType(v^.varSpec, nil);
  v^.owner := curRoutine;
  if v^.init <> nil then
  begin
    { typed constants are static: they keep their value across calls }
    v^.owner := nil;
    v^.isUsed := true;
    folded := constNone;
    if not isError(v^.varType) and checkConstInit(v^.init, v^.varType, folded) then
      v^.initValue := folded;
  end;
  declare(v);
end;

{ Resolves a routine's parameter and result types. }
procedure resolveSignature(r: PDecl);
var
  i: longint;
  p, res: PDecl;
  resultTy: PType;
begin
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    p^.owner := r;
    p^.varType := resolveType(p^.varSpec, nil);
    if (p^.varKind <> vkVarParam) and containsFile(p^.varType) then
    begin
      ms('file parameter ''');
      maN(p^.ident.spelling);
      ma(''' must be a ''var'' parameter');
      errR(p^.loc, identRange(p^.ident));
    end;
  end;
  if not r^.isFunction then
    exit;
  resultTy := errorType;
  if r^.resultSpec = nil then
  begin
    ms('function ''');
    maN(r^.ident.spelling);
    ma(''' needs a result type');
    errR(r^.ident.loc, identRange(r^.ident));
  end
  else
  begin
    resultTy := resolveType(r^.resultSpec, nil);
    { ISO: a simple type or a pointer; Turbo/FPC also strings and records
      (without files) }
    if not isError(resultTy) and not isValidResultType(resultTy) and
       (resultTy^.kind <> tyShortString) and
       not (not isISO and (resultTy^.kind = tyRecord) and not containsFile(resultTy)) then
    begin
      ms('function result type must be a simple or pointer type, not ');
      maT(resultTy);
      errR(r^.resultSpec^.b, specRange(r^.resultSpec));
      resultTy := errorType;
    end;
  end;
  r^.resultType := resultTy;
  res := newDecl(dkVar, r^.ident, r^.ident.loc);
  res^.varSpec := r^.resultSpec;
  res^.varKind := vkResult;
  res^.varType := resultTy;
  res^.owner := r;
  r^.resultVar := res;
end;

function sameSignature(a, b: PDecl): boolean;
var
  i: longint;
  p, q: PDecl;
begin
  if a^.params.count <> b^.params.count then
    exit(false);
  for i := 0 to a^.params.count - 1 do
  begin
    p := PDecl(listGet(a^.params, i));
    q := PDecl(listGet(b^.params, i));
    if (p^.varKind <> q^.varKind) or (p^.varType <> q^.varType) then
      exit(false);
  end;
  sameSignature := true;
end;

procedure checkRoutineBody(r: PDecl); forward;

{ External (C) routines: at the outermost level, with C-compatible types. }
procedure checkExternal(r: PDecl);

  function cType(t: PType): boolean;
  begin
    cType := isError(t) or (t^.kind = tyReal) or (t^.kind = tyPointer) or
             (isOrdinal(t) and (getBase(t)^.kind <> tyBoolean));
  end;

var
  i: longint;
  p: PDecl;
begin
  if curRoutine <> nil then
  begin
    ms('external routine ''');
    maN(r^.ident.spelling);
    ma(''' must be declared at program or unit level');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    if (p^.varType <> nil) and (p^.varKind <> vkVarParam) and not cType(p^.varType) then
    begin
      ms('parameter ''');
      maN(p^.ident.spelling);
      ma(''' of external routine ''');
      maN(r^.ident.spelling);
      ma(''' has type ');
      maT(p^.varType);
      ma('; use integer, real, char, pointer or var parameters');
      errR(p^.loc, identRange(p^.ident));
    end;
  end;
  if r^.isFunction and not cType(r^.resultType) then
  begin
    ms('external function ''');
    maN(r^.ident.spelling);
    ma(''' cannot return ');
    maT(r^.resultType);
    errR(r^.ident.loc, identRange(r^.ident));
  end;
end;

procedure declareRoutine(r: PDecl);
var
  fwd, p: PDecl;
  i: longint;
  rt: PType;
begin
  r^.parent := curRoutine;
  if curRoutine <> nil then
    r^.level := curRoutine^.level + 1
  else
    r^.level := 1;
  { the definition of an earlier forward declaration? }
  fwd := scopeLookupOwn(cur, r^.ident.name);
  if (fwd <> nil) and (fwd^.kind <> dkRoutine) then
    fwd := nil;
  if (fwd = nil) and (curUnitInterface <> nil) and (curRoutine = nil) then
  begin
    fwd := scopeLookupOwn(curUnitInterface, r^.ident.name);
    if (fwd <> nil) and (fwd^.kind <> dkRoutine) then
      fwd := nil;
  end;
  if (fwd <> nil) and fwd^.isForward and (fwd^.body = nil) and (r^.body <> nil) then
  begin
    if fwd^.isFunction <> r^.isFunction then
    begin
      ms('''');
      maN(r^.ident.spelling);
      ma(''' was declared forward as a ' + kindName(fwd));
      errR(r^.ident.loc, identRange(r^.ident));
      ms('forward declaration is here');
      note(fwd^.loc);
      exit;
    end;
    { ISO omits the parameter list here; if repeated, it must match }
    if r^.hasParamList or (r^.resultSpec <> nil) then
    begin
      for i := 0 to r^.params.count - 1 do
      begin
        p := PDecl(listGet(r^.params, i));
        p^.varType := resolveType(p^.varSpec, nil);
      end;
      rt := nil;
      if r^.resultSpec <> nil then
        rt := resolveType(r^.resultSpec, nil);
      if not sameSignature(fwd, r) or ((rt <> nil) and (rt <> fwd^.resultType)) then
      begin
        ms('heading of ''');
        maN(r^.ident.spelling);
        ma(''' does not match its forward declaration');
        errR(r^.ident.loc, identRange(r^.ident));
        ms('forward declaration is here');
        note(fwd^.loc);
      end;
    end;
    fwd^.body := r^.body;
    r^.canonical := fwd;
    checkRoutineBody(fwd);
    exit;
  end;

  resolveSignature(r);
  declare(r);
  if r^.isExternal then
    checkExternal(r);
  if r^.body <> nil then
    checkRoutineBody(r);
end;

{ A string constant passed where a pchar is expected (Turbo/FPC). }
function isPCharLiteral(target: PType; value: PNode): boolean;
var
  c: PDecl;
begin
  if isISO or (target^.kind <> tyPointer) or target^.untyped or
     (target^.pointee = nil) or (target^.pointee^.kind <> tyChar) then
    exit(false);
  if value^.kind = nkStringLit then
    exit(true);
  if value^.kind <> nkNameRef then
    exit(false);
  c := value^.refDecl;
  isPCharLiteral := (c <> nil) and (c^.kind = dkConst) and
                    ((c^.constValue.kind = ckString) or
                     ((c^.constType <> nil) and (getBase(c^.constType)^.kind = tyChar)));
end;

procedure resolvePendingPointers;
var
  i: longint;
  pp: PPendingPointer;
  d: PDecl;
  id: TIdent;
begin
  for i := 0 to pendingPointers.count - 1 do
  begin
    pp := PPendingPointer(listGet(pendingPointers, i));
    id := pp^.spec^.tname;
    d := lookup(id.name);
    if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) then
    begin
      pp^.ptr^.pointee := d^.declType;
      continue;
    end;
    if d <> nil then
    begin
      ms('''');
      maN(id.spelling);
      ma(''' is a ' + kindName(d) + ', not a type');
      errR(id.loc, identRange(id));
    end
    else
    begin
      ms('unknown type name ''');
      maN(id.spelling);
      ma('''');
      maDidYouMean(id.name);
      errR(id.loc, identRange(id));
    end;
    pp^.ptr^.pointee := errorType;
  end;
  listClear(pendingPointers);
end;

procedure declareAll(b: PBlock);
var
  i: longint;
  d, prev: PDecl;
begin
  for i := 0 to b^.decls.count - 1 do
  begin
    if diagShouldStop then
      exit;
    d := PDecl(listGet(b^.decls, i));
    if d^.kind <> dkType then
      resolvePendingPointers;
    case d^.kind of
      dkConst: declareConst(d);
      dkType: declareType(d);
      dkVar: declareVar(d);
      dkRoutine: declareRoutine(d);
      dkLabel:
        begin
          d^.labelOwner := curRoutine;
          prev := scopeInsert(cur, labelKey(d^.ident.name), d);
          if prev <> nil then
          begin
            ms('label ');
            maN(d^.ident.name);
            ma(' is declared twice');
            errR(d^.loc, identRange(d^.ident));
            ms('previous declaration is here');
            note(prev^.loc);
          end;
        end;
    else
      ;
    end;
  end;
  resolvePendingPointers;
end;

procedure visitStmt(s: PNode); forward;

procedure finishBlock(b: PBlock; warnUnused: boolean);
var
  i: longint;
  d: PDecl;
  savedBody: PNode;
begin
  { ISO 7185 6.1.4: every forward declaration needs a definition in the
    same block }
  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    if (d^.kind = dkRoutine) and d^.isForward and (d^.body = nil) then
    begin
      ms('''');
      maN(d^.ident.spelling);
      ma(''' is declared forward but never defined');
      errR(d^.ident.loc, identRange(d^.ident));
    end;
  end;
  savedBody := routineBody;
  routineBody := b^.body;
  visitStmt(b^.body);
  routineBody := savedBody;

  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    if warnUnused and (d^.kind = dkVar) and not d^.isUsed then
    begin
      ms('variable ''');
      maN(d^.ident.spelling);
      ma(''' is declared but never used [-Wunused-variable]');
      if diagWarningEnabled('unused-variable') then
        diagReportBuf(sevWarning, d^.loc, msg, identRange(d^.ident), noRange);
    end;
  end;

  { ISO 7185 6.8.1: every declared label prefixes exactly one statement }
  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    if (d^.kind = dkLabel) and (d^.labelTarget = nil) and
       (scopeLookupLocal(cur, labelKey(d^.ident.name)) = d) then { not a duplicate }
    begin
      ms('label ');
      maN(d^.ident.name);
      ma(' is declared but does not label any statement');
      errR(d^.loc, identRange(d^.ident));
    end;
  end;
end;

procedure checkBlock(b: PBlock);
begin
  declareAll(b);
  finishBlock(b, true);
end;

procedure checkRoutineBody(r: PDecl);
var
  savedScope: PScope;
  savedRoutine, p, prev: PDecl;
  i: longint;
begin
  savedScope := cur;
  savedRoutine := curRoutine;
  cur := newScope(cur);
  curRoutine := r;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    prev := scopeInsert(cur, p^.ident.name, p);
    if prev <> nil then
    begin
      ms('duplicate parameter name ''');
      maN(p^.ident.spelling);
      ma('''');
      errR(p^.loc, identRange(p^.ident));
      ms('previous parameter is here');
      note(prev^.loc);
    end;
  end;
  checkBlock(r^.body);
  if r^.isFunction and not r^.resultAssigned and not isError(r^.resultType) then
  begin
    ms('function ''');
    maN(r^.ident.spelling);
    ma(''' never assigns its result');
    diagReportBuf(sevWarning, r^.ident.loc, msg, identRange(r^.ident), noRange);
  end;
  cur := savedScope;
  curRoutine := savedRoutine;
end;

procedure checkGotos;
var
  i: longint;
  g: PPendingGoto;
  l: PDecl;
  ok: boolean;
begin
  for i := 0 to pendingGotos.count - 1 do
  begin
    g := PPendingGoto(listGet(pendingGotos, i));
    l := g^.stmt^.lblDecl;
    if (l = nil) or (l^.labelTarget = nil) then
      continue;
    if l^.labelOwner = g^.from then
      ok := containsPtr(g^.containers, l^.labelTarget) or
            ((l^.labelSequence <> nil) and containsPtr(g^.containers, l^.labelSequence))
    else
    begin
      { a goto out of a nested routine may only target the outermost
        statement sequence of the enclosing block }
      ok := l^.labelTopLevel;
      if ok then
      begin
        l^.labelNonLocal := true;
        if l^.labelIndex = 0 then
        begin
          if l^.labelOwner <> nil then
          begin
            l^.labelOwner^.nonLocalLabels := l^.labelOwner^.nonLocalLabels + 1;
            l^.labelIndex := l^.labelOwner^.nonLocalLabels;
          end
          else
          begin
            programLabelCount := programLabelCount + 1;
            l^.labelIndex := programLabelCount;
          end;
        end;
      end;
    end;
    if not ok then
    begin
      ms('''goto ');
      maN(l^.ident.name);
      ma(''' jumps into a statement that does not contain it');
      errR(g^.stmt^.b, nodeRange(g^.stmt));
      ms('label ');
      maN(l^.ident.name);
      ma(' is here; it can only be reached from its own statement sequence');
      note(l^.labelTarget^.b);
    end;
  end;
end;

procedure checkProgramParams(p: PDecl);
var
  seen: TIntMap;
  i: longint;
  id, prev: PIdent;
begin
  mapInit(seen);
  for i := 0 to p^.progParams.count - 1 do
  begin
    id := PIdent(listGet(p^.progParams, i));
    prev := PIdent(mapGet(seen, id^.name^.id));
    if prev <> nil then
    begin
      ms('duplicate program parameter ''');
      maN(id^.spelling);
      ma('''');
      errR(id^.loc, identRange(id^));
      ms('previous occurrence is here');
      note(prev^.loc);
      continue;
    end;
    mapPut(seen, id^.name^.id, id);
  end;
end;

{ ISO 7185 6.10: program parameters other than input/output are file
  variables of the program block, bound to the command-line arguments. }
procedure checkProgramParamDecls(p: PDecl);
var
  i: longint;
  id: PIdent;
  v: PDecl;
begin
  for i := 0 to p^.progParams.count - 1 do
  begin
    id := PIdent(listGet(p^.progParams, i));
    if nameEquals(id^.name, 'input') or nameEquals(id^.name, 'output') then
      continue;
    v := scopeLookupLocal(cur, id^.name);
    if (v <> nil) and (v^.kind <> dkVar) then
      v := nil;
    if v <> nil then
      v^.isUsed := true; { bound to a command-line argument }
    if (v <> nil) and (v^.varType <> nil) and
       ((v^.varType^.kind = tyFile) or isError(v^.varType)) then
      continue;
    ms('program parameter ''');
    maN(id^.spelling);
    ma(''' must be declared as a file variable in the program');
    errR(id^.loc, identRange(id^));
  end;
end;

{ --- Statement helpers --- }

procedure checkCondition(cond: PNode; const keyword: string);
var
  t: PType;
begin
  t := check(cond);
  if not isError(t) and (getBase(t)^.kind <> tyBoolean) then
  begin
    ms('condition of ''' + keyword + ''' must be of type ''boolean'', not ');
    maT(t);
    errR(cond^.b, nodeRange(cond));
  end;
end;

{ Reports an assignment to the control variable of an enclosing for
  statement; true if one was reported. }
function checkNotForControlVar(target: PNode): boolean;
var
  i: longint;
  f: PNode;
begin
  if (target^.kind <> nkNameRef) or (target^.refDecl = nil) then
    exit(false);
  for i := 0 to activeFors.count - 1 do
  begin
    f := PNode(listGet(activeFors, i));
    if f^.forVar^.refDecl <> target^.refDecl then
      continue;
    ms('cannot assign to ''');
    maN(target^.ref.spelling);
    ma(''' inside a ''for'' loop that it controls');
    errR(target^.b, nodeRange(target));
    ms('''for'' loop is here');
    note(f^.b);
    exit(true);
  end;
  checkNotForControlVar := false;
end;

{ Checks one case or variant label against the selector / tag type. }
function checkLabel(l: PCaseLabel; selTy: PType; const what: string): boolean;
var
  ok: boolean;
  k: longint;
  e: PNode;
  c: TConstant;
  bd: TOrdinalBounds;
begin
  ok := true;
  for k := 1 to 2 do
  begin
    if k = 1 then
      e := l^.lo
    else
      e := l^.hi;
    if e = nil then
      continue;
    if not evaluateConstant(e, what, c) then
    begin
      ok := false;
      continue;
    end;
    if not isError(selTy) and not areCompatible(c.ty, selTy) then
    begin
      ms(what + ' of type ');
      maT(c.ty);
      if what = 'case label' then
        ma(' does not match selector type ')
      else
        ma(' does not match tag type ');
      maT(selTy);
      errR(e^.b, nodeRange(e));
      ok := false;
      continue;
    end;
    if k = 1 then
      l^.loValue := c.value.intVal
    else
      l^.hiValue := c.value.intVal;
  end;
  if l^.hi = nil then
    l^.hiValue := l^.loValue;
  if ok and (l^.loValue > l^.hiValue) then
  begin
    ms('case label range is empty');
    errR(l^.lo^.b, caseLabelRange(l));
    ok := false;
  end;
  if ok and not isError(selTy) then
  begin
    bd := getOrdinalBounds(selTy);
    if (l^.loValue < bd.lo) or (l^.hiValue > bd.hi) then
    begin
      ms(what + ' is outside the range of type ');
      maT(selTy);
      errR(l^.lo^.b, caseLabelRange(l));
      ok := false;
    end;
  end;
  checkLabel := ok;
end;

{ ISO 7185 6.6.3.6: congruent parameter lists and results }
function congruent(const a: TPtrList; aResult: PType; const b: TPtrList;
                   bResult: PType): boolean;
var
  i: longint;
  pa, pb: PDecl;
  ta, tb: PType;
begin
  if (a.count <> b.count) or ((aResult = nil) <> (bResult = nil)) or
     ((aResult <> nil) and (aResult <> bResult)) then
    exit(false);
  for i := 0 to a.count - 1 do
  begin
    pa := PDecl(listGet(a, i));
    pb := PDecl(listGet(b, i));
    if pa^.varKind <> pb^.varKind then
      exit(false);
    ta := pa^.varType;
    tb := pb^.varType;
    if (ta <> nil) and (tb <> nil) and (ta^.kind = tyRoutine) and (tb^.kind = tyRoutine) then
    begin
      if not congruent(ta^.routineParams, ta^.routineResult, tb^.routineParams,
                       tb^.routineResult) then
        exit(false);
    end
    else if ta <> tb then
      exit(false);
  end;
  congruent := true;
end;

{ An argument for a procedural parameter: the name of a routine (or of
  another procedural parameter) with a congruent heading. }
procedure checkRoutineArg(arg: PNode; p: PDecl; formal: PType);
var
  kind: string;
  d: PDecl;
  params: TPtrList;
  res: PType;
  found: boolean;
begin
  if formal^.routineResult <> nil then
    kind := 'function'
  else
    kind := 'procedure';
  d := nil;
  if arg^.kind = nkNameRef then
    d := lookup(arg^.ref.name);
  listInit(params);
  res := nil;
  found := false;
  if (d <> nil) and (d^.kind = dkRoutine) then
  begin
    params := d^.params;
    if d^.isFunction then
      res := d^.resultType;
    found := true;
  end
  else if (d <> nil) and (d^.kind = dkVar) and (d^.varType <> nil) and
          (d^.varType^.kind = tyRoutine) then
  begin
    params := d^.varType^.routineParams;
    res := d^.varType^.routineResult;
    found := true;
  end;
  if not found then
  begin
    ms('argument for ' + kind + ' parameter ''');
    maN(p^.ident.spelling);
    ma(''' must be the name of a ' + kind);
    if (d <> nil) and (d^.kind = dkBuiltin) then
      ma(' (required procedures and functions cannot be passed)');
    errR(arg^.b, nodeRange(arg));
    exit;
  end;
  arg^.refDecl := d;
  arg^.typ := p^.varType;
  if not congruent(formal^.routineParams, formal^.routineResult, params, res) then
  begin
    ms('''');
    maN(arg^.ref.spelling);
    ma(''' does not match the heading of parameter ''');
    maN(p^.ident.spelling);
    ma(''' (');
    maT(formal);
    ma(')');
    errR(arg^.b, nodeRange(arg));
  end;
end;

{ Checks the arguments of a call. }
procedure checkCallArgs(const params: TPtrList; calleeDecl: PDecl;
                        const callee: TIdent; const args: TPtrList;
                        callRange: TRange);
var
  i: longint;
  arg: PNode;
  p: PDecl;
  at, pt: PType;
begin
  if args.count <> params.count then
  begin
    if args.count < params.count then
      ms('too few')
    else
      ms('too many');
    ma(' arguments in call to ''');
    maN(callee.spelling);
    ma(''': expected ');
    maI(params.count);
    ma(', got ');
    maI(args.count);
    errR(callee.loc, callRange);
    if calleeDecl <> nil then
      noteDeclaredHere(calleeDecl);
    for i := 0 to args.count - 1 do
      if PNode(listGet(args, i))^.kind <> nkFormatted then
        check(PNode(listGet(args, i)));
    exit;
  end;
  for i := 0 to args.count - 1 do
  begin
    arg := PNode(listGet(args, i));
    p := PDecl(listGet(params, i));
    if arg^.kind = nkFormatted then
    begin
      ms('field widths are only allowed in ''write'' and ''writeln''');
      errR(arg^.fmtWidth^.b, nodeRange(arg^.fmtWidth));
      continue;
    end;
    if (p^.varType <> nil) and (p^.varType^.kind = tyRoutine) then
    begin
      checkRoutineArg(arg, p, p^.varType);
      continue;
    end;
    at := check(arg);
    pt := p^.varType;
    if isError(at) or (pt = nil) or isError(pt) then
      continue;
    if p^.varKind = vkVarParam then
    begin
      if not isVariableReference(arg) then
      begin
        ms('argument for ''var'' parameter ''');
        maN(p^.ident.spelling);
        ma(''' must be a variable');
        errR(arg^.b, nodeRange(arg));
        continue;
      end;
      { ISO 7185 6.6.3.3: the same type }
      if not checkModifiable(arg) then
        continue;
      if at <> pt then
      begin
        ms('argument of type ');
        maT(at);
        ma(' for ''var'' parameter ''');
        maN(p^.ident.spelling);
        ma(''' must have exactly type ');
        maT(pt);
        errR(arg^.b, nodeRange(arg));
      end
      else
        checkNotForControlVar(arg);
      continue;
    end;
    if not isAssignable(pt, at) and not isPCharLiteral(pt, arg) then
    begin
      ms('cannot pass a value of type ');
      maT(at);
      ma(' to parameter ''');
      maN(p^.ident.spelling);
      ma(''' of type ');
      maT(pt);
      errR(arg^.b, nodeRange(arg));
    end;
  end;
end;

{$I pcsemastmt.inc}

{ --- Programs and units --- }

{ Makes the interfaces of the used units visible in s. }
procedure importUnits(s: PScope; const usesList: TPtrList);
var
  i: longint;
  iface: PScope;
begin
  for i := 0 to usesList.count - 1 do
  begin
    iface := PScope(mapGet(unitInterfaces, PIdent(listGet(usesList, i))^.name^.id));
    if iface <> nil then
      scopeAddImport(s, iface);
    { missing units are reported by the driver }
  end;
end;

procedure checkUnit(u: PDecl);
var
  iface, impl: PScope;
  i: longint;
  r: PDecl;
begin
  iface := newScope(standard);
  importUnits(iface, u^.ifaceUses);
  cur := iface;
  declareAll(u^.iface);

  impl := newScope(iface);
  importUnits(impl, u^.implUses);
  cur := impl;
  curUnitInterface := iface;
  declareAll(u^.impl);
  for i := 0 to u^.iface^.decls.count - 1 do
  begin
    r := PDecl(listGet(u^.iface^.decls, i));
    if (r^.kind = dkRoutine) and (r^.body = nil) and not r^.isExternal then
    begin
      ms('''');
      maN(r^.ident.spelling);
      ma(''' is declared in the interface of unit ''');
      maN(u^.ident.spelling);
      ma(''' but not implemented');
      errR(r^.ident.loc, identRange(r^.ident));
    end;
  end;
  finishBlock(u^.impl, true);
  curUnitInterface := nil;
  { interface variables may be used by other units: never unused }
  for i := 0 to u^.iface^.decls.count - 1 do
  begin
    r := PDecl(listGet(u^.iface^.decls, i));
    r^.isUsed := true;
  end;
  mapPut(unitInterfaces, u^.ident.name^.id, iface);
  cur := standard;
end;

function semaCheck(prog: PDecl; const units: TPtrList; m: TLangMode): boolean;
var
  errorsBefore, i: longint;
  programScope: PScope;
begin
  mode := m;
  errorsBefore := diagNumErrors;
  bufInit(msg);
  listInit(activeFors);
  listInit(pendingPointers);
  listInit(containers);
  listInit(pendingGotos);
  mapInit(unitInterfaces);
  loopDepth := 0;
  directSequence := nil;
  routineBody := nil;
  curRoutine := nil;
  curUnitInterface := nil;
  programLabelCount := 0;
  standard := newScope(nil);
  cur := standard;
  declareStandardScope;

  for i := 0 to units.count - 1 do
    checkUnit(PDecl(listGet(units, i)));
  checkProgramParams(prog);
  programScope := newScope(standard);
  importUnits(programScope, prog^.progUses);
  cur := programScope;
  checkBlock(prog^.progBlock);
  checkProgramParamDecls(prog);
  checkGotos;
  cur := standard;
  semaCheck := (diagNumErrors = errorsBefore) and not diagShouldStop;
end;

end.
