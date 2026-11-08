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
function semaCheck(prog: PDecl; const units: TPtrList; m: TLangMode;
                   separateUnits: boolean): boolean;
{ Checks unit u, compiled on its own; deps (dependency order) are the
  units it uses, directly or not, of which only the interfaces matter. }
function semaCheckUnit(u: PDecl; const deps: TPtrList; m: TLangMode): boolean;

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

  { objfpc: an array property used without (all) its indexes: the base of
    the nkIndex being checked (node), with the property, the chain's root
    and the indexes so far }
  PPendingIndex = ^TPendingIndex;
  TPendingIndex = record
    node: PNode;
    prop: PDecl;
    root: PNode;
    args: TPtrList;
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
  { objfpc: the program or unit being checked (private members are visible
    in the module declaring their class); the target of the assignment
    being checked (a property there is written, not read); the predeclared
    class TObject; the classes declared forward (their type declarations)
    in the current type section }
  curModule: pointer;
  curAssignTarget: PNode;
  tobjectType: PType;
  pendingClasses: TPtrList;
  pendingInterfaces: TPtrList; { interfaces declared forward (type decls) }
  { exceptions (objfpc): the predeclared class Exception; the exception
    handlers around the statement being checked (a bare raise needs one);
    loopDepth on entry to the innermost enclosing finally part, or -1 (exit,
    and break/continue out of it, are not allowed there). objfpc has no
    gotos, so none can leave a try statement. }
  exceptionType: PType;
  { class of C for each class C (by type id): the type of C used as a
    value and of Self in class methods }
  classRefs: TIntMap;
  handlerDepth: longint;
  finallyLoopDepth: longint;
  { the procedural type an @routine is assigned to (picks an overload) }
  addrOfTarget: PType;
  { the base of the nkIndex being checked: an array property may appear
    there without its indexes; the pending ones (of PPendingIndex) }
  indexBase: PNode;
  pendingIndexes: TPtrList;

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
    dkProperty: kindName := 'property';
    dkRoutine:
      if d^.methodKind = mtConstructor then
        kindName := 'constructor'
      else if d^.methodKind = mtDestructor then
        kindName := 'destructor'
      else if d^.isFunction then
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

{ The identifier of a predeclared member: the lower-case name, spelled as
  in Free Pascal. }
function memberIdent(const spelling: string): TIdent;
var
  id: TIdent;
begin
  id.name := intern(lowerStr(spelling));
  id.spelling := intern(spelling);
  id.loc := noLoc;
  memberIdent := id;
end;

function newTObjectMethod(const spelling: string; mk: TMethodKind;
                          res: PType; om: TObjMethod): PDecl;
var
  r: PDecl;
begin
  r := newDecl(dkRoutine, memberIdent(spelling), noLoc);
  r^.isFunction := res <> nil;
  listInit(r^.params);
  r^.level := 1;
  r^.methodKind := mk;
  r^.methodClassName := noIdent;
  r^.ownerClass := tobjectType;
  r^.objMethod := om;
  r^.resultType := res;
  r^.vmtIndex := -1;
  newTObjectMethod := r;
end;

{ class TObject: constructor Create; destructor Destroy; virtual;
  procedure Free; function ClassName: string; (bodies in code generation
  and the runtime). Destroy is virtual method 0. }
{ class of c (anonymous, shared) }
function classRefOf(c: PType): PType;
var
  r: PType;
begin
  r := PType(mapGet(classRefs, c^.id));
  if r = nil then
  begin
    r := newClassRefType(c);
    mapPut(classRefs, c^.id, r);
  end;
  classRefOf := r;
end;

{ a class function of TObject: Self is a class }
function newTObjectClassFn(const spelling: string; res: PType; om: TObjMethod): PDecl;
var
  r: PDecl;
begin
  r := newTObjectMethod(spelling, mtMethod, res, om);
  r^.isClassMethod := true;
  newTObjectClassFn := r;
end;

procedure declareTObject;
var
  members, fields, vtable: TPtrList;
  destroy, d, inheritsFrom_, aClass: PDecl;
  tclass, named: PType;
begin
  tobjectType := newClassType;
  setDeclName(tobjectType, intern('TObject'));
  listInit(members);
  listInit(fields);
  listInit(vtable);
  destroy := newTObjectMethod('Destroy', mtDestructor, nil, obDestroy);
  destroy^.isVirtual := true;
  destroy^.vmtIndex := 0;
  { TClass = class of TObject }
  tclass := classRefOf(tobjectType);
  aClass := newDecl(dkVar, memberIdent('AClass'), noLoc);
  aClass^.varKind := vkValueParam;
  aClass^.varType := tclass;
  inheritsFrom_ := newTObjectClassFn('InheritsFrom', tyBooleanT, obInheritsFrom);
  inheritsFrom_^.hasParamList := true;
  listAdd(inheritsFrom_^.params, aClass);
  listAdd(members, newTObjectMethod('Create', mtConstructor, nil, obCreate));
  listAdd(members, destroy);
  listAdd(members, newTObjectMethod('Free', mtMethod, nil, obFree));
  listAdd(members, newTObjectClassFn('ClassName', shortStringType(255), obClassName));
  listAdd(members, newTObjectMethod('ClassType', mtMethod, tclass, obClassType));
  listAdd(members, newTObjectClassFn('ClassParent', tclass, obClassParent));
  listAdd(members, inheritsFrom_);
  listAdd(members, newTObjectClassFn('InstanceSize', tyInt64T, obInstanceSize));
  listAdd(vtable, destroy);
  defineClass(tobjectType, nil, members, fields, vtable, nil);
  d := newDecl(dkType, memberIdent('TObject'), noLoc);
  d^.declType := tobjectType;
  scopeInsert(standard, d^.ident.name, d);
  named := newClassRefType(tobjectType);
  setDeclName(named, intern('TClass'));
  d := newDecl(dkType, memberIdent('TClass'), noLoc);
  d^.declType := named;
  scopeInsert(standard, d^.ident.name, d);
end;

