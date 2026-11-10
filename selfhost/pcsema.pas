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
  { check allows a value of an untyped parameter (checkAllowingUntyped) }
  allowUntyped: boolean;
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
  arrayOfConstT, pcharT, tmethodT: PType;
  addrOfWantsCode: boolean; { @TFoo.M for a pointer: its code address }
  allowSlice: boolean;
  { COM interfaces (objfpc): IUnknown (= IInterface), their root, and
    TInterfacedObject, the root of the classes implementing them }
  iunknownType, interfacedType: PType;
  { the advanced record being declared by a type declaration (records with
    methods must be named, so others are rejected) }
  namedAdvancedRecord: PTypeSpec;
  { what a typed constant's or initialized variable's value is called in
    diagnostics (... must be a constant) }
  constInitWhat: string;
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

{ A routine's calling convention in a message (see ccDefault). }
procedure maCC(isCDeclConv: boolean);
begin
  if isCDeclConv then
    ma('''cdecl''')
  else
    ma('''stdcall'' (the default)');
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
      else if d^.methodKind = mtOperator then
        kindName := 'operator'
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

procedure noteDeclaredHere(d: PDecl); forward;

procedure diagUndeclared(const id: TIdent);
var
  r, d: PDecl;
  owner: PType;
begin
  { in a record's static method, the record's instance members }
  r := curRoutine;
  while r <> nil do
  begin
    if r^.isStatic then
    begin
      d := findMember(r^.ownerClass, id.name, owner);
      if d <> nil then
      begin
        ms('instance member ''');
        maN(id.spelling);
        ma(''' cannot be used in a static class method');
        errR(id.loc, identRange(id));
        noteDeclaredHere(d);
        exit;
      end;
      break;
    end;
    r := r^.parent;
  end;
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

{ A use (named id) of d: warnings for its hint directives (deprecated,
  experimental, platform, library, unimplemented), as in Free Pascal. }
procedure warnHints(d: PDecl; const id: TIdent);
var
  h: longint;

  procedure warn(const what: string);
  begin
    ms('''');
    maN(d^.ident.spelling);
    ma('''' + what);
    if what = ' is deprecated' then
      if d^.hintMessage <> nil then
      begin
        ma(': ');
        maN(d^.hintMessage);
      end;
    ma(' [-Wdeprecated]');
    diagReportBuf(sevWarning, id.loc, msg, identRange(id), noRange);
  end;

begin
  if d = nil then
    exit;
  h := d^.hints;
  if (h = 0) or not diagWarningEnabled('deprecated') then
    exit;
  if (h and hintDeprecated) <> 0 then
    warn(' is deprecated');
  if (h and hintExperimental) <> 0 then
    warn(' is experimental');
  if (h and hintPlatform) <> 0 then
    warn(' is not portable (platform-specific)');
  if (h and hintLibrary) <> 0 then
    warn(' belongs to a library');
  if (h and hintUnimplemented) <> 0 then
    warn(' is not implemented');
  noteDeclaredHere(d);
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
                    bkInsert, bkSetLength, bkSlice, bkLength, bkCopy, bkPos, bkConcat, bkUpCase,
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

{ TMethod = record Code, Data: pointer; end (objfpc). }
procedure declareTMethod;
var
  all: TPtrList;
  f, d: PDecl;
  fl: PFieldList;
  k: longint;
begin
  listInit(all);
  fl := PFieldList(arenaAlloc(sizeof(TFieldList)));
  listInit(fl^.fixed);
  fl^.variant := nil;
  for k := 1 to 2 do
  begin
    if k = 1 then
      f := newDecl(dkField, memberIdent('Code'), noLoc)
    else
      f := newDecl(dkField, memberIdent('Data'), noLoc);
    f^.fieldType := tyUntypedPointerT;
    listAdd(all, f);
    listAdd(fl^.fixed, f);
  end;
  tmethodT := newRecordType(fl, all, false);
  setDeclName(tmethodT, intern('TMethod'));
  d := newDecl(dkType, memberIdent('TMethod'), noLoc);
  d^.declType := tmethodT;
  scopeInsert(standard, d^.ident.name, d);
end;

{ TVarRec, the elements of an array of const, and the vt* constants:
    record VType: int64; case of ... VInteger, VBoolean, VChar, ... end }
procedure declareVarRec;
var
  all, vs: TPtrList;
  vtype, f, d: PDecl;
  fl: PFieldList;
  v: PVariant;
  vp: PVariantPart;
  rec: PType;

  procedure addVariant(const name: string; t: PType);
  var
    one: PFieldList;
  begin
    f := newDecl(dkField, memberIdent(name), noLoc);
    f^.fieldType := t;
    listAdd(all, f);
    one := PFieldList(arenaAlloc(sizeof(TFieldList)));
    listInit(one^.fixed);
    listAdd(one^.fixed, f);
    one^.variant := nil;
    v := PVariant(arenaAlloc(sizeof(TVariant)));
    listInit(v^.labels);
    v^.fields := one;
    listAdd(vs, v);
  end;

begin
  listInit(all);
  listInit(vs);
  vtype := newDecl(dkField, memberIdent('VType'), noLoc);
  vtype^.fieldType := tyInt64T;
  listAdd(all, vtype);
  addVariant('VInteger', tyIntegerT);
  addVariant('VBoolean', tyBooleanT);
  addVariant('VChar', tyCharT);
  addVariant('VExtended', newPointerType(tyRealT));
  addVariant('VString', newPointerType(shortStringType(255)));
  addVariant('VPointer', tyUntypedPointerT);
  addVariant('VPChar', pcharT);
  addVariant('VAnsiString', tyUntypedPointerT);
  addVariant('VInt64', newPointerType(tyInt64T));
  if mode = lmObjFPC then { classes exist }
  begin
    addVariant('VObject', tobjectType);
    addVariant('VClass', classRefOf(tobjectType));
  end;
  vp := PVariantPart(arenaAlloc(sizeof(TVariantPart)));
  vp^.tag := nil;
  vp^.tagSpec := nil;
  vp^.tagType := nil;
  vp^.variants := vs;
  fl := PFieldList(arenaAlloc(sizeof(TFieldList)));
  listInit(fl^.fixed);
  listAdd(fl^.fixed, vtype);
  fl^.variant := vp;
  rec := newRecordType(fl, all, false);
  setDeclName(rec, intern('TVarRec'));
  d := newDecl(dkType, memberIdent('TVarRec'), noLoc);
  d^.declType := rec;
  scopeInsert(standard, d^.ident.name, d);
  arrayOfConstT := newType(tyArrayOfConst);
  arrayOfConstT^.aocElement := rec;
  stdConst('vtinteger', tyIntegerT, 0);
  stdConst('vtboolean', tyIntegerT, 1);
  stdConst('vtchar', tyIntegerT, 2);
  stdConst('vtextended', tyIntegerT, 3);
  stdConst('vtstring', tyIntegerT, 4);
  stdConst('vtpointer', tyIntegerT, 5);
  stdConst('vtpchar', tyIntegerT, 6);
  stdConst('vtobject', tyIntegerT, 7);
  stdConst('vtclass', tyIntegerT, 8);
  stdConst('vtansistring', tyIntegerT, 11);
  stdConst('vtint64', tyIntegerT, 16);
end;

procedure declareException;
var
  members, fields: TPtrList;
  msgField, param, create, message, d, fmtMsg, fmtArgs, createFmt: PDecl;
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
  fmtMsg := newDecl(dkVar, memberIdent('msg'), noLoc);
  fmtMsg^.varKind := vkConstParam;
  fmtMsg^.varType := shortStringType(255);
  fmtArgs := newDecl(dkVar, memberIdent('args'), noLoc);
  fmtArgs^.varKind := vkConstParam;
  fmtArgs^.varType := arrayOfConstT;
  createFmt := newTObjectMethod('CreateFmt', mtConstructor, nil, obExceptionCreateFmt);
  createFmt^.ownerClass := exceptionType;
  createFmt^.hasParamList := true;
  listAdd(createFmt^.params, fmtMsg);
  listAdd(createFmt^.params, fmtArgs);
  message := newDecl(dkProperty, memberIdent('Message'), noLoc);
  message^.propType := shortStringType(255);
  message^.readName := memberIdent('FMessage');
  message^.writeName := memberIdent('FMessage');
  message^.readDecl := msgField;
  message^.writeDecl := msgField;
  listInit(members);
  listAdd(members, msgField);
  listAdd(members, create);
  listAdd(members, createFmt);
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
  external_ := declareExceptionClass('EConvertError', exceptionType);
end;

{ IUnknown = interface end (COM; also named IInterface: Free Pascal's
  QueryInterface, _AddRef and _Release are not declared, the compiled code
  counts references itself) and
    TInterfacedObject = class(TObject, IUnknown)
    protected FRefCount: longint;
    public property RefCount: longint read FRefCount; end;
  Their iid and VMT are in the runtime. }
procedure declareInterfacedObject;
var
  empty, members, fields, impls: TPtrList;
  count, refCount, d: PDecl;
  impl: PInterfaceImpl;
begin
  iunknownType := newInterfaceType;
  setDeclName(iunknownType, intern('IUnknown'));
  iunknownType^.intfCOM := true;
  listInit(empty);
  defineInterface(iunknownType, nil, empty, empty, nil);
  d := newDecl(dkType, memberIdent('IUnknown'), noLoc);
  d^.declType := iunknownType;
  scopeInsert(standard, d^.ident.name, d);
  d := newDecl(dkType, memberIdent('IInterface'), noLoc);
  d^.declType := iunknownType;
  scopeInsert(standard, d^.ident.name, d);

  interfacedType := newClassType;
  setDeclName(interfacedType, intern('TInterfacedObject'));
  count := newDecl(dkField, memberIdent('FRefCount'), noLoc);
  count^.fieldType := tyIntegerT;
  count^.vis := vsProtected;
  refCount := newDecl(dkProperty, memberIdent('RefCount'), noLoc);
  refCount^.propType := tyIntegerT;
  refCount^.readName := memberIdent('FRefCount');
  refCount^.writeName := noIdent;
  refCount^.readDecl := count;
  refCount^.writeDecl := nil;
  listInit(members);
  listAdd(members, count);
  listAdd(members, refCount);
  listInit(fields);
  listAdd(fields, count);
  defineClass(interfacedType, tobjectType, members, fields,
              tobjectType^.classVTable, nil);
  impl := PInterfaceImpl(arenaAlloc(sizeof(TInterfaceImpl)));
  impl^.intf := iunknownType;
  listInit(impl^.methods);
  listInit(impls);
  listAdd(impls, impl);
  interfacedType^.classInterfaces := impls;
  d := newDecl(dkType, memberIdent('TInterfacedObject'), noLoc);
  d^.declType := interfacedType;
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
    pcharT := pchar;
    stdType('ansistring', ansiStringType);
    stdType('shortstring', shortStringType(255));
  end;
  if mode = lmTP then
    declareVarRec;
  if mode = lmObjFPC then
  begin
    declareTObject;
    declareTMethod;
    declareVarRec;
    declareException;
    declareInterfacedObject;
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

{ Can parameters params take n arguments (the rest from default values)? }
function acceptsArgCount(const params: TPtrList; n: longint): boolean;
begin
  acceptsArgCount := (n = params.count) or
    ((n < params.count) and (PDecl(listGet(params, n))^.paramDefault <> nil));
end;

function fold(e: PNode; var c: TConstant): boolean; forward;
procedure checkParamDefault(p: PDecl); forward;

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
{ The current routine has a try statement (see hasTry). }
procedure markHasTry;
begin
  if curRoutine = nil then
    exit;
  curRoutine^.hasTry := true;
  if curRoutine^.canonical <> nil then
    curRoutine^.canonical^.hasTry := true;
end;

{ The current routine computes a managed value (see usesManaged). }
procedure markUsesManaged;
begin
  if curRoutine = nil then
    exit;
  curRoutine^.usesManaged := true;
  if curRoutine^.canonical <> nil then
    curRoutine^.canonical^.usesManaged := true;
end;

function check(e: PNode): PType;
var
  t: PType;
  allow: boolean;
  nameS: string;
begin
  allow := allowUntyped;
  allowUntyped := false;
  if e^.typ <> nil then
    exit(e^.typ);
  t := visitExpr(e);
  if (t^.kind = tyUntyped) and not allow then
  begin
    if e^.kind = nkNameRef then
      nameS := nameStr(e^.ref.spelling)
    else
      nameS := '?';
    ms('''' + nameS + ''' is an untyped parameter: use a typecast such as longint(');
    if e^.kind = nkNameRef then
      ma(nameS)
    else
      ma('x');
    ma('), its address, or pass it to an untyped parameter');
    errR(e^.b, nodeRange(e));
    t := errorType;
  end;
  e^.typ := t;
  if isManaged(t) then
    markUsesManaged;
  check := t;
end;

{ Untyped parameters (Turbo/FPC  var x;  const x) can only be typecast,
  have their address taken, or be passed to untyped parameters (and to
  move and fillchar): those operands are checked with this. }
function checkAllowingUntyped(e: PNode): PType;
begin
  allowUntyped := true;
  checkAllowingUntyped := check(e);
  allowUntyped := false;
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
  if ((a^.kind = tyDynArray) and (b^.kind = tyNil)) or
     ((a^.kind = tyNil) and (b^.kind = tyDynArray)) then
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
function isPChar(t: PType): boolean;
begin
  isPChar := (t^.kind = tyPointer) and not t^.untyped and (t^.pointee <> nil) and
             (t^.pointee^.kind = tyChar);
end;

function isAssignable(target, value: PType): boolean;
begin
  if (target^.kind = tyOpenArray) or (value^.kind = tyOpenArray) then
    exit(false); { only passed along and indexed }
  if (target^.kind = tyDynArray) and (target <> value) then
    exit(value^.kind = tyNil);
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
  if (target^.kind = tyPointer) and (value^.kind in [tyClass, tyClassRef]) then
    exit(target^.untyped);
  if (target^.kind = tyShortString) and isStringish(value) then
    exit(true); { truncated to the target's length at run time }
  if target^.kind = tyAnsiString then
    exit(isStringish(value) or isPChar(value));
  if (target^.kind = tySet) and (value^.kind = tySet) then
    exit(areCompatible(target, value));
  { procedural values (Turbo/FPC): congruent headings, or nil }
  if target^.kind = tyRoutine then
  begin
    if value^.kind = tyNil then
      exit(true);
    exit((value^.kind = tyRoutine) and
         (value^.routineCDecl = target^.routineCDecl) and
         (value^.routineOfObject = target^.routineOfObject) and
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
      if (e^.base^.typ <> nil) and (e^.base^.typ^.kind = tyDynArray) then
        isVariableReference := e^.idxProp = nil { on the heap, like p^ }
      else
        isVariableReference := (e^.idxProp = nil) and isVariableReference(e^.base);
    nkDeref:
      isVariableReference := true; { p^ is a variable even for a call's result }
    nkCall:
      isVariableReference := e^.isVarCast or (e^.methodCast = 2); { T(x) of an untyped parameter x }
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
          { (a dynamic array's elements are not the parameter) }
          if (e^.idxProp <> nil) or (e^.base^.typ^.kind = tyDynArray) then
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
      nkCall:
        begin
          if not e^.isVarCast and (e^.methodCast = 0) then
            exit(nil);
          e := PNode(listGet(e^.args, 0)); { T(x): x }
        end;
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
          if at^.kind in [tyArrayOfConst, tyOpenArray, tyDynArray] then
            exit; { known only when the program runs }
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
  i, j, first: longint;
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
    first := all.count;
    if not resolveFieldList(v^.fields, all, seen) then
      ok := false;
    { the variants overlap, so their fields cannot be reference counted }
    for j := first to all.count - 1 do
    begin
      f := PDecl(listGet(all, j));
      if (f^.fieldType <> nil) and isManaged(f^.fieldType) then
      begin
        if f^.fieldType^.kind = tyAnsiString then
          ms('a variant part cannot contain ansistrings')
        else if f^.fieldType^.kind = tyDynArray then
          ms('a variant part cannot contain dynamic arrays')
        else
          ms('a variant part cannot contain COM interfaces');
        errR(f^.loc, identRange(f^.ident));
        ok := false;
      end;
    end;
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
    tsArrayOfConst:
      buildType := arrayOfConstT;
    tsDynArray:
      begin
        elem := resolveType(ts^.dynSpec, nil);
        if isError(elem) then
          exit;
        if containsFile(elem) then
        begin
          ms('the elements of a dynamic array cannot be files');
          errR(ts^.dynSpec^.b, specRange(ts^.dynSpec));
          exit;
        end;
        buildType := dynArrayType(elem);
      end;
    tsOpenArray:
      begin
        elem := resolveType(ts^.openSpec, nil);
        if isError(elem) then
          exit;
        if containsFile(elem) then
        begin
          ms('the elements of an open array cannot be files');
          errR(ts^.openSpec^.b, specRange(ts^.openSpec));
          exit;
        end;
        buildType := openArrayType(elem);
      end;
    tsUntyped:
      buildType := tyUntypedT;
    tsNamed:
      begin
        d := lookup(ts^.tname.name);
        if (d <> nil) and (d^.kind = dkType) then
          warnHints(d, ts^.tname);
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
          checkParamDefault(p);
        end;
        res := nil;
        if ts^.rtIsFunction then
        begin
          res := resolveType(ts^.rtResult, nil);
          if not isError(res) and not isValidResultType(res) and
             not (ts^.rtOfObject and
                  ((res^.kind = tyShortString) or isMethodPointer(res) or
                   ((res^.kind = tyRecord) and not containsFile(res) and
                    not containsInterface(res)))) then
          begin
            ms('function result type must be a simple or pointer type, not ');
            maT(res);
            errR(ts^.rtResult^.b, specRange(ts^.rtResult));
            res := errorType;
          end;
        end;
        if ts^.rtOfObject then
          buildType := newMethodPointerType(ts^.rtParams, res, ts^.rtCallConv = ccCDecl)
        else
          buildType := newRoutineType(ts^.rtParams, res, ts^.rtCallConv = ccCDecl);
      end;
    tsString:
      begin
        if ts^.maxLen = nil then
        begin
          if ts^.isLong then
            exit(ansiStringType);
          exit(shortStringType(255));
        end;
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
        if isManaged(elem) then
        begin
          if elem^.kind = tyAnsiString then
            ms('the components of a file cannot be ansistrings')
          else if elem^.kind = tyDynArray then
            ms('the components of a file cannot be dynamic arrays')
          else
            ms('the components of a file cannot be COM interfaces');
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
        if (ts^.recMembers.count > 0) and (ts <> namedAdvancedRecord) then
        begin
          ms('methods are not allowed in anonymous or local records; declare ' +
             'the record type in a ''type'' section at program or unit level');
          errR(ts^.b, specRange(ts));
        end;
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
    tsInterface:
      begin
        ms('an interface type must be declared in a ''type'' section');
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
procedure declareAdvancedRecord(td: PDecl); forward;

procedure declareType(td: PDecl);
begin
  if (td^.typeSpec^.kind = tsRecord) and (td^.typeSpec^.recMembers.count > 0) and
     (curRoutine = nil) then
  begin
    declareAdvancedRecord(td);
    exit;
  end;
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
{ compareOut false: out and var parameters count as the same, as when
  looking for routines that overloads cannot tell apart }
function sameParamList(a, b: PDecl; compareOut: boolean): boolean; forward;

{ The word for a by-reference parameter, for diagnostics. }
function paramKeyword(p: PDecl): string;
begin
  if p^.isOut then
    paramKeyword := 'out'
  else
    paramKeyword := 'var';
end;

function sameDefaults(heading, impl: PDecl): boolean; forward;
function isPCharLiteral(target: PType; value: PNode): boolean; forward;
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
  { a record's method gets the record by reference; in a class method,
    Self is the class (a class reference) }
  if c^.classRecord <> nil then
  begin
    self_^.varKind := vkVarParam;
    self_^.varType := c^.classRecord;
  end
  else if r^.isClassMethod then
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
    if r^.callConv = ccDefault then
    begin
      if isCDecl(inh) then
        r^.inheritedCDecl := true; { as in Free Pascal }
    end
    else if isCDecl(r) <> isCDecl(inh) then
    begin
      ms('calling convention of ''');
      maN(r^.ident.spelling);
      ma(''' is ');
      maCC(isCDecl(r));
      ma(' but the method it overrides in ');
      maT(owner);
      ma(' is ');
      maCC(isCDecl(inh));
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
  if (d = nil) and (parentClass <> nil) then
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
  t^.intfCOM := not ts^.isCorba;
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

  function kindName(t: PType): string;
  begin
    if t^.intfCOM then
      kindName := 'COM'
    else
      kindName := 'CORBA';
  end;

begin
  parentIntf := nil;
  i^.intfCOM := not ts^.isCorba;
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
    if (parentIntf <> nil) and (parentIntf^.intfCOM <> i^.intfCOM) then
    begin
      ms('a ' + kindName(i) + ' interface cannot inherit from the ' +
         kindName(parentIntf) + ' interface ');
      maT(parentIntf);
      errR(ts^.intfParentName.loc, identRange(ts^.intfParentName));
      parentIntf := nil;
    end;
  end;
  { a COM interface without a parent descends from IUnknown }
  if (parentIntf = nil) and i^.intfCOM then
    parentIntf := iunknownType;
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
           (not m^.isFunction or (r^.resultType = m^.resultType)) and
           (isCDecl(r) = isCDecl(m)) then
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
  { the objects of COM interfaces are reference counted by
    TInterfacedObject }
  for k := 0 to listed.count - 1 do
    if PType(listGet(listed, k))^.intfCOM and
       not inheritsFrom(parentClass, interfacedType) then
    begin
      ms('class ');
      maT(c);
      ma(' implements the COM interface ');
      maT(PType(listGet(listed, k)));
      ma(', so it must descend from TInterfacedObject');
      errR(PIdent(listGet(listedWhere, k))^.loc,
           identRange(PIdent(listGet(listedWhere, k))^));
      break;
    end;

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

{ --- Advanced records (objfpc, $modeswitch advancedrecords) --- }

{ A class operator's parameters: two (binary), one (unary - + not, and the
  conversions := and Explicit); no var parameters; the record among the
  parameter or result types. }
procedure checkOperatorHeading(r: PDecl; rt: PType);
var
  sym: string;
  n, i: longint;
  unary, either, hasRecord: boolean;
  p: PDecl;
begin
  sym := copy(nameStr(r^.ident.name), 9, 255);
  n := r^.params.count;
  unary := (sym = 'not') or (sym = ':=') or (sym = 'explicit');
  either := (sym = '-') or (sym = '+');
  if (unary and (n <> 1)) or (not unary and either and (n <> 1) and (n <> 2)) or
     (not unary and not either and (n <> 2)) then
  begin
    ms('''');
    maN(r^.ident.spelling);
    if unary then
      ma(''' takes one parameter')
    else if either then
      ma(''' takes one or two parameters')
    else
      ma(''' takes two parameters');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  hasRecord := r^.resultType = rt;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    if p^.varKind = vkVarParam then
    begin
      ms('the parameters of an operator cannot be ''var''');
      errR(p^.loc, identRange(p^.ident));
    end;
    if p^.varType = rt then
      hasRecord := true;
  end;
  if not hasRecord then
  begin
    ms('an operator of ');
    maT(rt);
    ma(' needs a parameter or result of that type');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
end;

{ A method, class method (static) or class operator of record rt. }
procedure defineRecordMethodT(c, rt: PType; r: PDecl);
begin
  r^.ownerClass := c;
  r^.parent := nil;
  r^.level := 1;
  resolveSignature(r);
  if r^.isVirtual or r^.isOverride or r^.isAbstract then
  begin
    ms('the methods of a record cannot be ''virtual'', ''override'' or ' +
       '''abstract''');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  if r^.isClassMethod then
    r^.isStatic := true { no Self }
  else
    makeSelf(r, c);
  if (r^.methodKind = mtConstructor) and (r^.params.count = 0) then
  begin
    ms('a record''s constructor needs parameters');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  if r^.methodKind = mtOperator then
    checkOperatorHeading(r, rt);
end;

procedure defineRecordMembers(rt: PType; ts: PTypeSpec);
var
  c: PType;
  members, noVTable: TPtrList;
  own: TIntMap;
  i: longint;
  f, m, prev, overloadOf: PDecl;
begin
  c := newClassType;
  setDeclName(c, rt^.declName);
  c^.classRecord := rt;
  rt^.memberTable := c;
  listInit(members);
  mapInit(own);
  for i := 0 to rt^.allFields.count - 1 do
  begin
    f := PDecl(listGet(rt^.allFields, i));
    listAdd(members, f);
    if mapGet(own, f^.ident.name^.id) = nil then
      mapPut(own, f^.ident.name^.id, f);
  end;
  for i := 0 to ts^.recMembers.count - 1 do
  begin
    m := PDecl(listGet(ts^.recMembers, i));
    overloadOf := nil;
    prev := PDecl(mapGet(own, m^.ident.name^.id));
    if prev <> nil then
    begin
      if prev^.kind = dkRoutine then
        overloadOf := prev;
      if (overloadOf = nil) or (m^.kind <> dkRoutine) then
      begin
        ms('redefinition of ''');
        maN(m^.ident.spelling);
        ma('''');
        errR(m^.ident.loc, identRange(m^.ident));
        ms('previous definition is here');
        note(prev^.loc);
        continue;
      end;
    end
    else
      mapPut(own, m^.ident.name^.id, m);
    listAdd(members, m);
    if m^.kind = dkRoutine then
    begin
      defineRecordMethodT(c, rt, m);
      if (overloadOf <> nil) and not addOverload(overloadOf, m) then
      begin
        m^.ownerClass := nil; { rejected: not a member }
        listPop(members);
      end;
      continue;
    end;
    if m^.propIsDefault then
    begin
      ms('a record cannot have a default property');
      errR(m^.loc, identRange(m^.ident));
    end;
    definePropertyAccessors(c, nil, m, own);
  end;
  listInit(noVTable);
  defineClass(c, nil, members, rt^.allFields, noVTable, curModule);
end;

{ TR = record fields; methods; operators; properties end; -- the record type
  (its fields), then its member table }
procedure declareAdvancedRecord(td: PDecl);
var
  saved: PTypeSpec;
  t: PType;
begin
  saved := namedAdvancedRecord;
  namedAdvancedRecord := td^.typeSpec;
  t := resolveType(td^.typeSpec, td^.ident.spelling);
  namedAdvancedRecord := saved;
  td^.declType := t;
  declare(td); { its methods' headings may name it }
  if t^.kind = tyRecord then
    defineRecordMembers(t, td^.typeSpec);
end;

function memberTableOf(t: PType): PType;
begin
  if (t <> nil) and (t^.kind = tyRecord) then
    memberTableOf := t^.memberTable
  else
    memberTableOf := nil;
end;

procedure setConversion(n: PNode; op: PDecl; t: PType);
begin
  n^.convOp := op;
  n^.convFrom := n^.typ;
  n^.typ := t;
end;

{ A user-defined conversion of a value of type from_ to to_: a := operator
  (explicit_: an Explicit one first) of either record whose parameter takes
  from_ (exactly, else by assignment) and whose result is to_; nil if none. }
function findConversion(from_, to_: PType; explicit_: boolean): PDecl;
var
  pass, side: longint;
  name: string;
  best, r: PDecl;
  bestExact, exact: boolean;
  t, mt, pt: PType;
begin
  findConversion := nil;
  if (mode <> lmObjFPC) or (from_ = nil) or (to_ = nil) then
    exit;
  for pass := 1 to 2 do
  begin
    if pass = 1 then
      name := 'operatorexplicit'
    else
      name := 'operator:=';
    if (pass = 1) and not explicit_ then
      continue;
    best := nil;
    bestExact := false;
    for side := 1 to 2 do
    begin
      if side = 1 then
        t := from_
      else
        t := to_;
      mt := memberTableOf(t);
      if (mt = nil) or ((t = to_) and (to_ = from_)) then
        continue;
      r := findOwnMember(mt, intern(name));
      while r <> nil do
      begin
        pt := nil;
        if r^.params.count = 1 then
          pt := PDecl(listGet(r^.params, 0))^.varType;
        if (r^.methodKind = mtOperator) and (pt <> nil) and (r^.resultType = to_) then
        begin
          exact := pt = from_;
          if exact or isAssignable(pt, from_) then
            if (best = nil) or (exact and not bestExact) then
            begin
              best := r;
              bestExact := exact;
            end;
        end;
        r := r^.nextOverload;
      end;
    end;
    if best <> nil then
      exit(best);
  end;
end;

{ Converts value to t by a user-defined := operator if there is one. }
function convertImplicitly(value: PNode; t: PType): boolean;
var
  op: PDecl;
begin
  op := findConversion(value^.typ, t, false);
  if op = nil then
    exit(false);
  setConversion(value, op, t);
  convertImplicitly := true;
end;

{ The class operator name of the records among args' types that fits args
  best (exact types, then assignable ones, then ones converted by a :=
  operator), with the conversions it needs annotated; nil if none fits.
  found: whether either record has an operator name of that arity. }
function resolveOperator(const name: string; const args: TPtrList;
                         var found: boolean): PDecl;
var
  tables: TPtrList;
  i, j, cost, bestCost: longint;
  mt, pt, at: PType;
  best, r: PDecl;
  ok, seen: boolean;
begin
  listInit(tables);
  for i := 0 to args.count - 1 do
  begin
    mt := memberTableOf(PNode(listGet(args, i))^.typ);
    if mt <> nil then
    begin
      seen := false;
      for j := 0 to tables.count - 1 do
        if listGet(tables, j) = pointer(mt) then
          seen := true;
      if not seen then
        listAdd(tables, mt);
    end;
  end;
  best := nil;
  bestCost := 0;
  for j := 0 to tables.count - 1 do
  begin
    r := findOwnMember(PType(listGet(tables, j)), intern(name));
    while r <> nil do
    begin
      if (r^.methodKind = mtOperator) and (r^.params.count = args.count) then
      begin
        found := true;
        cost := 0;
        ok := true;
        i := 0;
        while (i < args.count) and ok do
        begin
          pt := PDecl(listGet(r^.params, i))^.varType;
          at := PNode(listGet(args, i))^.typ;
          if (pt = nil) or isError(pt) then
            ok := false
          else if pt = at then
          else if isAssignable(pt, at) then
            cost := cost + 1
          else if findConversion(at, pt, false) <> nil then
            cost := cost + 100
          else
            ok := false;
          i := i + 1;
        end;
        if ok and ((best = nil) or (cost < bestCost)) then
        begin
          best := r;
          bestCost := cost;
        end;
      end;
      r := r^.nextOverload;
    end;
  end;
  if best <> nil then
    for i := 0 to args.count - 1 do
    begin
      pt := PDecl(listGet(best^.params, i))^.varType;
      at := PNode(listGet(args, i))^.typ;
      if (pt <> at) and not isAssignable(pt, at) then
        convertImplicitly(PNode(listGet(args, i)), pt);
    end;
  listFree(tables);
  resolveOperator := best;
end;

function operatorSymbol(op: TBinaryOp): string;
begin
  case op of
    boAdd: operatorSymbol := '+';
    boSub: operatorSymbol := '-';
    boMul: operatorSymbol := '*';
    boRealDiv: operatorSymbol := '/';
    boIntDiv: operatorSymbol := 'div';
    boMod: operatorSymbol := 'mod';
    boAnd: operatorSymbol := 'and';
    boOr: operatorSymbol := 'or';
    boXor: operatorSymbol := 'xor';
    boShl: operatorSymbol := 'shl';
    boShr: operatorSymbol := 'shr';
    boEq: operatorSymbol := '=';
    boNe: operatorSymbol := '<>';
    boLt: operatorSymbol := '<';
    boGt: operatorSymbol := '>';
    boLe: operatorSymbol := '<=';
    boGe: operatorSymbol := '>=';
    boIn: operatorSymbol := 'in';
  else
    operatorSymbol := '';
  end;
end;

{ l op r where an operand is an advanced record: its class operator (<>
  may use =, negated). }
function checkRecordBinary(e: PNode): PType;
var
  sym: string;
  args: TPtrList;
  found, negate: boolean;
  op: PDecl;
begin
  sym := operatorSymbol(e^.bop);
  listInit(args);
  listAdd(args, e^.lhs);
  listAdd(args, e^.rhs);
  found := false;
  op := nil;
  if sym <> '' then
    op := resolveOperator('operator' + sym, args, found);
  negate := false;
  if (op = nil) and (e^.bop = boNe) then
  begin
    op := resolveOperator('operator=', args, found);
    negate := op <> nil;
  end;
  listFree(args);
  if op = nil then
  begin
    ms('operator ''' + binaryOpSpelling(e^.bop) + ''' is not overloaded for ');
    maT(e^.lhs^.typ);
    ma(' and ');
    maT(e^.rhs^.typ);
    errRR(e^.opLoc, nodeRange(e^.lhs), nodeRange(e^.rhs));
    exit(errorType);
  end;
  e^.opDecl := op;
  e^.opNegated := negate;
  if negate then
    exit(tyBooleanT);
  checkRecordBinary := op^.resultType;
end;

{ op x where x is an advanced record: its class operator. }
function checkRecordUnary(e: PNode): PType;
var
  sym: string;
  args: TPtrList;
  found: boolean;
  op: PDecl;
begin
  if e^.uop = uoNot then
    sym := 'not'
  else if e^.uop = uoMinus then
    sym := '-'
  else
    sym := '+';
  listInit(args);
  listAdd(args, e^.operand);
  found := false;
  op := resolveOperator('operator' + sym, args, found);
  listFree(args);
  if op = nil then
  begin
    ms('operator ''' + unaryOpSpelling(e^.uop) + ''' is not overloaded for ');
    maT(e^.operand^.typ);
    errR(e^.b, nodeRange(e^.operand));
    exit(errorType);
  end;
  e^.uopDecl := op;
  checkRecordUnary := op^.resultType;
end;

{ A record's := or Explicit operator: its overloads may differ in the result
  type alone. }
function isConversionOperator(r: PDecl): boolean;
begin
  isConversionOperator := (r^.methodKind = mtOperator) and
    ((nameStr(r^.ident.name) = 'operator:=') or
     (nameStr(r^.ident.name) = 'operatorexplicit'));
end;

procedure declareMethodBody(r: PDecl);
var
  c, rt, convRT: PType;
  m, p, x, match, td: PDecl;
  i: longint;
  what: string;
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
  c := nil;
  td := lookup(r^.methodClassName.name);
  if (td <> nil) and (td^.kind = dkType) then
    c := memberTableOf(td^.declType); { an advanced record }
  if c = nil then
    c := lookupClass(r^.methodClassName);
  if c = nil then
    exit;
  if c^.classRecord <> nil then
    what := 'record'
  else
    what := 'class';
  m := findOwnMember(c, r^.ident.name);
  if (m <> nil) and ((m^.kind <> dkRoutine) or (m^.objMethod <> obNone)) then
    m := nil;
  if m = nil then
  begin
    ms(what + ' ');
    maT(c);
    if r^.methodKind = mtOperator then
      ma(' has no operator ''')
    else
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
    convRT := nil;
    if isConversionOperator(m) and (r^.resultSpec <> nil) then
      convRT := resolveType(r^.resultSpec, nil);
    match := nil;
    x := m;
    while x <> nil do
    begin
      if sameSignature(x, r) and (x^.body = nil) and
         ((convRT = nil) or (convRT = x^.resultType)) then
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
    ma(''' must be implemented where its ' + what + ' is declared');
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
    if not sameSignature(m, r) or ((rt <> nil) and (rt <> m^.resultType)) or
       not sameDefaults(m, r) then
    begin
      ms('heading of ''');
      maQualified(c, m^.ident);
      ma(''' does not match its declaration');
      errR(r^.ident.loc, identRange(r^.ident));
      noteDeclaredHere(m);
    end;
  end;
  if isCDecl(r) <> isCDecl(m) then
  begin
    ms('calling convention of ''');
    maQualified(c, m^.ident);
    ma(''' is ');
    maCC(isCDecl(r));
    ma(' but its declaration''s is ');
    maCC(isCDecl(m));
    errR(r^.ident.loc, identRange(r^.ident));
    noteDeclaredHere(m);
  end;
  m^.body := r^.body;
  m^.implDecl := r;
  r^.canonical := m;
  if r^.isInline then
    m^.isInline := true;
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
    if d^.kind = dkType then
    begin
      c := memberTableOf(d^.declType);
      if c <> nil then { an advanced record's }
      begin
        for j := 0 to c^.classMembers.count - 1 do
        begin
          r := PDecl(listGet(c^.classMembers, j));
          if (r^.kind = dkRoutine) and (r^.body = nil) then
          begin
            ms('method ''');
            maQualified(c, r^.ident);
            ma(''' is declared but never implemented');
            errR(r^.ident.loc, identRange(r^.ident));
          end;
        end;
        continue;
      end;
    end;
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

{ Does with context w expose methods (an object's, a class's in a class
  method, or an advanced record's)? }
function hasMembers(w: PWithContext): boolean;
begin
  hasMembers := (w <> nil) and
    ((classOfWith(w) <> nil) or (memberTableOf(w^.rec^.typ) <> nil));
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
  ignoredT: PType;
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

  if isMethodPointer(t) or (t^.kind = tyDynArray) then { only nil }
  begin
    if init^.kind = nkNil then
    begin
      ignoredT := check(init);
      exit(true);
    end;
    ms('typed constants of type ');
    maT(t);
    ma(' can only be initialized with nil');
    errR(init^.b, nodeRange(init));
    exit(false);
  end;
  if not evaluateConstant(init, constInitWhat, c) then
    exit;
  charArray := (c.value.kind = ckString) and isCharArrayOfLength(t, c.value.strVal^.len);
  if not charArray and not (isSimple(t) or (t^.kind in [tyShortString, tyAnsiString]) or
                            isStringType(t)) then
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

{ The variable that e, the target of an absolute, is in: e is a variable, a
  field of a record in one or an element at a constant index of an array
  in one. Nil for anything else. }
function absoluteRootOf(e: PNode): PDecl;
var
  c: TConstant;
begin
  absoluteRootOf := nil;
  case e^.kind of
    nkNameRef:
      if (e^.refDecl <> nil) and (e^.refDecl^.kind = dkVar) and (e^.refWith = nil) and
         (e^.refDecl^.varType^.kind <> tyArrayOfConst) then
        absoluteRootOf := e^.refDecl;
    nkField:
      if (e^.base^.typ^.kind = tyRecord) and (e^.fieldDecl <> nil) and
         (e^.fieldDecl^.kind = dkField) then
        absoluteRootOf := absoluteRootOf(e^.base);
    nkIndex:
      if (e^.idxProp = nil) and (e^.base^.typ^.kind = tyArray) and fold(e^.index, c) then
        absoluteRootOf := absoluteRootOf(e^.base);
  end;
end;

{ v: T absolute target, as in Free Pascal. Checked before v is declared, so
  that it cannot name itself. }
procedure checkAbsolute(v: PDecl; target: PNode);
var
  r, enc, x, root: PDecl;
begin
  { the name of an enclosing function is its result }
  r := nil;
  if target^.kind = nkNameRef then
    r := scopeFind(cur, target^.ref.name);
  if (r <> nil) and (r^.kind = dkRoutine) and r^.isFunction then
  begin
    enc := curRoutine;
    while enc <> nil do
    begin
      x := r;
      while x <> nil do
      begin
        if enc = x then
        begin
          if x <> curRoutine then
            x^.resultVar^.captured := true;
          target^.refDecl := x^.resultVar;
          target^.typ := x^.resultType;
          x^.resultAssigned := true;
          v^.absRoot := x^.resultVar;
          exit;
        end;
        x := x^.nextOverload;
      end;
      enc := enc^.parent;
    end;
  end;
  if isError(checkAllowingUntyped(target)) then
    exit;
  root := absoluteRootOf(target);
  if root = nil then
  begin
    if (target^.kind = nkIntegerLit) or
       ((target^.kind = nkNameRef) and (target^.refDecl <> nil) and
        (target^.refDecl^.kind = dkConst)) then
      ms('''absolute'' with a memory address is not supported')
    else
      ms('''absolute'' needs a variable, a field of a record variable or an element ' +
         'at a constant index');
    errR(target^.b, nodeRange(target));
    exit;
  end;
  v^.absRoot := root;
  if root^.varKind = vkResult then { objfpc Result }
    root^.owner^.resultAssigned := true;
end;

procedure declareVar(v: PDecl);
var
  folded: TConstValue;
begin
  v^.varType := resolveType(v^.varSpec, nil);
  v^.owner := curRoutine;
  if v^.absTarget <> nil then
    checkAbsolute(v, v^.absTarget);
  if v^.init <> nil then
  begin
    { typed constants are static: they keep their value across calls (an
      initialized variable is set again whenever its routine runs) }
    if not v^.isInitVar then
    begin
      v^.owner := nil;
      v^.isUsed := true;
    end;
    folded := constNone;
    if v^.isInitVar then
      constInitWhat := 'initial value'
    else
      constInitWhat := 'typed constant value';
    if not isError(v^.varType) and checkConstInit(v^.init, v^.varType, folded) then
      v^.initValue := folded;
    constInitWhat := 'typed constant value';
  end;
  declare(v);
end;

{ Resolves a routine's parameter and result types. }
{ Is e, a checked expression, a constant (nil, a set of constants, a
  concatenation of string constants)? }
function isConstantDefault(e: PNode): boolean;
var
  i: longint;
  el: PSetElement;
  c: TConstant;
begin
  if e^.kind = nkNil then
    exit(true);
  if (e^.kind = nkBinary) and (e^.bop = boAdd) and (e^.opDecl = nil) and
     (e^.typ^.kind in [tyShortString, tyAnsiString]) then
    exit(isConstantDefault(e^.lhs) and isConstantDefault(e^.rhs));
  if e^.kind = nkSetCtor then
  begin
    for i := 0 to e^.setElems.count - 1 do
    begin
      el := PSetElement(listGet(e^.setElems, i));
      if not fold(el^.lo, c) then
        exit(false);
      if (el^.hi <> nil) and not fold(el^.hi, c) then
        exit(false);
    end;
    exit(true);
  end;
  isConstantDefault := fold(e, c);
end;

{ Checks the default value of parameter p, of a resolved type: a constant
  that can be passed to it. }
procedure checkParamDefault(p: PDecl);
var
  e: PNode;
  t, at: PType;
  c: TConstant;
  bd: TOrdinalBounds;
begin
  e := p^.paramDefault;
  if (e = nil) or isError(p^.varType) then
    exit;
  t := p^.varType;
  at := check(e);
  if isError(at) then
    exit;
  if not isConstantDefault(e) then
  begin
    ms('default parameter value must be a constant');
    errR(e^.b, nodeRange(e));
    exit;
  end;
  if not isAssignable(t, at) and not isPCharLiteral(t, e) then
  begin
    ms('cannot pass a default value of type ');
    maT(at);
    ma(' to parameter ''');
    maN(p^.ident.spelling);
    ma(''' of type ');
    maT(t);
    errR(e^.b, nodeRange(e));
    exit;
  end;
  if fold(e, c) and (c.value.kind = ckOrdinal) and isOrdinal(t) then
  begin
    bd := getOrdinalBounds(t);
    if (c.value.intVal < bd.lo) or (c.value.intVal > bd.hi) then
    begin
      ms('value ');
      maV(c.ty, c.value.intVal);
      ma(' is out of range for ');
      maT(t);
      errR(e^.b, nodeRange(e));
    end;
  end;
end;

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
    checkParamDefault(p);
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
       (resultTy^.kind <> tyShortString) and not isMethodPointer(resultTy) and
       not (not isISO and (resultTy^.kind = tyRecord) and not containsFile(resultTy)) then
    begin
      ms('function result type must be a simple or pointer type, not ');
      maT(resultTy);
      errR(r^.resultSpec^.b, specRange(r^.resultSpec));
      resultTy := errorType;
    end
    else if not isError(resultTy) and (resultTy^.kind = tyRecord) and
            containsInterface(resultTy) then
    begin
      ms('a function cannot return a record containing COM interfaces');
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

function sameParamList(a, b: PDecl; compareOut: boolean): boolean;
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
    if (p^.varKind <> q^.varKind) or (p^.varType <> q^.varType) or
       (compareOut and (p^.isOut <> q^.isOut)) then
      exit(false);
  end;
  sameParamList := true;
end;

function sameSignature(a, b: PDecl): boolean;
begin
  sameSignature := sameParamList(a, b, true);
end;

{ External (C) routines: at the outermost level, with C-compatible types. }
{ A type with a C equivalent, for external routines. }
function isCType(t: PType): boolean;
begin
  isCType := isError(t) or (t^.kind = tyReal) or (t^.kind = tyPointer) or
             (isOrdinal(t) and (getBase(t)^.kind <> tyBoolean));
end;

{ A procedural type C code can call: cdecl, with parameters and result of
  C types. }
function isCCallback(rt: PType): boolean;
var
  i: longint;
  p: PDecl;
begin
  isCCallback := false;
  if not rt^.routineCDecl or
     ((rt^.routineResult <> nil) and not isCType(rt^.routineResult)) then
    exit;
  for i := 0 to rt^.routineParams.count - 1 do
  begin
    p := PDecl(listGet(rt^.routineParams, i));
    if (p^.varType <> nil) and (p^.varKind <> vkVarParam) and
       not isCType(p^.varType) then
      exit;
  end;
  isCCallback := true;
end;

{ The string constant e, the library or symbol name (what) of an external
  routine; false after a diagnostic. }
function externalString(e: PNode; const what: string; var s: PName): boolean;
var
  t: PType;
  c: TConstant;
begin
  externalString := false;
  t := check(e);
  if isError(t) then
    exit;
  if fold(e, c) then
  begin
    if c.value.kind = ckString then
    begin
      s := c.value.strVal;
      exit(true);
    end;
    if (c.value.kind = ckOrdinal) and (getBase(t)^.kind = tyChar) then
    begin
      s := intern(chr(c.value.intVal));
      exit(true);
    end;
  end;
  ms('the ' + what + ' of an external routine must be a string constant');
  errR(e^.b, nodeRange(e));
end;

{ Why routine r cannot be a procedural value ('': it can). Nested routines
  can be passed to ISO procedural parameters (allowNested). }
function notProceduralValue(r: PDecl; allowNested: boolean): string;
var
  i: longint;
  p: PDecl;
begin
  notProceduralValue := '';
  if (r^.parent <> nil) and not allowNested then
    exit('only routines declared at program or unit level can be ' +
         'procedural values');
  if not r^.isExternal then
    exit;
  if not isCDecl(r) then
    exit('an external routine must be ''cdecl'' to be a procedural value');
  if r^.isVarArgs then
    exit('a ''varargs'' routine cannot be a procedural value');
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    if (p^.varType <> nil) and (p^.varType^.kind = tyRoutine) then
      exit('an external routine with procedural parameters cannot be a ' +
           'procedural value');
  end;
end;

{ External (C) routines: declared at the outermost level, with parameters
  and result of types that have a C equivalent; the library and symbol
  names: string constants. }
procedure checkExternal(r: PDecl);
var
  i: longint;
  p: PDecl;
  s: PName;
begin
  if curRoutine <> nil then
  begin
    ms('external routine ''');
    maN(r^.ident.spelling);
    ma(''' must be declared at program or unit level');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  if r^.libraryExpr <> nil then
    if externalString(r^.libraryExpr, 'library name', s) then
      r^.libraryName := s;
  if r^.nameExpr <> nil then
    if externalString(r^.nameExpr, 'symbol name', s) then
      r^.externalName := s;
  if r^.isVarArgs and not isCDecl(r) then
  begin
    ms('''varargs'' routine ''');
    maN(r^.ident.spelling);
    ma(''' must be ''cdecl''');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    { (an untyped parameter: a pointer, void * in C) }
    if (p^.varType = nil) or (p^.varKind = vkVarParam) or
       (p^.varType^.kind = tyUntyped) then
      continue;
    if p^.varType^.kind = tyRoutine then
    begin
      if not isCCallback(p^.varType) then
      begin
        ms('parameter ''');
        maN(p^.ident.spelling);
        ma(''' of external routine ''');
        maN(r^.ident.spelling);
        ma(''' has type ');
        maT(p^.varType);
        ma('; a callback needs a ''cdecl'' procedural type with integer, ' +
           'real, char, pointer or var parameters');
        errR(p^.loc, identRange(p^.ident));
      end;
      continue;
    end;
    if not isCType(p^.varType) then
    begin
      ms('parameter ''');
      maN(p^.ident.spelling);
      ma(''' of external routine ''');
      maN(r^.ident.spelling);
      ma(''' has type ');
      maT(p^.varType);
      ma('; use integer, real, char, pointer, procedural or var parameters');
      errR(p^.loc, identRange(p^.ident));
    end;
  end;
  if r^.isFunction and not isCType(r^.resultType) then
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
type
  TSetBits = array[0..255] of boolean;

{ The elements of the constant set s. }
procedure constantSetBits(s: PNode; var bits: TSetBits);
var
  i: longint;
  v, lo, hi: int64;
  el: PSetElement;
  c: TConstant;
begin
  for i := 0 to 255 do
    bits[i] := false;
  for i := 0 to s^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(s^.setElems, i));
    if not fold(el^.lo, c) then
      exit;
    lo := c.value.intVal;
    hi := lo;
    if el^.hi <> nil then
    begin
      if not fold(el^.hi, c) then
        exit;
      hi := c.value.intVal;
    end;
    if lo < 0 then
      lo := 0;
    if hi > 255 then
      hi := 255;
    v := lo;
    while v <= hi do
    begin
      bits[v] := true;
      v := v + 1;
    end;
  end;
end;

{ Are the constant default values a and b of a parameter equal? }
function sameDefaultValue(a, b: PNode): boolean;
var
  ca, cb: TConstant;
  ba, bb: TSetBits;
  i: longint;
begin
  if (a^.kind = nkNil) or (b^.kind = nkNil) then
    exit((a^.kind = nkNil) and (b^.kind = nkNil));
  if (a^.kind = nkSetCtor) or (b^.kind = nkSetCtor) then
  begin
    if (a^.kind <> nkSetCtor) or (b^.kind <> nkSetCtor) then
      exit(false);
    constantSetBits(a, ba);
    constantSetBits(b, bb);
    for i := 0 to 255 do
      if ba[i] <> bb[i] then
        exit(false);
    exit(true);
  end;
  if not fold(a, ca) or not fold(b, cb) then
    exit(false);
  if ca.value.kind <> cb.value.kind then
    exit(false);
  case ca.value.kind of
    ckOrdinal: sameDefaultValue := ca.value.intVal = cb.value.intVal;
    ckString: sameDefaultValue := ca.value.strVal = cb.value.strVal;
  else
    sameDefaultValue := ca.value.realVal = cb.value.realVal;
  end;
end;

{ Do the default values of impl, the implementation of heading (with the
  same signature), fit? It may leave them out, or repeat them. }
function sameDefaults(heading, impl: PDecl): boolean;
var
  i, errors: longint;
  p: PDecl;
  hd: PNode;
begin
  sameDefaults := false;
  for i := 0 to impl^.params.count - 1 do
  begin
    p := PDecl(listGet(impl^.params, i));
    if p^.paramDefault = nil then
      continue;
    hd := PDecl(listGet(heading^.params, i))^.paramDefault;
    if hd = nil then
      exit;
    errors := diagNumErrors;
    checkParamDefault(p);
    if diagNumErrors <> errors then { reported }
      continue;
    if not sameDefaultValue(hd, p^.paramDefault) then
      exit;
  end;
  sameDefaults := true;
end;

function addOverload(first, r: PDecl): boolean;
var
  x, last: PDecl;
begin
  addOverload := false;
  x := first;
  while x <> nil do
  begin
    if sameParamList(x, r, false) and
       not (isConversionOperator(r) and (x^.resultType <> r^.resultType)) then
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
      if not sameSignature(fwd, r) or ((rt <> nil) and (rt <> fwd^.resultType)) or
         not sameDefaults(fwd, r) then
      begin
        ms('heading of ''');
        maN(r^.ident.spelling);
        ma(''' does not match its forward declaration');
        errR(r^.ident.loc, identRange(r^.ident));
        ms('forward declaration is here');
        note(fwd^.loc);
      end;
    end;
    if isCDecl(r) <> isCDecl(fwd) then
    begin
      ms('calling convention of ''');
      maN(r^.ident.spelling);
      ma(''' is ');
      maCC(isCDecl(r));
      ma(' but its forward declaration''s is ');
      maCC(isCDecl(fwd));
      errR(r^.ident.loc, identRange(r^.ident));
      ms('forward declaration is here');
      note(fwd^.loc);
    end;
    fwd^.body := r^.body;
    r^.canonical := fwd;
    if r^.isInline then
      fwd^.isInline := true;
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
  if (p^.kind = tyShortString) and (a^.kind in [tyShortString, tyStringLit, tyAnsiString]) then
    exit(1);
  if (p^.kind = tyShortString) and (ab^.kind = tyChar) then
    exit(2);
  if (p^.kind = tyAnsiString) and (a^.kind in [tyShortString, tyStringLit]) then
    exit(2);
  if (p^.kind = tyAnsiString) and (ab^.kind = tyChar) then
    exit(3);
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
{ Does an expression of type a fit an open array without being an array
  constructor: the same open array, an array of exactly its element type,
  or a string constant for chars? }
function fitsOpenArray(arg: PNode; a, oa: PType): boolean;
var
  c: TConstant;
begin
  if a = oa then
    exit(true);
  if a^.kind = tyArray then
    exit(a^.elementType = oa^.openElement);
  if a^.kind = tyDynArray then
    exit(a^.dynElement = oa^.openElement);
  fitsOpenArray := (oa^.openElement^.kind = tyChar) and
                   ((a^.kind = tyStringLit) or ((getBase(a)^.kind = tyChar) and fold(arg, c)));
end;

{ The cost of passing [...] for parameter p (-1: it cannot be): as an array
  of const first, then a set, then an open array, as in Free Pascal. }
function setCtorCost(e: PNode; p: PDecl): longint;
var
  pt, t, elemT: PType;
  i, k, cost: longint;
  el: PSetElement;
  x: PNode;
begin
  setCtorCost := -1;
  pt := p^.varType;
  if pt^.kind = tyArrayOfConst then
    exit(0);
  if pt^.kind = tyOpenArray then
  begin
    if p^.varKind = vkVarParam then
      exit;
    elemT := pt^.openElement;
    cost := 2;
    for i := 0 to e^.setElems.count - 1 do
    begin
      el := PSetElement(listGet(e^.setElems, i));
      if el^.hi <> nil then
        exit;
      t := check(el^.lo);
      if isError(t) or (not isAssignable(elemT, t) and not isPCharLiteral(elemT, el^.lo)) then
        exit;
      cost := cost + conversionCost(elemT, t);
    end;
    exit(cost);
  end;
  if (pt^.kind <> tySet) or (p^.varKind = vkVarParam) then
    exit;
  for i := 0 to e^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(e^.setElems, i));
    for k := 1 to 2 do
    begin
      if k = 1 then
        x := el^.lo
      else
        x := el^.hi;
      if x = nil then
        continue;
      t := check(x);
      if isError(t) or not isOrdinal(t) or not haveSameOrdinalHost(pt^.setElement, t) then
        exit;
    end;
  end;
  setCtorCost := 1;
end;

function callCost(r: PDecl; const args, argTys: TPtrList): longint;
var
  i, cost, c: longint;
  p: PDecl;
  pt, at: PType;
  arg: PNode;
begin
  callCost := -1;
  if not acceptsArgCount(r^.params, args.count) then
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
    if (arg^.kind = nkSetCtor) and (at = arrayOfConstT) then
    begin
      c := setCtorCost(arg, p);
      if c < 0 then
        exit;
      cost := cost + c;
      continue;
    end;
    if pt^.kind = tyOpenArray then
    begin
      if (at = nil) or not fitsOpenArray(arg, at, pt) then
        exit;
      if at <> pt then
        cost := cost + 1;
      continue;
    end;
    if at = nil then { a routine's name: for a procedural parameter }
    begin
      if pt^.kind <> tyRoutine then
        exit;
      continue;
    end;
    if pt^.kind = tyUntyped then { any variable, after the typed fits }
    begin
      if not isVariableReference(arg) then
        exit;
      cost := cost + 100;
      continue;
    end;
    if at^.kind = tyUntyped then
      exit;
    if p^.varKind = vkVarParam then
    begin
      if (at <> pt) or not isVariableReference(arg) then
        exit;
      continue;
    end;
    if not isAssignable(pt, at) and not isPCharLiteral(pt, arg) then
    begin
      if findConversion(at, pt, false) = nil then
        exit;
      cost := cost + 1000; { a record's := operator }
      continue;
    end;
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
    if p^.isOut then
      ma('out ')
    else if p^.varKind = vkVarParam then
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
{ slice(a, n), for an open array parameter only. }
function isSliceCall(e: PNode): boolean;
var
  d: PDecl;
begin
  isSliceCall := false;
  if (e^.kind = nkCall) and nameEquals(e^.callee.name, 'slice') then
  begin
    d := lookup(e^.callee.name);
    isSliceCall := (d <> nil) and (d^.kind = dkBuiltin);
  end;
end;

function hasConstArrayParam(const cands: TPtrList; i: longint): boolean;
var
  k: longint;
  r: PDecl;
begin
  hasConstArrayParam := false;
  for k := 0 to cands.count - 1 do
  begin
    r := PDecl(listGet(cands, k));
    if (i < r^.params.count) and (PDecl(listGet(r^.params, i))^.varType <> nil) and
       (PDecl(listGet(r^.params, i))^.varType^.kind in [tyArrayOfConst, tyOpenArray]) then
      exit(true);
  end;
end;

{ [a, b, ...] passed for an array of const: any values it can hold. }
procedure checkConstArray(e: PNode; pt: PType);
var
  i: longint;
  el: PSetElement;
  t: PType;
begin
  for i := 0 to e^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(e^.setElems, i));
    if el^.hi <> nil then
    begin
      ms('a range cannot be an element of an array of const');
      errR(el^.hi^.b, nodeRange(el^.hi));
      continue;
    end;
    t := check(el^.lo);
    if isError(t) then
      continue;
    if not (isIntegerLike(t) or (t^.kind = tyReal) or (getBase(t)^.kind = tyChar) or
            (getBase(t)^.kind = tyBoolean) or isStringish(t) or
            (t^.kind in [tyPointer, tyNil, tyClass, tyClassRef])) then
    begin
      ms('a value of type ');
      maT(t);
      ma(' cannot be an element of an array of const');
      errR(el^.lo^.b, nodeRange(el^.lo));
    end;
  end;
  e^.typ := pt;
end;

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
    { [...] for an open array need not be a valid set }
    if (a^.kind = nkSetCtor) and hasConstArrayParam(cands, i) then
    begin
      listAdd(argTys, arrayOfConstT);
      continue;
    end;
    allowSlice := isSliceCall(a);
    if isRoutineName(a) then
      t := nil
    else
      t := checkAllowingUntyped(a);
    allowSlice := false;
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
  if r^.isStatic then { a record's static method: its statics }
  begin
    cur := newClassScope(cur, r^.ownerClass, nil);
    cur^.routineBoundary := true;
  end
  else if r^.methodKind <> mtNone then
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
    cur^.routineBoundary := true;
  end;
  cur := newScope(cur);
  if cur^.parent = savedScope then
    cur^.routineBoundary := true;
  curRoutine := r;
  if (r^.methodKind <> mtNone) and not r^.isStatic then
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
    if (pa^.varKind <> pb^.varKind) or (pa^.isOut <> pb^.isOut) then
      exit(false);
    ta := pa^.varType;
    tb := pb^.varType;
    if (ta <> nil) and (tb <> nil) and (ta^.kind = tyRoutine) and (tb^.kind = tyRoutine) then
    begin
      if (ta^.routineCDecl <> tb^.routineCDecl) or
         (ta^.routineOfObject <> tb^.routineOfObject) or
         not congruent(ta^.routineParams, ta^.routineResult, tb^.routineParams,
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
function checkValueFor(targetTy: PType; value: PNode): PType; forward;

procedure checkRoutineArg(arg: PNode; p: PDecl; formal: PType);
var
  kind: string;
  d, x: PDecl;
  params: TPtrList;
  res, xr: PType;
  found, cdeclConv: boolean;
  n: PNode;
  why: string;
  ofT: PType;
begin
  if formal^.routineOfObject then { @obj.M, or a method pointer }
  begin
    ofT := checkValueFor(formal, arg);
    if isError(ofT) then
      exit;
    if not isAssignable(formal, ofT) then
    begin
      ms('cannot pass a value of type ');
      maT(ofT);
      ma(' to parameter ''');
      maN(p^.ident.spelling);
      ma(''' of type ');
      maT(formal);
      errR(arg^.b, nodeRange(arg));
    end
    else if (p^.varKind = vkVarParam) and not isVariableReference(arg) then
    begin
      ms('argument for ''' + paramKeyword(p) + ''' parameter ''');
      maN(p^.ident.spelling);
      ma(''' must be a variable');
      errR(arg^.b, nodeRange(arg));
    end
    else if p^.varKind = vkVarParam then
      checkModifiable(arg);
    exit;
  end;
  cdeclConv := false;
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
      if (isCDecl(x) = formal^.routineCDecl) and
         congruent(formal^.routineParams, formal^.routineResult, x^.params, xr) then
      begin
        d := x;
        break;
      end;
      x := x^.nextOverload;
    end;
    params := d^.params;
    if d^.isFunction then
      res := d^.resultType;
    cdeclConv := isCDecl(d);
    found := true;
  end
  else if (d <> nil) and (d^.kind = dkVar) and (d^.varType <> nil) and
          (d^.varType^.kind = tyRoutine) then
  begin
    params := d^.varType^.routineParams;
    res := d^.varType^.routineResult;
    cdeclConv := d^.varType^.routineCDecl;
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
  if d^.kind = dkRoutine then
  begin
    why := notProceduralValue(d, not formal^.routineCDecl);
    if why <> '' then
    begin
      ms('''');
      maN(n^.ref.spelling);
      ma(''': ' + why);
      errR(arg^.b, nodeRange(arg));
      exit;
    end;
  end;
  if (cdeclConv <> formal^.routineCDecl) or
     not congruent(formal^.routineParams, formal^.routineResult, params, res) then
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
{ An argument (number i, from 0) beyond the parameters of a 'varargs'
  external routine: a value C can take (ordinals are passed as int at least,
  reals as double, string constants as C strings). }
procedure checkVarArg(arg: PNode; i: longint; const callee: TIdent);
var
  t: PType;
  stringConst: boolean;
begin
  if arg^.kind = nkFormatted then
  begin
    ms('field widths are only allowed in ''write'' and ''writeln''');
    errR(arg^.fmtWidth^.b, nodeRange(arg^.fmtWidth));
    exit;
  end;
  t := check(arg);
  stringConst := (t^.kind = tyStringLit) and
                 ((arg^.kind = nkStringLit) or
                  ((arg^.kind = nkNameRef) and (arg^.refDecl <> nil) and
                   (arg^.refDecl^.kind = dkConst)));
  if isError(t) or isOrdinal(t) or (t^.kind = tyReal) or (t^.kind = tyPointer) or
     (t^.kind = tyClass) or (t^.kind = tyNil) or stringConst then
    exit;
  ms('argument ');
  maI(i + 1);
  ma(' of ''');
  maN(callee.spelling);
  ma(''' has type ');
  maT(t);
  ma('; a ''varargs'' argument must be an integer, real, char, boolean, ' +
     'pointer or string constant');
  errR(arg^.b, nodeRange(arg));
end;

procedure checkOpenArrayArg(arg: PNode; p: PDecl; oa: PType);
var
  i: longint;
  el: PSetElement;
  t, elemT, at: PType;
  slice: boolean;
  target: PNode;

  procedure mustBeVariable;
  begin
    ms('argument for ''' + paramKeyword(p) + ''' parameter ''');
    maN(p^.ident.spelling);
    ma(''' must be a variable');
    errR(arg^.b, nodeRange(arg));
  end;

begin
  elemT := oa^.openElement;
  if arg^.kind = nkSetCtor then
  begin
    if p^.varKind = vkVarParam then
    begin
      mustBeVariable;
      exit;
    end;
    for i := 0 to arg^.setElems.count - 1 do
    begin
      el := PSetElement(listGet(arg^.setElems, i));
      if el^.hi <> nil then
      begin
        ms('a range cannot be an element of an open array');
        errR(el^.hi^.b, nodeRange(el^.hi));
        continue;
      end;
      t := check(el^.lo);
      if isError(t) then
        continue;
      if not isAssignable(elemT, t) and not isPCharLiteral(elemT, el^.lo) and
         not convertImplicitly(el^.lo, elemT) then
      begin
        ms('a value of type ');
        maT(t);
        ma(' cannot be an element of an open array of ');
        maT(elemT);
        errR(el^.lo^.b, nodeRange(el^.lo));
      end;
    end;
    arg^.typ := oa;
    exit;
  end;
  slice := isSliceCall(arg);
  allowSlice := slice;
  at := check(arg);
  allowSlice := false;
  if isError(at) then
    exit;
  if not fitsOpenArray(arg, at, oa) then
  begin
    ms('cannot pass a value of type ');
    maT(at);
    ma(' to parameter ''');
    maN(p^.ident.spelling);
    ma(''' of type ');
    maT(oa);
    errR(arg^.b, nodeRange(arg));
    exit;
  end;
  if p^.varKind = vkVarParam then
  begin
    target := arg;
    if slice then
      target := PNode(listGet(arg^.args, 0));
    if not isVariableReference(target) then
      mustBeVariable
    else
      checkModifiable(target);
  end;
end;

procedure checkCallArgs(const params: TPtrList; calleeDecl: PDecl;
                        const callee: TIdent; const args: TPtrList;
                        callRange: TRange);
var
  aocT: PType;
  isVarArgsCall: boolean;
  i: longint;
  arg: PNode;
  p: PDecl;
  at, pt: PType;
begin
  { a call whose result is discarded still holds it for a while }
  if (calleeDecl <> nil) and (calleeDecl^.kind = dkRoutine) and
     calleeDecl^.isFunction and (calleeDecl^.resultType <> nil) and
     isManaged(calleeDecl^.resultType) then
    markUsesManaged;
  if (calleeDecl <> nil) and (calleeDecl^.kind = dkVar) and
     (calleeDecl^.varType <> nil) and (calleeDecl^.varType^.kind = tyRoutine) and
     (calleeDecl^.varType^.routineResult <> nil) and
     isManaged(calleeDecl^.varType^.routineResult) then
    markUsesManaged;
  isVarArgsCall := (calleeDecl <> nil) and (calleeDecl^.kind = dkRoutine) and
                   calleeDecl^.isVarArgs;
  if not acceptsArgCount(params, args.count) and
     not (isVarArgsCall and (args.count > params.count)) then
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
    if i >= params.count then { varargs }
    begin
      checkVarArg(arg, i, callee);
      continue;
    end;
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
    if (p^.varType <> nil) and (p^.varType^.kind = tyOpenArray) then
    begin
      if (calleeDecl <> nil) and (calleeDecl^.kind = dkRoutine) and isCDecl(calleeDecl) then
      begin
        ms('an open array cannot be passed to a ''cdecl'' routine');
        errR(arg^.b, nodeRange(arg));
      end
      else
        checkOpenArrayArg(arg, p, p^.varType);
      continue;
    end;
    if (p^.varType <> nil) and (p^.varType^.kind = tyArrayOfConst) then
    begin
      if (calleeDecl <> nil) and (calleeDecl^.kind = dkRoutine) and isCDecl(calleeDecl) then
      begin
        ms('an array of const cannot be passed to a ''cdecl'' routine');
        errR(arg^.b, nodeRange(arg));
        continue;
      end;
      if arg^.kind = nkSetCtor then
      begin
        checkConstArray(arg, p^.varType);
        continue;
      end;
      aocT := check(arg);
      if not isError(aocT) and (aocT^.kind <> tyArrayOfConst) then
      begin
        ms('cannot pass a value of type ');
        maT(aocT);
        ma(' to parameter ''');
        maN(p^.ident.spelling);
        ma(''' of type ''array of const''');
        errR(arg^.b, nodeRange(arg));
      end;
      continue;
    end;
    if (p^.varType <> nil) and (p^.varType^.kind = tyUntyped) then
    begin
      { Turbo/FPC: any variable, passed by reference }
      if isError(checkAllowingUntyped(arg)) then
        continue;
      if not isVariableReference(arg) then
      begin
        ms('argument for untyped parameter ''');
        maN(p^.ident.spelling);
        ma(''' must be a variable');
        errR(arg^.b, nodeRange(arg));
      end
      else if p^.varKind = vkVarParam then
        checkModifiable(arg);
      continue;
    end;
    pt := p^.varType;
    if (pt <> nil) and ((arg^.kind = nkAddrOf) or
                        ((pt^.kind = tyDynArray) and (arg^.kind = nkSetCtor))) then
      at := checkValueFor(pt, arg)
    else
      at := check(arg);
    if isError(at) or (pt = nil) or isError(pt) then
      continue;
    if p^.varKind = vkVarParam then
    begin
      if not isVariableReference(arg) then
      begin
        ms('argument for ''' + paramKeyword(p) + ''' parameter ''');
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
        ma(' for ''' + paramKeyword(p) + ''' parameter ''');
        maN(p^.ident.spelling);
        ma(''' must have exactly type ');
        maT(pt);
        errR(arg^.b, nodeRange(arg));
      end
      else
        checkNotForControlVar(arg);
      continue;
    end;
    if not isAssignable(pt, at) and not isPCharLiteral(pt, arg) and
       not convertImplicitly(arg, pt) then
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
  iunknownType := nil;
  interfacedType := nil;
  namedAdvancedRecord := nil;
  constInitWhat := 'typed constant value';
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