{ class Exception (Free Pascal's SysUtils): private FMessage: string;
  constructor Create(const msg: string); property Message: string read
  FMessage write FMessage. Its VMT is in the runtime, which prints the
  message of an unhandled exception. }
{ A predeclared name = class(parent); (its VMT is in the runtime). }
function declareExceptionClass(const name: string; parent: PType): PType;
var
  c: PType;
  members: TPtrList;
  d: PDecl;
begin
  c := newClassType;
  setDeclName(c, intern(name));
  listInit(members);
  defineClass(c, parent, members, parent^.classAllFields, parent^.classVTable, nil);
  d := newDecl(dkType, memberIdent(name), noLoc);
  d^.declType := c;
  scopeInsert(standard, d^.ident.name, d);
  declareExceptionClass := c;
end;

procedure declareException;
var
  members, fields: TPtrList;
  msgField, param, create, message, d: PDecl;
  i: longint;
  external_, intError: PType;
begin
  exceptionType := newClassType;
  setDeclName(exceptionType, intern('Exception'));
  msgField := newDecl(dkField, memberIdent('FMessage'), noLoc);
  msgField^.fieldType := shortStringType(255);
  msgField^.vis := vsPrivate;
  param := newDecl(dkVar, memberIdent('msg'), noLoc);
  param^.varKind := vkConstParam;
  param^.varType := shortStringType(255);
  create := newTObjectMethod('Create', mtConstructor, nil, obExceptionCreate);
  create^.ownerClass := exceptionType;
  create^.hasParamList := true;
  listAdd(create^.params, param);
  message := newDecl(dkProperty, memberIdent('Message'), noLoc);
  message^.propType := shortStringType(255);
  message^.readName := memberIdent('FMessage');
  message^.writeName := memberIdent('FMessage');
  message^.readDecl := msgField;
  message^.writeDecl := msgField;
  listInit(members);
  listAdd(members, msgField);
  listAdd(members, create);
  listAdd(members, message);
  listInit(fields);
  for i := 0 to tobjectType^.classAllFields.count - 1 do
    listAdd(fields, listGet(tobjectType^.classAllFields, i));
  listAdd(fields, msgField);
  defineClass(exceptionType, tobjectType, members, fields,
              tobjectType^.classVTable, nil);
  d := newDecl(dkType, memberIdent('Exception'), noLoc);
  d^.declType := exceptionType;
  scopeInsert(standard, d^.ident.name, d);
  { the exception for integer div and mod by zero, as in SysUtils }
  external_ := declareExceptionClass('EExternal', exceptionType);
  intError := declareExceptionClass('EIntError', external_);
  external_ := declareExceptionClass('EDivByZero', intError);
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
  if mode = lmObjFPC then
  begin
    declareTObject;
    declareException;
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
  if ((a^.kind = tyClass) and (b^.kind = tyNil)) or
     ((a^.kind = tyNil) and (b^.kind = tyClass)) then
    exit(true);
  if ((a^.kind = tyClassRef) and (b^.kind = tyNil)) or
     ((a^.kind = tyNil) and (b^.kind = tyClassRef)) then
    exit(true);
  if (a^.kind = tyClassRef) and (b^.kind = tyClassRef) then { related classes }
    exit(inheritsFrom(a^.refClass, b^.refClass) or inheritsFrom(b^.refClass, a^.refClass));
  if ((a^.kind = tyInterface) and (b^.kind = tyNil)) or
     ((a^.kind = tyNil) and (b^.kind = tyInterface)) then
    exit(true);
  if (a^.kind = tyInterface) and (b^.kind = tyInterface) then { related }
    exit(interfaceInheritsFrom(a, b) or interfaceInheritsFrom(b, a));
  if (a^.kind = tyClass) and (b^.kind = tyClass) then { related classes (objfpc) }
    exit(inheritsFrom(a, b) or inheritsFrom(b, a));
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
  { classes (objfpc): an object of the class or a descendant, or nil; any
    object fits an untyped pointer }
  if target^.kind = tyClass then
    exit((value^.kind = tyNil) or
         ((value^.kind = tyClass) and inheritsFrom(value, target)));
  { interfaces: nil, a descendant interface, an object of a class that
    implements it (listed by the class or an ancestor) }
  if target^.kind = tyInterface then
  begin
    if value^.kind = tyNil then
      exit(true);
    if value^.kind = tyInterface then
      exit(interfaceInheritsFrom(value, target));
    if value^.kind = tyClass then
      exit(findInterface(value, target) <> nil);
    exit(false);
  end;
  { class references: the class or a descendant, or nil }
  if target^.kind = tyClassRef then
    exit((value^.kind = tyNil) or
         ((value^.kind = tyClassRef) and inheritsFrom(value^.refClass, target^.refClass)));
  if (target^.kind = tyPointer) and (value^.kind = tyClass) then
    exit(target^.untyped);
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

{ Is e (already checked) a value of a class type? }
function isObject(e: PNode): boolean;
begin
  isObject := (e^.typ <> nil) and (e^.typ^.kind = tyClass);
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
    nkField:
      { obj.f is a variable like p^.f, whatever obj is (objfpc) }
      if (e^.fieldDecl <> nil) and (e^.fieldDecl^.kind = dkVar) then
        isVariableReference := true { a class variable }
      else if isObject(e^.base) then
        isVariableReference := (e^.fieldDecl <> nil) and
                               (e^.fieldDecl^.kind = dkField)
      else
        isVariableReference := isVariableReference(e^.base);
    nkIndex:
      isVariableReference := (e^.idxProp = nil) and isVariableReference(e^.base);
    nkDeref:
      isVariableReference := true; { p^ is a variable even for a call's result }
  else
    isVariableReference := false;
  end;
end;

{ The const parameter a designator is part of (x, x[i], x.f; not x^ or
  obj.f). }
function constParamRoot(e: PNode): PDecl;
begin
  while true do
    case e^.kind of
      nkIndex:
        begin
          if e^.idxProp <> nil then
            exit(nil);
          e := e^.base;
        end;
      nkField:
        begin
          if isObject(e^.base) or
             ((e^.fieldDecl <> nil) and (e^.fieldDecl^.kind = dkVar)) then
            exit(nil);
          e := e^.base;
        end;
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
    tsClass:
      begin
        ms('a class type must be declared in a ''type'' section');
        errR(ts^.b, specRange(ts));
      end;
    tsClassRef: { class of TFoo (TFoo may be forward) }
      begin
        d := lookup(ts^.tname.name);
        if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) and
           isError(d^.declType) then
          exit;
        if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) and
           (d^.declType^.kind = tyClass) then
          exit(newClassRefType(d^.declType));
        if d = nil then
        begin
          ms('unknown class name ''');
          maN(ts^.tname.spelling);
          ma('''');
          maDidYouMean(ts^.tname.name);
          errR(ts^.tname.loc, identRange(ts^.tname));
        end
        else
        begin
          ms('''');
          maN(ts^.tname.spelling);
          ma(''' is not a class type');
          errR(ts^.tname.loc, identRange(ts^.tname));
          noteDeclaredHere(d);
        end;
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

procedure declareClass(td: PDecl); forward;
procedure declareInterface(td: PDecl); forward;

procedure declareType(td: PDecl);
begin
  if td^.typeSpec^.kind = tsClass then
  begin
    declareClass(td);
    exit;
  end;
  if td^.typeSpec^.kind = tsInterface then
  begin
    declareInterface(td);
    exit;
  end;
  td^.declType := resolveType(td^.typeSpec, td^.ident.spelling);
  declare(td);
end;

{ --- Classes (objfpc) --- }

procedure resolveSignature(r: PDecl); forward;
procedure checkRoutineBody(r: PDecl); forward;
function sameSignature(a, b: PDecl): boolean; forward;
function addOverload(first, r: PDecl): boolean; forward;

{ 'TFoo.Bar' (without quotes) }
procedure maQualified(c: PType; const id: TIdent);
begin
  bufAddTypeName(msg, c);
  ma('.');
  maN(id.spelling);
end;

procedure defineClassMembers(c: PType; ts: PTypeSpec); forward;

{ TFoo = class ... end;  /  TFoo = class; }
procedure declareClass(td: PDecl);
var
  ts: PTypeSpec;
  c: PType;
  i, j: longint;
  pd: PDecl;
begin
  ts := td^.typeSpec;
  if curRoutine <> nil then
  begin
    ms('class ''');
    maN(td^.ident.spelling);
    ma(''' must be declared at program or unit level');
    errR(td^.ident.loc, identRange(td^.ident));
    td^.declType := errorType;
    ts^.resolved := errorType;
    declare(td);
    exit;
  end;
  { the definition of a class declared forward in this type section? }
  if not ts^.isForwardClass then
    for i := 0 to pendingClasses.count - 1 do
    begin
      pd := PDecl(listGet(pendingClasses, i));
      if (pd^.ident.name = td^.ident.name) and
         (scopeLookupOwn(cur, td^.ident.name) = pd) then
      begin
        { remove it, keeping the others in order }
        for j := i to pendingClasses.count - 2 do
          listSet(pendingClasses, j, listGet(pendingClasses, j + 1));
        listPop(pendingClasses);
        c := pd^.declType;
        td^.declType := c;
        ts^.resolved := c;
        defineClassMembers(c, ts);
        exit;
      end;
    end;
  c := newClassType;
  setDeclName(c, td^.ident.spelling);
  c^.classModule := curModule;
  td^.declType := c;
  ts^.resolved := c;
  declare(td); { before the members, which may refer to the class }
  if ts^.isForwardClass then
  begin
    listAdd(pendingClasses, td);
    exit;
  end;
  defineClassMembers(c, ts);
end;

{ A forward class must be defined in the same type section. }
procedure checkPendingClasses;
var
  i: longint;
  td: PDecl;
  members: TPtrList;
begin
  for i := 0 to pendingClasses.count - 1 do
  begin
    td := PDecl(listGet(pendingClasses, i));
    ms('class ''');
    maN(td^.ident.spelling);
    ma(''' is declared forward but never defined');
    errR(td^.ident.loc, identRange(td^.ident));
    listInit(members);
    defineClass(td^.declType, tobjectType, members,
                tobjectType^.classAllFields, tobjectType^.classVTable,
                curModule);
  end;
  listClear(pendingClasses);
  for i := 0 to pendingInterfaces.count - 1 do
  begin
    td := PDecl(listGet(pendingInterfaces, i));
    ms('interface ''');
    maN(td^.ident.spelling);
    ma(''' is declared forward but never defined');
    errR(td^.ident.loc, identRange(td^.ident));
    listInit(members);
    defineInterface(td^.declType, nil, members, members, curModule);
  end;
  listClear(pendingInterfaces);
end;

{ The defined class named id (a parent class or a method's class), or nil
  after a diagnostic. }
function lookupClass(const id: TIdent): PType;
var
  d: PDecl;
  c: PType;
begin
  lookupClass := nil;
  d := lookup(id.name);
  if d = nil then
  begin
    ms('unknown class name ''');
    maN(id.spelling);
    ma('''');
    maDidYouMean(id.name);
    errR(id.loc, identRange(id));
    exit;
  end;
  if (d^.kind = dkType) and (d^.declType <> nil) and isError(d^.declType) then
    exit;
  c := nil;
  if (d^.kind = dkType) and (d^.declType <> nil) and
     (d^.declType^.kind = tyClass) then
    c := d^.declType;
  if c = nil then
  begin
    ms('''');
    maN(id.spelling);
    ma(''' is not a class type');
    errR(id.loc, identRange(id));
    noteDeclaredHere(d);
    exit;
  end;
  if not c^.classDefined then
  begin
    ms('class ''');
    maN(id.spelling);
    ma(''' is not defined yet (only declared forward)');
    errR(id.loc, identRange(id));
    exit;
  end;
  lookupClass := c;
end;

{ The hidden parameter Self of method r of class c. }
procedure makeSelf(r: PDecl; c: PType);
var
  self_: PDecl;
begin
  self_ := newDecl(dkVar, memberIdent('Self'), noLoc);
  self_^.varKind := vkValueParam;
  { in a class method, Self is the class (a class reference) }
  if r^.isClassMethod then
    self_^.varType := classRefOf(c)
  else
    self_^.varType := c;
  self_^.owner := r;
  r^.selfVar := self_;
end;

function sameHeading(a, b: PDecl): boolean;
begin
  sameHeading := (a^.methodKind = b^.methodKind) and
                 (a^.isClassMethod = b^.isClassMethod) and
                 (a^.isFunction = b^.isFunction) and sameSignature(a, b) and
                 (not a^.isFunction or (a^.resultType = b^.resultType));
end;

procedure defineMethod(c, parentClass: PType; r, inh: PDecl; owner: PType;
                       var vtable: TPtrList);
var
  x, match: PDecl;
begin
  r^.ownerClass := c;
  r^.parent := nil;
  r^.level := 1;
  resolveSignature(r);
  makeSelf(r, c);
  { of overloaded inherited methods, the one with this heading }
  if (inh <> nil) and (inh^.overloadIndex <> 0) then
  begin
    match := nil;
    x := inh;
    while x <> nil do
    begin
      if sameSignature(x, r) then
        match := x;
      x := x^.nextOverload;
    end;
    if (match <> nil) or r^.isOverride then
      inh := match;
  end;
  if r^.isVirtual and r^.isOverride then
  begin
    ms('method ''');
    maN(r^.ident.spelling);
    ma(''' cannot be both ''virtual'' and ''override''');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if r^.isAbstract and not r^.isVirtual and not r^.isOverride then
  begin
    ms('abstract method ''');
    maN(r^.ident.spelling);
    ma(''' must be ''virtual''');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if r^.isOverride then
  begin
    if (inh = nil) or (inh^.vmtIndex < 0) then
    begin
      ms('method ''');
      maN(r^.ident.spelling);
      ma(''' is marked ''override'' but there is no virtual method ''');
      maN(r^.ident.spelling);
      ma(''' to override in ');
      maT(parentClass);
      errR(r^.ident.loc, identRange(r^.ident));
      if inh <> nil then
        noteDeclaredHere(inh);
      exit;
    end;
    if not sameHeading(inh, r) then
    begin
      ms('heading of ''');
      maN(r^.ident.spelling);
      ma(''' does not match the method it overrides in ');
      maT(owner);
      errR(r^.ident.loc, identRange(r^.ident));
      noteDeclaredHere(inh);
      exit;
    end;
    r^.vmtIndex := inh^.vmtIndex;
    listSet(vtable, inh^.vmtIndex, r);
    exit;
  end;
  if r^.isVirtual then
  begin
    r^.vmtIndex := vtable.count;
    listAdd(vtable, r);
  end;
end;

{ A property's accessor named id: a member declared before it in class c,
  or one of the parent's; nil after a diagnostic. }
function findAccessor(c, parentClass: PType; const own: TIntMap;
                      const id: TIdent): PDecl;
var
  d: PDecl;
  owner: PType;
begin
  d := PDecl(mapGet(own, id.name^.id));
  if d = nil then
    d := findMember(parentClass, id.name, owner);
  if d = nil then
  begin
    ms('no field or method named ''');
    maN(id.spelling);
    ma(''' in ');
    maT(c);
    errR(id.loc, identRange(id));
  end;
  findAccessor := d;
end;

{ Do the leading parameters of accessor method r fit property p: its index
  parameters (same types and kinds), then an integer for index N? }
function indexParamsMatch(p, r: PDecl): boolean;
var
  i, n, extra: longint;
  a, b: PDecl;
begin
  indexParamsMatch := false;
  n := p^.propParams.count;
  extra := 0;
  if p^.propIndex <> nil then
    extra := 1;
  if r^.params.count < n + extra then
    exit;
  for i := 0 to n - 1 do
  begin
    a := PDecl(listGet(r^.params, i));
    b := PDecl(listGet(p^.propParams, i));
    if (a^.varType <> b^.varType) or (a^.varKind <> b^.varKind) then
      exit;
  end;
  if p^.propIndex <> nil then
  begin
    a := PDecl(listGet(r^.params, n));
    exit((a^.varKind <> vkVarParam) and (a^.varType <> nil) and
         isIntegerLike(a^.varType));
  end;
  indexParamsMatch := true;
end;

{ Appends " (use a function (i: integer; ...): T)" (or a procedure, with
  the value) to the message: the accessor property p wants. }
procedure maWantedAccessor(p: PDecl; isRead: boolean);
var
  i: longint;
  q: PDecl;
  first: boolean;
begin
  if isRead then
    ma(' (use a function (')
  else
    ma(' (use a procedure (');
  first := true;
  for i := 0 to p^.propParams.count - 1 do
  begin
    q := PDecl(listGet(p^.propParams, i));
    if not first then
      ma('; ');
    first := false;
    if q^.varKind = vkConstParam then
      ma('const ');
    maN(q^.ident.spelling);
    ma(': ');
    bufAddTypeName(msg, q^.varType);
  end;
  if p^.propIndex <> nil then
  begin
    if not first then
      ma('; ');
    first := false;
    ma('index: integer');
  end;
  if not isRead then
  begin
    if not first then
      ma('; ');
    ma('value: ');
    bufAddTypeName(msg, p^.propType);
  end;
  ma(')');
  if isRead then
  begin
    ma(': ');
    bufAddTypeName(msg, p^.propType);
  end;
  ma(')');
end;

{ Does routine x fit as property p's read accessor (leading: its index
  parameters and N) / write accessor? }
function readFits(p, x: PDecl; t: PType; leading: longint): boolean;
begin
  readFits := x^.isFunction and (x^.params.count = leading) and
              indexParamsMatch(p, x) and (x^.resultType = t);
end;

function writeFits(p, x: PDecl; t: PType; leading: longint): boolean;
var
  prm: PDecl;
begin
  writeFits := false;
  if (x^.methodKind = mtMethod) and not x^.isFunction and
     (x^.params.count = leading + 1) and indexParamsMatch(p, x) then
  begin
    prm := PDecl(listGet(x^.params, leading));
    writeFits := (prm^.varKind <> vkVarParam) and (prm^.varType = t);
  end;
end;

{ property P[params]: T [index N] read R write W -- R: a field of type T
  (a plain property) or a function returning T; W: a field of type T or a
  procedure taking the value (a value or const parameter of type T). The
  methods first take the index parameters of an array property (same types
  and kinds), then N (an integer) for an indexed property. }
procedure definePropertyAccessors(c, parentClass: PType; p: PDecl;
                                  const own: TIntMap);
var
  t, it, dt: PType;
  rd, wr, prm, x: PDecl;
  ok, paramsOK, plain: boolean;
  i, leading: longint;
  folded: TConstant;
begin
  t := resolveType(p^.propSpec, nil);
  p^.propType := t;
  paramsOK := true;
  for i := 0 to p^.propParams.count - 1 do
  begin
    prm := PDecl(listGet(p^.propParams, i));
    prm^.varType := resolveType(prm^.varSpec, nil);
    if isError(prm^.varType) then
      paramsOK := false;
    if prm^.varKind = vkVarParam then
    begin
      ms('the index parameters of a property cannot be ''var''');
      errR(prm^.loc, identRange(prm^.ident));
      paramsOK := false;
    end;
  end;
  if p^.propIndex <> nil then
  begin
    it := check(p^.propIndex);
    if not isError(it) and (not isIntegerLike(it) or not fold(p^.propIndex, folded)) then
    begin
      ms('the index of a property must be an integer constant');
      errR(p^.propIndex^.b, nodeRange(p^.propIndex));
      paramsOK := false;
    end;
  end;
  if p^.propDefaultValue <> nil then
  begin
    dt := check(p^.propDefaultValue);
    if not isError(dt) and not isError(t) and
       (not fold(p^.propDefaultValue, folded) or not isAssignable(t, dt)) then
    begin
      ms('the default value of property ''');
      maN(p^.ident.spelling);
      ma(''' must be a constant of type ');
      maT(t);
      errR(p^.propDefaultValue^.b, nodeRange(p^.propDefaultValue));
    end;
  end;
  if p^.propIsDefault and (p^.propParams.count = 0) then
  begin
    ms('only an array property can be the default property');
    errR(p^.loc, identRange(p^.ident));
  end;
  plain := (p^.propParams.count = 0) and (p^.propIndex = nil);
  leading := p^.propParams.count;
  if p^.propIndex <> nil then
    leading := leading + 1;
  if not identValid(p^.readName) and not identValid(p^.writeName) then
  begin
    ms('property ''');
    maN(p^.ident.spelling);
    ma(''' needs a ''read'' or ''write'' accessor');
    errR(p^.loc, identRange(p^.ident));
    exit;
  end;
  rd := nil;
  wr := nil;
  if identValid(p^.readName) then
    rd := findAccessor(c, parentClass, own, p^.readName);
  if (rd <> nil) and (rd^.kind = dkRoutine) and (rd^.overloadIndex <> 0) then
  begin { of overloaded methods, the one that fits }
    x := rd;
    while x <> nil do
    begin
      if readFits(p, x, t, leading) then
      begin
        rd := x;
        break;
      end;
      x := x^.nextOverload;
    end;
  end;
  if rd <> nil then
  begin
    ok := (plain and (rd^.kind = dkField) and (rd^.fieldType = t)) or
          ((rd^.kind = dkRoutine) and readFits(p, rd, t, leading));
    if not ok and not isError(t) and paramsOK then
    begin
      ms('''');
      maN(p^.readName.spelling);
      ma(''' cannot be read as property ''');
      maN(p^.ident.spelling);
      ma(''' of type ');
      maT(t);
      if plain then
        ma(' (use a field of that type or a function without parameters ' +
           'returning it)')
      else
        maWantedAccessor(p, true);
      errR(p^.readName.loc, identRange(p^.readName));
      rd := nil;
    end;
  end;
  if identValid(p^.writeName) then
    wr := findAccessor(c, parentClass, own, p^.writeName);
  if (wr <> nil) and (wr^.kind = dkRoutine) and (wr^.overloadIndex <> 0) then
  begin { of overloaded methods, the one that fits }
    x := wr;
    while x <> nil do
    begin
      if writeFits(p, x, t, leading) then
      begin
        wr := x;
        break;
      end;
      x := x^.nextOverload;
    end;
  end;
  if wr <> nil then
  begin
    ok := (plain and (wr^.kind = dkField) and (wr^.fieldType = t)) or
          ((wr^.kind = dkRoutine) and writeFits(p, wr, t, leading));
    if not ok and not isError(t) and paramsOK then
    begin
      ms('''');
      maN(p^.writeName.spelling);
      ma(''' cannot be written as property ''');
      maN(p^.ident.spelling);
      ma(''' of type ');
      maT(t);
      if plain then
        ma(' (use a field of that type or a procedure with one parameter ' +
           'of it)')
      else
        maWantedAccessor(p, false);
      errR(p^.writeName.loc, identRange(p^.writeName));
      wr := nil;
    end;
  end;
  p^.readDecl := rd;
  p^.writeDecl := wr;
end;

{ --- Interfaces (objfpc, CORBA style) --- }

{ IFoo = interface ... end;  /  IFoo = interface; }
procedure defineInterfaceMembers(i: PType; ts: PTypeSpec); forward;

procedure declareInterface(td: PDecl);
var
  ts: PTypeSpec;
  t: PType;
  k, j: longint;
  pd: PDecl;
begin
  ts := td^.typeSpec;
  if curRoutine <> nil then
  begin
    ms('interface ''');
    maN(td^.ident.spelling);
    ma(''' must be declared at program or unit level');
    errR(td^.ident.loc, identRange(td^.ident));
    td^.declType := errorType;
    ts^.resolved := errorType;
    declare(td);
    exit;
  end;
  if not ts^.isForwardIntf then
    for k := 0 to pendingInterfaces.count - 1 do
    begin
      pd := PDecl(listGet(pendingInterfaces, k));
      if (pd^.ident.name = td^.ident.name) and
         (scopeLookupOwn(cur, td^.ident.name) = pd) then
      begin
        for j := k to pendingInterfaces.count - 2 do
          listSet(pendingInterfaces, j, listGet(pendingInterfaces, j + 1));
        listPop(pendingInterfaces);
        t := pd^.declType;
        td^.declType := t;
        ts^.resolved := t;
        defineInterfaceMembers(t, ts);
        exit;
      end;
    end;
  t := newInterfaceType;
  setDeclName(t, td^.ident.spelling);
  td^.declType := t;
  ts^.resolved := t;
  declare(td);
  if ts^.isForwardIntf then
  begin
    listAdd(pendingInterfaces, td);
    exit;
  end;
  defineInterfaceMembers(t, ts);
end;

{ The defined interface named id, or nil after a diagnostic. }
function lookupInterface(const id: TIdent): PType;
var
  d: PDecl;
  t: PType;
begin
  lookupInterface := nil;
  d := lookup(id.name);
  if d = nil then
  begin
    ms('unknown interface name ''');
    maN(id.spelling);
    ma('''');
    maDidYouMean(id.name);
    errR(id.loc, identRange(id));
    exit;
  end;
  if (d^.kind = dkType) and (d^.declType <> nil) and isError(d^.declType) then
    exit;
  t := nil;
  if (d^.kind = dkType) and (d^.declType <> nil) and
     (d^.declType^.kind = tyInterface) then
    t := d^.declType;
  if t = nil then
  begin
    ms('''');
    maN(id.spelling);
    ma(''' is not an interface type');
    errR(id.loc, identRange(id));
    noteDeclaredHere(d);
    exit;
  end;
  if not t^.intfDefined then
  begin
    ms('interface ''');
    maN(id.spelling);
    ma(''' is not defined yet (only declared forward)');
    errR(id.loc, identRange(id));
    exit;
  end;
  lookupInterface := t;
end;

{ The method name of interface i or its nearest ancestor declaring it (the
  first of its overloads). }
function findInterfaceMethod(i: PType; name: PName): PDecl;
var
  k: longint;
  r: PDecl;
begin
  findInterfaceMethod := nil;
  while i <> nil do
  begin
    for k := 0 to i^.intfMethods.count - 1 do
    begin
      r := PDecl(listGet(i^.intfMethods, k));
      if r^.ident.name = name then
        exit(r);
    end;
    i := i^.intfParent;
  end;
end;

procedure defineInterfaceMembers(i: PType; ts: PTypeSpec);
var
  parentIntf: PType;
  own, all: TPtrList;
  seen: TIntMap;
  k: longint;
  r, inh, prev: PDecl;
begin
  parentIntf := nil;
  if identValid(ts^.intfParentName) then
  begin
    if ts^.intfParentName.name = intern(lowerStr(nameStr(i^.declName))) then
    begin
      ms('interface ');
      maT(i);
      ma(' cannot inherit from itself');
      errR(ts^.intfParentName.loc, identRange(ts^.intfParentName));
    end
    else
      parentIntf := lookupInterface(ts^.intfParentName);
  end;
  listInit(own);
  listInit(all);
  if parentIntf <> nil then
    for k := 0 to parentIntf^.intfAllMethods.count - 1 do
      listAdd(all, listGet(parentIntf^.intfAllMethods, k));
  mapInit(seen);
  for k := 0 to ts^.intfMemberList.count - 1 do
  begin
    r := PDecl(listGet(ts^.intfMemberList, k));
    r^.routineIntf := i;
    r^.parent := nil;
    r^.level := 1;
    resolveSignature(r);
    if r^.isVirtual or r^.isOverride or r^.isAbstract then
    begin
      ms('the methods of an interface cannot be ''virtual'', ''override'' or ' +
         '''abstract''');
      errR(r^.ident.loc, identRange(r^.ident));
      continue;
    end;
    inh := nil;
    if parentIntf <> nil then
      inh := findInterfaceMethod(parentIntf, r^.ident.name);
    if inh <> nil then
    begin
      ms('''');
      maN(r^.ident.spelling);
      ma(''' is already declared in interface ');
      maT(inh^.routineIntf);
      errR(r^.ident.loc, identRange(r^.ident));
      noteDeclaredHere(inh);
      continue;
    end;
    prev := PDecl(mapGet(seen, r^.ident.name^.id));
    if prev <> nil then
    begin
      if not addOverload(prev, r) then
        continue;
    end
    else
      mapPut(seen, r^.ident.name^.id, r);
    r^.vmtIndex := all.count;
    listAdd(all, r);
    listAdd(own, r);
  end;
  defineInterface(i, parentIntf, own, all, curModule);
end;

{ The methods of class c implementing the methods of interface i (by
  slot): methods (not class methods) of c or its ancestors with the same
  name and heading. where: the interface's name in c's declaration, to
  report a missing one (nil: inherited, already checked). }
function implementInterface(c, i: PType; where: PIdent): PInterfaceImpl;
var
  impl: PInterfaceImpl;
  k: longint;
  m, r, found: PDecl;
  x: PType;
begin
  impl := PInterfaceImpl(arenaAlloc(sizeof(TInterfaceImpl)));
  impl^.intf := i;
  listInit(impl^.methods);
  for k := 0 to i^.intfAllMethods.count - 1 do
  begin
    m := PDecl(listGet(i^.intfAllMethods, k));
    found := nil;
    x := c;
    while (x <> nil) and (found = nil) do
    begin
      r := findOwnMember(x, m^.ident.name);
      if (r <> nil) and (r^.kind <> dkRoutine) then
        r := nil;
      while (r <> nil) and (found = nil) do
      begin
        if (r^.methodKind = mtMethod) and not r^.isClassMethod and
           (r^.objMethod = obNone) and (r^.isFunction = m^.isFunction) and
           sameSignature(r, m) and
           (not m^.isFunction or (r^.resultType = m^.resultType)) then
          found := r;
        r := r^.nextOverload;
      end;
      x := x^.classParent;
    end;
    if (found = nil) and (where <> nil) then
    begin
      ms('class ');
      maT(c);
      ma(' does not implement ''');
      bufAddTypeName(msg, m^.routineIntf);
      ma('.');
      maN(m^.ident.spelling);
      ma('''');
      errR(where^.loc, identRange(where^));
      noteDeclaredHere(m);
    end;
    listAdd(impl^.methods, found);
  end;
  implementInterface := impl;
end;

procedure defineClassMembers(c: PType; ts: PTypeSpec);
var
  parentClass, owner, pc, it: PType;
  members, fields, vtable, listed, listedWhere, impls: TPtrList;
  own: TIntMap;
  i, k: longint;
  m, prev, inh, defaultProp, overloadOf, td: PDecl;
  pi: PInterfaceImpl;

  { the interfaces class(TParent, I1, I2) lists }
  procedure addListed(id: PIdent);
  var
    t: PType;
    j: longint;
  begin
    t := lookupInterface(id^);
    if t = nil then
      exit;
    for j := 0 to listed.count - 1 do
      if listGet(listed, j) = pointer(t) then
      begin
        ms('interface ''');
        maN(id^.spelling);
        ma(''' is listed twice');
        errR(id^.loc, identRange(id^));
        exit;
      end;
    listAdd(listed, t);
    listAdd(listedWhere, id);
  end;

begin
  defaultProp := nil;
  parentClass := tobjectType;
  listInit(listed);
  listInit(listedWhere);
  if identValid(ts^.parentName) then
  begin
    td := lookup(ts^.parentName.name);
    if (td <> nil) and (td^.kind <> dkType) then
      td := nil;
    if ts^.parentName.name = intern(lowerStr(nameStr(c^.declName))) then
    begin
      ms('class ');
      maT(c);
      ma(' cannot inherit from itself');
      errR(ts^.parentName.loc, identRange(ts^.parentName));
    end
    else if (td <> nil) and (td^.declType <> nil) and
            (td^.declType^.kind = tyInterface) then
      addListed(@ts^.parentName)
    else
    begin
      pc := lookupClass(ts^.parentName);
      if pc <> nil then
        parentClass := pc;
    end;
  end;
  for k := 0 to ts^.interfaceNames.count - 1 do
    addListed(PIdent(listGet(ts^.interfaceNames, k)));

  listInit(members);
  listInit(fields);
  for i := 0 to parentClass^.classAllFields.count - 1 do
    listAdd(fields, listGet(parentClass^.classAllFields, i));
  listInit(vtable);
  for i := 0 to parentClass^.classVTable.count - 1 do
    listAdd(vtable, listGet(parentClass^.classVTable, i));
  mapInit(own);
  for i := 0 to ts^.memberList.count - 1 do
  begin
    m := PDecl(listGet(ts^.memberList, i));
    prev := PDecl(mapGet(own, m^.ident.name^.id));
    overloadOf := nil; { objfpc: an overloaded method }
    if (prev <> nil) and (mode = lmObjFPC) and (prev^.kind = dkRoutine) and
       (m^.kind = dkRoutine) then
    begin
      overloadOf := prev;
      prev := nil;
    end;
    if prev <> nil then
    begin
      ms('redefinition of ''');
      maN(m^.ident.spelling);
      ma('''');
      errR(m^.ident.loc, identRange(m^.ident));
      ms('previous definition is here');
      note(prev^.loc);
      continue;
    end;
    owner := nil;
    inh := findMember(parentClass, m^.ident.name, owner);
    if (inh <> nil) and not ((m^.kind = dkRoutine) and (inh^.kind = dkRoutine)) then
    begin
      ms('''');
      maN(m^.ident.spelling);
      ma(''' is already declared in class ');
      maT(owner);
      errR(m^.ident.loc, identRange(m^.ident));
      noteDeclaredHere(inh);
      continue;
    end;
    if overloadOf = nil then
      mapPut(own, m^.ident.name^.id, m);
    listAdd(members, m);
    case m^.kind of
      dkField:
        begin
          m^.fieldType := resolveType(m^.fieldSpec, nil);
          if containsFile(m^.fieldType) then
          begin
            ms('a field of a class cannot be a file');
            errR(m^.ident.loc, identRange(m^.ident));
          end;
          listAdd(fields, m);
        end;
      dkRoutine:
        begin
          if (inh <> nil) and (inh^.kind <> dkRoutine) then
            inh := nil;
          defineMethod(c, parentClass, m, inh, owner, vtable);
          if (overloadOf <> nil) and not addOverload(overloadOf, m) then
          begin
            m^.ownerClass := nil; { rejected: not a member }
            listPop(members);
          end;
        end;
      dkVar: { class var: static storage }
        begin
          m^.varType := resolveType(m^.varSpec, nil);
          m^.varClass := c;
          if containsFile(m^.varType) then
          begin
            ms('a class variable cannot be a file');
            errR(m^.ident.loc, identRange(m^.ident));
          end;
        end;
    else
      begin
        definePropertyAccessors(c, parentClass, m, own);
        if m^.propIsDefault then
        begin
          if defaultProp <> nil then
          begin
            ms('class ');
            maT(c);
            ma(' already has a default property');
            errR(m^.ident.loc, identRange(m^.ident));
            noteDeclaredHere(defaultProp);
          end;
          defaultProp := m;
        end;
      end;
    end;
  end;
  defineClass(c, parentClass, members, fields, vtable, curModule);
  { the interfaces: the parent's (with this class's overrides), then its own }
  listInit(impls);
  for k := 0 to parentClass^.classInterfaces.count - 1 do
  begin
    pi := PInterfaceImpl(listGet(parentClass^.classInterfaces, k));
    listAdd(impls, implementInterface(c, pi^.intf, nil));
  end;
  for k := 0 to listed.count - 1 do
  begin
    it := PType(listGet(listed, k));
    if findInterface(parentClass, it) = nil then
      listAdd(impls, implementInterface(c, it, PIdent(listGet(listedWhere, k))));
  end;
  c^.classInterfaces := impls;
end;

{ The default property of c or its nearest ancestor declaring one. }
function defaultProperty(c: PType): PDecl;
var
  i: longint;
  m: PDecl;
begin
  defaultProperty := nil;
  while c <> nil do
  begin
    for i := 0 to c^.classMembers.count - 1 do
    begin
      m := PDecl(listGet(c^.classMembers, i));
      if (m^.kind = dkProperty) and m^.propIsDefault then
        exit(m);
    end;
    c := c^.classParent;
  end;
end;

{ procedure TFoo.Bar ...; begin ... end;  -- the body of a method. }
procedure maRoutineHeading(r: PDecl); forward;

procedure declareMethodBody(r: PDecl);
var
  c, rt: PType;
  m, p, x, match: PDecl;
  i: longint;
begin
  if curRoutine <> nil then
  begin
    ms('method ''');
    maN(r^.methodClassName.spelling);
    ma('.');
    maN(r^.ident.spelling);
    ma(''' must be implemented at program or unit level');
    errR(r^.methodClassName.loc, identRange(r^.methodClassName));
    exit;
  end;
  c := lookupClass(r^.methodClassName);
  if c = nil then
    exit;
  m := findOwnMember(c, r^.ident.name);
  if (m <> nil) and ((m^.kind <> dkRoutine) or (m^.objMethod <> obNone)) then
    m := nil;
  if m = nil then
  begin
    ms('class ');
    maT(c);
    ma(' has no method ''');
    maN(r^.ident.spelling);
    ma('''');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if m^.overloadIndex <> 0 then { objfpc: the method with this heading }
  begin
    for i := 0 to r^.params.count - 1 do
    begin
      p := PDecl(listGet(r^.params, i));
      p^.varType := resolveType(p^.varSpec, nil);
    end;
    match := nil;
    x := m;
    while x <> nil do
    begin
      if sameSignature(x, r) and (x^.body = nil) then
        match := x;
      x := x^.nextOverload;
    end;
    if match = nil then
    begin
      ms('heading of ''');
      maQualified(c, m^.ident);
      ma(''' does not match any of its overloads');
      errR(r^.ident.loc, identRange(r^.ident));
      x := m;
      while x <> nil do
      begin
        ms('candidate: ');
        maRoutineHeading(x);
        note(x^.loc);
        x := x^.nextOverload;
      end;
      exit;
    end;
    m := match;
  end;
  if c^.classModule <> curModule then
  begin
    ms('method ''');
    maQualified(c, m^.ident);
    ma(''' must be implemented where its class is declared');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if (m^.methodKind <> r^.methodKind) or (m^.isFunction <> r^.isFunction) or
     (m^.isClassMethod <> r^.isClassMethod) then
  begin
    ms('''');
    maQualified(c, m^.ident);
    if m^.isClassMethod then
      ma(''' was declared as a class ' + kindName(m))
    else
      ma(''' was declared as a ' + kindName(m));
    errR(r^.ident.loc, identRange(r^.ident));
    noteDeclaredHere(m);
    exit;
  end;
  if m^.isAbstract then
  begin
    ms('abstract method ''');
    maQualified(c, m^.ident);
    ma(''' cannot have a body');
    errR(r^.ident.loc, identRange(r^.ident));
    noteDeclaredHere(m);
    exit;
  end;
  if m^.body <> nil then
  begin
    ms('redefinition of ''');
    maQualified(c, m^.ident);
    ma('''');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
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
    if not sameSignature(m, r) or ((rt <> nil) and (rt <> m^.resultType)) then
    begin
      ms('heading of ''');
      maQualified(c, m^.ident);
      ma(''' does not match its declaration');
      errR(r^.ident.loc, identRange(r^.ident));
      noteDeclaredHere(m);
    end;
  end;
  m^.body := r^.body;
  m^.implDecl := r;
  r^.canonical := m;
  checkRoutineBody(m);
end;

{ Every method of the classes declared in b has a body. }
procedure checkMethodsImplemented(b: PBlock);
var
  i, j: longint;
  d, r: PDecl;
  c: PType;
begin
  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    if (d^.kind <> dkType) or (d^.typeSpec^.kind <> tsClass) or
       d^.typeSpec^.isForwardClass then
      continue;
    c := d^.declType;
    if (c = nil) or (c^.kind <> tyClass) then
      continue;
    for j := 0 to d^.typeSpec^.memberList.count - 1 do
    begin
      r := PDecl(listGet(d^.typeSpec^.memberList, j));
      if (r^.kind = dkRoutine) and (r^.ownerClass = c) and not r^.isAbstract and
         (r^.body = nil) then
      begin
        ms('method ''');
        maQualified(c, r^.ident);
        ma(''' is declared but never implemented');
        errR(r^.ident.loc, identRange(r^.ident));
      end;
    end;
  end;
end;

{ The method whose body (or a routine nested in it) is being checked. }
function enclosingMethod: PDecl;
var
  r: PDecl;
begin
  r := curRoutine;
  while r <> nil do
  begin
    if r^.methodKind <> mtNone then
      exit(r);
    r := r^.parent;
  end;
  enclosingMethod := nil;
end;

{ The class whose members a with context exposes (nil for records). }
function classOfWith(w: PWithContext): PType;
begin
  classOfWith := nil;
  if (w = nil) or (w^.rec^.typ = nil) then
    exit;
  if w^.rec^.typ^.kind = tyClass then
    classOfWith := w^.rec^.typ
  else if w^.rec^.typ^.kind = tyClassRef then { Self in a class method }
    classOfWith := w^.rec^.typ^.refClass;
end;

{ Is d an instance member (a field, a property, a method that is not a
  class method or constructor)? Class methods cannot use them. }
function isInstanceMember(d: PDecl): boolean;
begin
  isInstanceMember := (d^.kind in [dkField, dkProperty]) or
                      ((d^.kind = dkRoutine) and (d^.methodKind <> mtNone) and
                       not d^.isClassMethod and (d^.methodKind <> mtConstructor));
end;

{ In a class method, Self is a class: a constructor called through it
  creates an object of that class (its static type: Self's class). }
function constructedThroughSelf(r: PDecl; w: PWithContext): PType;
begin
  constructedThroughSelf := nil;
  if (r^.methodKind <> mtConstructor) or (w = nil) or not w^.isSelf then
    exit;
  if (w^.rec^.typ <> nil) and (w^.rec^.typ^.kind = tyClassRef) then
    constructedThroughSelf := w^.rec^.typ^.refClass;
end;

{ Reports use of a private or protected member d of class owner where it
  is not visible: private members are visible in the program or unit
  declaring the class, protected ones also in methods of descendants. }
procedure checkMemberAccess(d: PDecl; owner: PType; const id: TIdent);
var
  m: PDecl;
  strict_: boolean;
begin
  if (d^.vis = vsPublic) or (d^.vis = vsPublished) or (owner = nil) then
    exit;
  strict_ := d^.vis in [vsStrictPrivate, vsStrictProtected];
  if not strict_ and (owner^.classModule = curModule) then
    exit;
  { strict private: methods of the class; (strict) protected: also of its
    descendants }
  m := enclosingMethod;
  if m <> nil then
  begin
    if d^.vis = vsStrictPrivate then
    begin
      if m^.ownerClass = owner then
        exit;
    end
    else if d^.vis <> vsPrivate then
      if inheritsFrom(m^.ownerClass, owner) then
        exit;
  end;
  ms('''');
  maN(id.spelling);
  case d^.vis of
    vsPrivate: ma(''' is a private member of ');
    vsProtected: ma(''' is a protected member of ');
    vsStrictPrivate: ma(''' is a strict private member of ');
  else
    ma(''' is a strict protected member of ');
  end;
  maT(owner);
  errR(id.loc, identRange(id));
  noteDeclaredHere(d);
end;

{ Visibility of a member found by name through a class scope. }
procedure checkScopeAccess(d: PDecl; w: PWithContext; const id: TIdent);
var
  c, owner: PType;
begin
  c := classOfWith(w);
  if c = nil then
    exit;
  owner := nil;
  findMember(c, id.name, owner);
  checkMemberAccess(d, owner, id);
  if w^.isSelf and (w^.rec^.typ^.kind = tyClassRef) and isInstanceMember(d) then
  begin
    ms('instance member ''');
    maN(id.spelling);
    ma(''' cannot be used in a class method');
    errR(id.loc, identRange(id));
    noteDeclaredHere(d);
  end;
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

{ r (resolved) joins the overloads first...: its parameters must differ.
  False after a diagnostic. }
function addOverload(first, r: PDecl): boolean;
var
  x, last: PDecl;
begin
  addOverload := false;
  x := first;
  while x <> nil do
  begin
    if sameSignature(x, r) then
    begin
      ms('redefinition of ''');
      maN(r^.ident.spelling);
      ma(''' (overloads must differ in their parameters)');
      errR(r^.ident.loc, identRange(r^.ident));
      ms('previous definition is here');
      note(x^.loc);
      exit;
    end;
    x := x^.nextOverload;
  end;
  if first^.overloadIndex = 0 then
    first^.overloadIndex := 1;
  last := first;
  while last^.nextOverload <> nil do
    last := last^.nextOverload;
  r^.overloadIndex := last^.overloadIndex + 1;
  last^.nextOverload := r;
  addOverload := true;
end;

{ Declares r; in objfpc, another routine of its name in the same scope
  makes it an overload (with other parameters). False if r was rejected as
  such (its body is not checked then). }
function declareRoutineName(r: PDecl): boolean;
var
  first: PDecl;
begin
  if mode = lmObjFPC then
  begin
    first := scopeLookupOwn(cur, r^.ident.name);
    if (first <> nil) and (first^.kind = dkRoutine) then
      exit(addOverload(first, r));
  end;
  declare(r);
  declareRoutineName := true;
end;

procedure declareRoutine(r: PDecl);
var
  fwd, p, x, match: PDecl;
  i: longint;
  rt: PType;
  inInterface: boolean;
begin
  r^.parent := curRoutine;
  if curRoutine <> nil then
    r^.level := curRoutine^.level + 1
  else
    r^.level := 1;
  if identValid(r^.methodClassName) then
  begin
    declareMethodBody(r);
    exit;
  end;
  if r^.isClassMethod then
  begin
    ms('only a method can be a class ' + kindName(r) + '; implement it as ''' +
       kindName(r) + ' TClass.');
    maN(r^.ident.spelling);
    ma('''');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  { the definition of an earlier forward declaration? }
  fwd := scopeLookupOwn(cur, r^.ident.name);
  if (fwd <> nil) and (fwd^.kind <> dkRoutine) then
    fwd := nil;
  inInterface := false;
  if (fwd = nil) and (curUnitInterface <> nil) and (curRoutine = nil) then
  begin
    fwd := scopeLookupOwn(curUnitInterface, r^.ident.name);
    if (fwd <> nil) and (fwd^.kind <> dkRoutine) then
      fwd := nil;
    inInterface := fwd <> nil;
  end;
  { objfpc: of overloaded routines, the one with this heading }
  if (fwd <> nil) and (fwd^.overloadIndex <> 0) and (r^.body <> nil) then
  begin
    for i := 0 to r^.params.count - 1 do
    begin
      p := PDecl(listGet(r^.params, i));
      p^.varType := resolveType(p^.varSpec, nil);
    end;
    match := nil;
    x := fwd;
    while x <> nil do
    begin
      if x^.isForward and (x^.body = nil) and sameSignature(x, r) then
        match := x;
      x := x^.nextOverload;
    end;
    fwd := match;
    if (match = nil) and inInterface then
    begin
      ms('overloads of ''');
      maN(r^.ident.spelling);
      ma(''' must all be declared in the interface');
      errR(r^.ident.loc, identRange(r^.ident));
      exit;
    end;
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
  if inInterface and (mode = lmObjFPC) then { a different heading }
  begin
    ms('overloads of ''');
    maN(r^.ident.spelling);
    ma(''' must all be declared in the interface');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if not declareRoutineName(r) then
    exit;
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

{ --- Overload resolution (objfpc) --- }

{ The routines a call of first's name may mean: first's overloads; for a
  method with the overload directive also those of its ancestors that it
  does not override. }
procedure collectOverloads(first: PDecl; var outList: TPtrList);
var
  x, up, y: PDecl;
  directive, dup: boolean;
  start, i, j: longint;
  inherited_: TPtrList;
  parentClass, owner: PType;
begin
  directive := false;
  start := outList.count;
  x := first;
  while x <> nil do
  begin
    listAdd(outList, x);
    if x^.hasOverloadDirective then
      directive := true;
    x := x^.nextOverload;
  end;
  if not directive or (first^.methodKind = mtNone) or (first^.ownerClass = nil) then
    exit;
  parentClass := first^.ownerClass^.classParent;
  if parentClass = nil then
    exit;
  owner := nil;
  up := findMember(parentClass, first^.ident.name, owner);
  if (up = nil) or (up^.kind <> dkRoutine) then
    exit;
  listInit(inherited_);
  collectOverloads(up, inherited_);
  for i := 0 to inherited_.count - 1 do
  begin
    y := PDecl(listGet(inherited_, i));
    dup := false;
    for j := start to outList.count - 1 do
      if sameSignature(PDecl(listGet(outList, j)), y) then
        dup := true;
    if not dup then
      listAdd(outList, y);
  end;
end;

{ How far an argument of type a is from a parameter of type p (lower is
  better): an exact type, a subrange of it, a wider integer type (integer
  before int64), a same-size or narrower one, real, an ancestor class (the
  nearest first), anything else assignable. }
function conversionCost(p, a: PType): longint;
var
  pb, ab, c: PType;
  d: longint;
begin
  if p = a then
    exit(0);
  pb := getBase(p);
  ab := getBase(a);
  if pb = ab then
    exit(1);
  if (pb^.kind = tyInteger) and (ab^.kind = tyInteger) then
  begin
    if pb^.bits > ab^.bits then
    begin
      if pb^.bits = 64 then
        exit(3);
      exit(2);
    end;
    if pb^.bits = ab^.bits then
      exit(4);
    exit(5);
  end;
  if (p^.kind = tyReal) and (ab^.kind = tyInteger) then
    exit(6);
  if (p^.kind = tyShortString) and (a^.kind in [tyShortString, tyStringLit]) then
    exit(1);
  if (p^.kind = tyShortString) and (ab^.kind = tyChar) then
    exit(2);
  if ((p^.kind = tyClass) and (a^.kind = tyClass)) or
     ((p^.kind = tyClassRef) and (a^.kind = tyClassRef)) then
  begin
    if p^.kind = tyClassRef then
    begin
      p := p^.refClass;
      a := a^.refClass;
    end;
    d := 0;
    c := a;
    while (c <> nil) and (c <> p) do
    begin
      d := d + 1;
      c := c^.classParent;
    end;
    exit(1 + d);
  end;
  if a^.kind = tyNil then
    exit(1);
  conversionCost := 7;
end;

{ Is arg the name of a routine (passed for a procedural parameter)? }
function isRoutineName(arg: PNode): boolean;
var
  d: PDecl;
begin
  isRoutineName := false;
  if arg^.kind <> nkNameRef then
    exit;
  d := lookup(arg^.ref.name);
  isRoutineName := (d <> nil) and (d^.kind = dkRoutine);
end;

{ The cost of calling r with args (types argTys; nil: a routine's name), or
  -1 if they do not fit. }
function callCost(r: PDecl; const args, argTys: TPtrList): longint;
var
  i, cost: longint;
  p: PDecl;
  pt, at: PType;
  arg: PNode;
begin
  callCost := -1;
  if r^.params.count <> args.count then
    exit;
  cost := 0;
  for i := 0 to args.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    pt := p^.varType;
    at := PType(listGet(argTys, i));
    arg := PNode(listGet(args, i));
    if (pt = nil) or isError(pt) then
      exit;
    if at = nil then { a routine's name: for a procedural parameter }
    begin
      if pt^.kind <> tyRoutine then
        exit;
      continue;
    end;
    if p^.varKind = vkVarParam then
    begin
      if (at <> pt) or not isVariableReference(arg) then
        exit;
      continue;
    end;
    if not isAssignable(pt, at) and not isPCharLiteral(pt, arg) then
      exit;
    cost := cost + conversionCost(pt, at);
  end;
  callCost := cost;
end;

{ "f(integer; var string): real" for diagnostics }
procedure maRoutineHeading(r: PDecl);
var
  i: longint;
  p: PDecl;
begin
  maN(r^.ident.spelling);
  ma('(');
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    if i > 0 then
      ma('; ');
    if p^.varKind = vkVarParam then
      ma('var ')
    else if p^.varKind = vkConstParam then
      ma('const ');
    if p^.varType <> nil then
      bufAddTypeName(msg, p^.varType)
    else
      ma('?');
  end;
  ma(')');
  if r^.isFunction and (r^.resultType <> nil) then
  begin
    ma(': ');
    bufAddTypeName(msg, r^.resultType);
  end;
end;

{ The routine of first's name that args fit best: first itself unless it
  is overloaded. nil after a diagnostic (no fit, or a tie). }
function resolveOverload(first: PDecl; const args: TPtrList; const callee: TIdent;
                         rng: TRange): PDecl;
var
  cands, argTys: TPtrList;
  i, cost, bestCost: longint;
  r, best: PDecl;
  t: PType;
  a: PNode;
  tie: boolean;
begin
  resolveOverload := nil;
  listInit(cands);
  collectOverloads(first, cands);
  if cands.count < 2 then
    exit(first);
  listInit(argTys);
  for i := 0 to args.count - 1 do
  begin
    a := PNode(listGet(args, i));
    if a^.kind = nkFormatted then
      exit(first); { reported with the call's arguments }
    if isRoutineName(a) then
      t := nil
    else
      t := check(a);
    if (t <> nil) and isError(t) then
      exit;
    listAdd(argTys, t);
  end;
  best := nil;
  bestCost := -1;
  tie := false;
  for i := 0 to cands.count - 1 do
  begin
    r := PDecl(listGet(cands, i));
    cost := callCost(r, args, argTys);
    if cost < 0 then
      continue;
    if (best = nil) or (cost < bestCost) then
    begin
      best := r;
      bestCost := cost;
      tie := false;
    end
    else if cost = bestCost then
      tie := true;
  end;
  if (best <> nil) and not tie then
    exit(best);
  if best <> nil then
    ms('ambiguous call to overloaded ''')
  else
    ms('no overload of ''');
  maN(callee.spelling);
  if best <> nil then
    ma(''' with arguments (')
  else
    ma(''' takes arguments (');
  for i := 0 to argTys.count - 1 do
  begin
    if i > 0 then
      ma(', ');
    t := PType(listGet(argTys, i));
    if t <> nil then
      bufAddTypeName(msg, t)
    else
      ma('routine');
  end;
  ma(')');
  errR(callee.loc, rng);
  for i := 0 to cands.count - 1 do
  begin
    r := PDecl(listGet(cands, i));
    if (best = nil) or (callCost(r, args, argTys) = bestCost) then
    begin
      ms('candidate: ');
      maRoutineHeading(r);
      note(r^.loc);
    end;
  end;
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
    begin
      resolvePendingPointers;
      checkPendingClasses;
    end;
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
  checkPendingClasses;
end;

procedure visitStmt(s: PNode); forward;

procedure finishBlock(b: PBlock; warnUnused: boolean);
var
  i, savedHandlers, savedFinally: longint;
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
  checkMethodsImplemented(b);
  savedBody := routineBody;
  routineBody := b^.body;
  savedHandlers := handlerDepth;
  savedFinally := finallyLoopDepth;
  handlerDepth := 0;
  finallyLoopDepth := -1;
  visitStmt(b^.body);
  handlerDepth := savedHandlers;
  finallyLoopDepth := savedFinally;
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
  selfRef: PNode;
  w: PWithContext;
begin
  savedScope := cur;
  savedRoutine := curRoutine;
  if r^.methodKind <> mtNone then
  begin
    { a method sees the members of its class through Self }
    selfRef := newNode(nkNameRef, noLoc, noLoc);
    selfRef^.ref := r^.selfVar^.ident;
    selfRef^.refDecl := r^.selfVar;
    selfRef^.typ := r^.selfVar^.varType;
    w := PWithContext(arenaAlloc(sizeof(TWithContext)));
    w^.rec := selfRef;
    w^.isSelf := true;
    cur := newClassScope(cur, r^.ownerClass, w);
  end;
  cur := newScope(cur);
  curRoutine := r;
  if r^.methodKind <> mtNone then
    scopeInsert(cur, r^.selfVar^.ident.name, r^.selfVar);
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
      if isPredeclared(prev) then
      begin
        ms('''Self'' is the object a method is called on');
        note(p^.loc);
      end
      else
      begin
        ms('previous parameter is here');
        note(prev^.loc);
      end;
    end;
  end;
  { objfpc: Result names the function result }
  if (mode = lmObjFPC) and r^.isFunction and (r^.resultVar <> nil) then
    scopeInsert(cur, intern('result'), r^.resultVar);
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
  d, x: PDecl;
  params: TPtrList;
  res, xr: PType;
  found: boolean;
  n: PNode;
begin
  if formal^.routineResult <> nil then
    kind := 'function'
  else
    kind := 'procedure';
  { Turbo/FPC: @f, as objfpc requires (the operand: a routine's name) }
  n := arg;
  if not isISO and (arg^.kind = nkAddrOf) then
  begin
    arg^.typ := p^.varType;
    n := arg^.inner;
  end;
  d := nil;
  if n^.kind = nkNameRef then
    d := lookup(n^.ref.name);
  listInit(params);
  res := nil;
  found := false;
  if (d <> nil) and (d^.kind = dkRoutine) and (d^.methodKind = mtNone) then
  begin
    { objfpc: of overloaded routines, the one whose heading fits }
    x := d;
    while (d^.overloadIndex <> 0) and (x <> nil) do
    begin
      xr := nil;
      if x^.isFunction then
        xr := x^.resultType;
      if congruent(formal^.routineParams, formal^.routineResult, x^.params, xr) then
      begin
        d := x;
        break;
      end;
      x := x^.nextOverload;
    end;
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
  n^.refDecl := d;
  n^.typ := p^.varType;
  if not congruent(formal^.routineParams, formal^.routineResult, params, res) then
  begin
    ms('''');
    maN(n^.ref.spelling);
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

{ interfaceOnly: u was compiled separately, so only its interface is
  declared, for the units and program using it }
procedure checkUnit(u: PDecl; interfaceOnly: boolean);
var
  iface, impl: PScope;
  i: longint;
  r: PDecl;
begin
  curModule := u;
  iface := newScope(standard);
  importUnits(iface, u^.ifaceUses);
  cur := iface;
  declareAll(u^.iface);
  if interfaceOnly then
  begin
    for i := 0 to u^.iface^.decls.count - 1 do
    begin
      r := PDecl(listGet(u^.iface^.decls, i));
      r^.isUsed := true;
    end;
    mapPut(unitInterfaces, u^.ident.name^.id, iface);
    cur := standard;
    exit;
  end;

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
  checkMethodsImplemented(u^.iface);
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

procedure semaInit(m: TLangMode);
begin
  mode := m;
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
  curModule := nil;
  curAssignTarget := nil;
  tobjectType := nil;
  exceptionType := nil;
  handlerDepth := 0;
  finallyLoopDepth := -1;
  listInit(pendingInterfaces);
  mapInit(classRefs);
  addrOfTarget := nil;
  indexBase := nil;
  listInit(pendingIndexes);
  listInit(pendingClasses);
  standard := newScope(nil);
  cur := standard;
  declareStandardScope;
end;

function semaCheck(prog: PDecl; const units: TPtrList; m: TLangMode;
                   separateUnits: boolean): boolean;
var
  errorsBefore, i: longint;
  programScope: PScope;
begin
  errorsBefore := diagNumErrors;
  semaInit(m);
  for i := 0 to units.count - 1 do
    checkUnit(PDecl(listGet(units, i)), separateUnits);
  checkProgramParams(prog);
  curModule := prog;
  programScope := newScope(standard);
  importUnits(programScope, prog^.progUses);
  cur := programScope;
  checkBlock(prog^.progBlock);
  checkProgramParamDecls(prog);
  checkGotos;
  cur := standard;
  semaCheck := (diagNumErrors = errorsBefore) and not diagShouldStop;
end;

function semaCheckUnit(u: PDecl; const deps: TPtrList; m: TLangMode): boolean;
var
  errorsBefore, i: longint;
begin
  errorsBefore := diagNumErrors;
  semaInit(m);
  for i := 0 to deps.count - 1 do
    checkUnit(PDecl(listGet(deps, i)), true);
  checkUnit(u, false);
  checkGotos;
  semaCheckUnit := (diagNumErrors = errorsBefore) and not diagShouldStop;
end;

end.
