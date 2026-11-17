unit pcsema;
{ Semantic analysis of the self-hosted compiler; a port of
  src/sema/Sema.cpp. One pass over the tree in declaration order: scopes,
  constant folding, types (name equivalence), and the ISO 7185 rules of
  compatibility and assignment-compatibility, plus the Turbo/FPC
  extensions. It annotates the tree (types, declarations, constant values)
  and reports the same diagnostics as the C++ compiler. }

interface

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcscope, pcparse;

{ Checks the units (dependencies first) and the program. Returns true if
  they are valid. }
function semaCheck(prog: PDecl; const units: TPtrList; m: TLangMode;
                   separateUnits: boolean): boolean;
{ Checks unit u, compiled on its own; deps (dependency order) are the
  units it uses, directly or not, of which only the interfaces matter. }
function semaCheckUnit(u: PDecl; const deps: TPtrList; m: TLangMode): boolean;
{ The predeclared names (of PName, lower case) of mode m, which
  declared(NAME) in $IF sees. }
function standardNames(m: TLangMode): TPtrList;

implementation

const
  pascalMaxInt = 2147483647;

type
  TConstant = record
    ty: PType;
    value: TConstValue;
  end;

  { a specialization of generic g for types (of PType): r, checked in scope
    for a use at use }
  PInstance = ^TInstance;
  TInstance = record
    g, r: PDecl;
    types: TPtrList;
    scope: PScope;
    use: TLoc;
    { the pending instance whose body asked for it (an index into
      pendingInstances), -1 if none: an error in it notes that one's use
      too, so it leads back to the code outside the generics }
    parent: longint;
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
  activeFors: TPtrList;       { of PNode (nkFor, nkForIn), innermost last }
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
  { f += e in function f: the f read again, which is f's result too (as in
    Free Pascal), not a call }
  compoundResultRef: PNode;
  compoundResultVar: PDecl;
  tobjectType: PType;
  pendingClasses: TPtrList;
  pendingInterfaces: TPtrList; { interfaces declared forward (type decls) }
  { exceptions (objfpc): the predeclared class Exception; the exception
    handlers around the statement being checked (a bare raise needs one);
    loopDepth on entry to the innermost enclosing finally part, or -1 (exit,
    and break/continue out of it, are not allowed there). objfpc has no
    gotos, so none can leave a try statement. }
  exceptionType: PType;
  { TSharedObject (objfpc): the root of the thread-safe classes }
  sharedObjectType: PType;
  arrayOfConstT, pcharT, tmethodT: PType;
  tguidT: PType; { the predeclared TGUID }
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
  { while the members of a class or advanced record are declared: its name,
    which prefixes those of the types it declares (TOuter.TInner) }
  nestPrefix: PName;
  { generic routines (objfpc): the specializations made (by generic and
    type arguments); those whose bodies are to be checked (in their scope,
    for a use); those of the module being checked, added to its block for
    code generation }
  instances: TPtrList;        { of PInstance }
  pendingInstances: TPtrList; { of PInstance }
  { the pending instance whose body is being checked, or -1 }
  checkingInstance: longint;
  moduleInstances: TPtrList;  { of PDecl }
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

{ 'record helper' or 'type helper' (helper c), for diagnostics }
function helperKindName(c: PType): string;
begin
  if c^.classRecordHelper then
    helperKindName := 'record helper'
  else
    helperKindName := 'type helper';
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
      { (labels; generic routines' keys, f<1>) }
      if ((cand^.len > 0) and ((cand^.chars^[0] = '#') or
                               (cand^.chars^[cand^.len - 1] = '>'))) or
         (cand = name) then
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
function genericKey(name: PName; n: longint): PName; forward;
function isGenericName(name: PName): boolean; forward;
procedure declareGenericType(td: PDecl); forward;
function specializeType(const name: TIdent; const args: TPtrList): PDecl; forward;

function isInstanceMember(d: PDecl): boolean; forward;

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
        if isInstanceMember(d) then
          ms('instance member ''')
        else
          ms('class method ''');
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
  if isGenericName(id.name) then
  begin
    ms('''');
    maN(id.spelling);
    ma(''' is a generic; give its type arguments: specialize ');
    maN(id.spelling);
    ma('<...>');
    errR(id.loc, identRange(id));
    exit;
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
                    bkParamStr, bkAssign, bkClose, bkFlush, bkBlockRead, bkBlockWrite,
                    bkSeek, bkFilePos, bkFileSize, bkGetFileHandle,
                    bkAssignFile, bkCloseFile, bkExceptAddr];
end;

{ Free Pascal's objpas built-ins, predeclared only with --mode=objfpc. }
function isObjFPCOnly(k: TBuiltinKind): boolean;
begin
  isObjFPCOnly := k in [bkAssignFile, bkCloseFile, bkExceptAddr, bkAddChecked,
                        bkSubChecked, bkMulChecked, bkWait, bkWhenAll, bkWhenAny,
                        bkIteratorNext, bkIteratorValue];
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

{ TGUID = packed record case integer of
    1: (Data1: longword; Data2, Data3: word; Data4: array[0..7] of byte);
    2: (D1: longword; D2, D3: word; D4: array[0..7] of byte);
    3: (time_low: longword; time_mid, time_hi_and_version: word;
        clock_seq_hi_and_reserved, clock_seq_low: byte;
        node: array[0..5] of byte);
  end; and PGUID = ^TGUID, as in Free Pascal's system unit (tp, objfpc). }
procedure declareTGUID;
var
  all, vs: TPtrList;
  one, fl: PFieldList;
  v: PVariant;
  vp: PVariantPart;
  rec, pguid: PType;
  d: PDecl;

  function bytes(hi: int64): PType;
  begin
    bytes := newArrayType(newSubrangeType(tyIntegerT, 0, hi), tyByteT, false);
  end;

  procedure startVariant;
  begin
    one := PFieldList(arenaAlloc(sizeof(TFieldList)));
    listInit(one^.fixed);
    one^.variant := nil;
    v := PVariant(arenaAlloc(sizeof(TVariant)));
    listInit(v^.labels);
    v^.fields := one;
    listAdd(vs, v);
  end;

  procedure field(const name: string; t: PType);
  var
    f: PDecl;
  begin
    f := newDecl(dkField, memberIdent(name), noLoc);
    f^.fieldType := t;
    listAdd(one^.fixed, f);
    listAdd(all, f);
  end;

begin
  listInit(all);
  listInit(vs);
  startVariant;
  field('Data1', tyCardinalT);
  field('Data2', tyWordT);
  field('Data3', tyWordT);
  field('Data4', bytes(7));
  startVariant;
  field('D1', tyCardinalT);
  field('D2', tyWordT);
  field('D3', tyWordT);
  field('D4', bytes(7));
  startVariant;
  field('time_low', tyCardinalT);
  field('time_mid', tyWordT);
  field('time_hi_and_version', tyWordT);
  field('clock_seq_hi_and_reserved', tyByteT);
  field('clock_seq_low', tyByteT);
  field('node', bytes(5));
  vp := PVariantPart(arenaAlloc(sizeof(TVariantPart)));
  vp^.tag := nil;
  vp^.tagSpec := nil;
  vp^.tagType := nil;
  vp^.variants := vs;
  fl := PFieldList(arenaAlloc(sizeof(TFieldList)));
  listInit(fl^.fixed);
  fl^.variant := vp;
  rec := newRecordType(fl, all, false);
  setDeclName(rec, intern('TGUID'));
  tguidT := rec;
  d := newDecl(dkType, memberIdent('TGUID'), noLoc);
  d^.declType := rec;
  scopeInsert(standard, d^.ident.name, d);
  pguid := newPointerType(rec);
  setDeclName(pguid, intern('PGUID'));
  d := newDecl(dkType, memberIdent('PGUID'), noLoc);
  d^.declType := pguid;
  scopeInsert(standard, d^.ident.name, d);
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
  external_ := declareExceptionClass('EIntOverflow', intError); { (objfpc overflow) }
  { tasks: a cancelled task's wait raises EOperationCancelled; tokens }
  external_ := declareExceptionClass('EOperationCancelled', exceptionType);
  d := newDecl(dkType, memberIdent('TCancelToken'), noLoc);
  d^.declType := tokenType;
  scopeInsert(standard, d^.ident.name, d);
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

{ TSharedObject = class(TObject) end: a class derived from it says it is
  thread-safe (its methods synchronize what they share), so shared
  variables can hold its objects. Its VMT is in the runtime. }
procedure declareSharedObject;
var
  members, fields: TPtrList;
  d: PDecl;
  i: longint;
begin
  sharedObjectType := newClassType;
  setDeclName(sharedObjectType, intern('TSharedObject'));
  listInit(members);
  listInit(fields);
  for i := 0 to tobjectType^.classAllFields.count - 1 do
    listAdd(fields, listGet(tobjectType^.classAllFields, i));
  defineClass(sharedObjectType, tobjectType, members, fields,
              tobjectType^.classVTable, nil);
  d := newDecl(dkType, memberIdent('TSharedObject'), noLoc);
  d^.declType := sharedObjectType;
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
    { FPC's pointer-sized signed integers (every target is 64-bit) }
    stdType('sizeint', tyInt64T);
    stdType('ptrint', tyInt64T);
    stdType('nativeint', tyInt64T);
    stdType('cardinal', tyCardinalT);
    stdType('longword', tyCardinalT);
    stdType('dword', tyCardinalT);
    { unsigned 64-bit, and FPC's pointer-sized unsigned integers }
    stdType('qword', tyQWordT);
    stdType('uint64', tyQWordT);
    stdType('nativeuint', tyQWordT);
    stdType('ptruint', tyQWordT);
    stdType('sizeuint', tyQWordT);
    { FPC's THandle (a file descriptor on Unix): longint }
    stdType('thandle', tyIntegerT);
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
    { Free Pascal's names for the one-byte char (char is AnsiChar there) }
    stdType('ansichar', tyCharT);
    { a 16-bit character code (FPC's widechar; unicodechar is the same) }
    stdType('widechar', tyWideCharT);
    stdType('unicodechar', tyWideCharT);
    stdType('pansichar', pchar);
    stdType('ansistring', ansiStringType);
    stdType('shortstring', shortStringType(255));
    declareTGUID;
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
    declareSharedObject;
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
    if (not isTPOnly(k) or not isISO) and
       (not isObjFPCOnly(k) or (mode = lmObjFPC)) then
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
var
  i: longint;
begin
  acceptsArgCount := false;
  if n > params.count then
    exit;
  for i := n to params.count - 1 do
    if PDecl(listGet(params, i))^.paramDefault = nil then
      exit;
  acceptsArgCount := true;
end;

{ The value of an argument: of Name := value, the value. }
function argValue(a: PNode): PNode;
begin
  if a^.kind = nkNamedArg then
    exit(a^.naValue);
  argValue := a;
end;

function hasNamedArgs(const args: TPtrList): boolean;
var
  i: longint;
begin
  hasNamedArgs := false;
  for i := 0 to args.count - 1 do
    if PNode(listGet(args, i))^.kind = nkNamedArg then
      exit(true);
end;

{ The parameter of params that argument i of args is for (by its position,
  or by its name), or nil. }
function paramOfArg(const params, args: TPtrList; i: longint): PDecl;
var
  a: PNode;
  k: longint;
begin
  paramOfArg := nil;
  a := PNode(listGet(args, i));
  if a^.kind = nkNamedArg then
  begin
    for k := 0 to params.count - 1 do
      if PDecl(listGet(params, k))^.ident.name = a^.naName.name then
        exit(PDecl(listGet(params, k)));
    exit;
  end;
  if i < params.count then
    paramOfArg := PDecl(listGet(params, i));
end;

{ Where the arguments of a call with named ones go: map holds, for each
  argument, its parameter (of PDecl). Arguments by position come first and
  are for the first parameters; each parameter is given once; one that is
  not given has a default value. False if args do not fit, reported if
  report. }
function mapArgs(const params, args: TPtrList; var map: TPtrList;
                 report: boolean; const callee: TIdent): boolean;
var
  given: TPtrList;
  named: boolean;
  i, at: longint;
  a: PNode;
begin
  mapArgs := false;
  listInit(map);
  listInit(given);
  for i := 0 to params.count - 1 do
    listAdd(given, nil);
  named := false;
  for i := 0 to args.count - 1 do
  begin
    a := PNode(listGet(args, i));
    at := i;
    if a^.kind = nkNamedArg then
    begin
      named := true;
      at := 0;
      while at < params.count do
      begin
        if PDecl(listGet(params, at))^.ident.name = a^.naName.name then
          break;
        at := at + 1;
      end;
      if at = params.count then
      begin
        if report then
        begin
          ms('''');
          maN(callee.spelling);
          ma(''' has no parameter named ''');
          maN(a^.naName.spelling);
          ma('''');
          errR(a^.naName.loc, identRange(a^.naName));
        end;
        exit;
      end;
    end
    else if named then
    begin
      if report then
      begin
        ms('an argument without a name cannot follow a named one');
        errR(a^.b, nodeRange(a));
      end;
      exit;
    end
    else if at >= params.count then
    begin
      if report then
      begin
        ms('too many arguments in call to ''');
        maN(callee.spelling);
        ma(''': it has ');
        maI(params.count);
        if params.count = 1 then
          ma(' parameter')
        else
          ma(' parameters');
        errR(a^.b, nodeRange(a));
      end;
      exit;
    end;
    if listGet(given, at) <> nil then
    begin
      if report then
      begin
        ms('parameter ''');
        maN(PDecl(listGet(params, at))^.ident.spelling);
        ma(''' is given twice');
        errR(a^.b, nodeRange(a));
        ms('its other argument is here');
        note(PNode(listGet(given, at))^.b);
      end;
      exit;
    end;
    listSet(given, at, a);
    listAdd(map, listGet(params, at));
  end;
  for i := 0 to params.count - 1 do
    if (listGet(given, i) = nil) and
       (PDecl(listGet(params, i))^.paramDefault = nil) then
    begin
      if report then
      begin
        ms('no argument for parameter ''');
        maN(PDecl(listGet(params, i))^.ident.spelling);
        ma(''' in call to ''');
        maN(callee.spelling);
        ma('''');
        errR(callee.loc, identRange(callee));
      end;
      exit;
    end;
  mapArgs := true;
end;

var
  argsFailed: boolean; { see orderArgs }
  { the call checked for a value of type wantedType (checkValueFor) }
  wantedCall: PNode;
  wantedType: PType;
  keepCharLiterals: boolean; { see visitDestructure }

{ The arguments args of a call to a routine with params, put in the order
  of the parameters: a call with named arguments (Name := value) is
  rewritten to one by position, where a parameter left out before the last
  one given takes its default value. After an error argsFailed is set, for
  checkCallArgs. }
procedure orderArgs(const params: TPtrList; var args: TPtrList;
                    const callee: TIdent);
var
  map, ordered: TPtrList;
  i, j, count: longint;
  found: PNode;
begin
  if not hasNamedArgs(args) then
    exit;
  if not mapArgs(params, args, map, true, callee) then
  begin
    argsFailed := true;
    exit;
  end;
  count := 0;
  for j := 0 to params.count - 1 do
    for i := 0 to map.count - 1 do
      if listGet(map, i) = listGet(params, j) then
        count := j + 1;
  listInit(ordered);
  for j := 0 to count - 1 do
  begin
    found := nil;
    for i := 0 to map.count - 1 do
      if listGet(map, i) = listGet(params, j) then
        found := argValue(PNode(listGet(args, i)));
    if found = nil then
      found := PDecl(listGet(params, j))^.paramDefault;
    listAdd(ordered, found);
  end;
  args := ordered;
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

{ Is e a constant of zero or more (that a qword can hold)? }
function isNonNegativeConstant(e: PNode): boolean;
var
  c: TConstant;
begin
  isNonNegativeConstant := fold(e, c) and (c.value.kind = ckOrdinal) and
                           (c.value.intVal >= 0);
end;

{ The type integer operands are converted to for an operator: qword if one
  is qword and the other an int64 constant of zero or more; int64 if
  either is int64; qword if either is qword (the other, of 32 bits or
  less, is converted to qword, as in FPC); cardinal if both are unsigned
  (or the other is a constant that fits); int64 when cardinal meets a
  signed value; integer otherwise. }

function integerOperandType(le: PNode; l: PType; re: PNode; r: PType): PType;
var
  pl, pr, otherTy: PType;
  other: PNode;
begin
  pl := promoteInteger(l);
  pr := promoteInteger(r);
  { qword with an int64 constant of zero or more: qword }
  if (isQWord(pl) and (pr = tyInt64T) and isNonNegativeConstant(re)) or
     (isQWord(pr) and (pl = tyInt64T) and isNonNegativeConstant(le)) then
    exit(tyQWordT);
  if (pl = tyInt64T) or (pr = tyInt64T) then
    exit(tyInt64T);
  if isQWord(pl) or isQWord(pr) then
    exit(tyQWordT);
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

{ Are a and b tuple types with the same element types, in turn (the names
  may differ)? Such tuples have one layout. }
function sameTupleShape(a, b: PType): boolean;
var
  i: longint;
  ta, tb: PType;
begin
  sameTupleShape := false;
  if (a^.kind <> tyRecord) or (b^.kind <> tyRecord) then
    exit;
  if not a^.isTupleRecord or not b^.isTupleRecord or
     (a^.allFields.count <> b^.allFields.count) then
    exit;
  for i := 0 to a^.allFields.count - 1 do
  begin
    ta := PDecl(listGet(a^.allFields, i))^.fieldType;
    tb := PDecl(listGet(b^.allFields, i))^.fieldType;
    if (ta <> tb) and not sameTupleShape(ta, tb) then
      exit;
  end;
  sameTupleShape := true;
end;

function isAssignable(target, value: PType): boolean;
begin
  if (target^.kind = tyOpenArray) or (value^.kind = tyOpenArray) then
    exit(false); { only passed along and indexed }
  if (target^.kind in [tyDynArray, tyTask, tyToken, tyIterator]) and (target <> value) then
    exit(value^.kind = tyNil);
  if containsFile(target) or containsFile(value) then
    exit(false); { files cannot be copied }
  if target = value then
    exit(true);
  if sameTupleShape(target, value) then
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
         (value^.routineReference = target^.routineReference) and
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

{ Is e a quoted character ('a')? Where a type is inferred it is a string,
  like any quoted text. }
function isCharLiteral(e: PNode): boolean;
begin
  while e^.kind = nkParen do
    e := e^.inner;
  isCharLiteral := (e^.kind = nkStringLit) and (e^.strValue^.len = 1) and
                   (e^.strWide = 0);
end;

{ The type that values of types a and b have in common where one type is
  inferred for both (the elements of [a, b], the arguments for one type
  parameter), or nil: the same type; the wider integer type; real for a
  real and an integer (nothing is lost); str for strings and characters;
  else the one the other can be assigned to. }
function commonType(a, b, str: PType): PType;
var
  ia, ib: boolean;
  pa, pb: PType;
begin
  if a = b then
    exit(a);
  ia := getBase(a)^.kind = tyInteger;
  ib := getBase(b)^.kind = tyInteger;
  if ia and ib then
  begin
    pa := promoteInteger(a);
    pb := promoteInteger(b);
    if pa = pb then
      exit(pa);
    exit(tyInt64T);
  end;
  if ((a^.kind = tyReal) and (ib or (b^.kind = tyReal))) or
     ((b^.kind = tyReal) and ia) then
    exit(tyRealT);
  if isStringish(a) and isStringish(b) then
    exit(str);
  if isAssignable(a, b) then
    exit(a);
  if isAssignable(b, a) then
    exit(b);
  commonType := nil;
end;

{ Can a variable (an element, a type argument) have the type t of a value,
  when it is given no type of its own? }
function isInferable(t: PType): boolean;
begin
  isInferable := not ((t^.kind in [tyNil, tyUntyped, tyOpenArray, tyArrayOfConst]) or
                      containsFile(t) or ((t^.kind = tySet) and isEmptySet(t)));
end;

{ Is e (already checked) a value of a class type? }
function isObject(e: PNode): boolean;
begin
  isObject := (e^.typ <> nil) and (e^.typ^.kind = tyClass);
end;

{ Can e be used where a variable is required? }
function isVariableReference(e: PNode): boolean; forward;

{ Can e's address be passed for a constref parameter: a variable (not a
  bit field or a property)? }
function isConstRefAddressable(e: PNode): boolean;
begin
  if (e^.kind = nkField) and
     ((e^.fieldDecl = nil) or not (e^.fieldDecl^.kind in [dkField, dkVar])) then
    exit(false);
  isConstRefAddressable := isVariableReference(e);
end;

function isVariableReference(e: PNode): boolean;
var
  d: PDecl;
begin
  if isBitPackedComponent(e) then { assignable, but without an address }
    exit(false);
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
      if (e^.base^.typ <> nil) and (e^.base^.typ^.kind in [tyDynArray, tyPointer]) then
        isVariableReference := e^.idxProp = nil { on the heap, like p^; p[i] }
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

{ e (a bit field) where an address is needed }
procedure diagBitFieldAddress(e: PNode);
begin
  ms('the elements and fields of bitpacked arrays and records have no address ' +
     '(they cannot be passed as ''var'' parameters or used with ''@'')');
  errR(e^.b, nodeRange(e));
end;

{ Does t have a bitpacked part (itself, an element, a field)? }
{ Does t have a record laid out with a field alignment (packed, or
  $packrecords N): itself, an element, a field? }
function containsAlignedRecord(t: PType): boolean;
var
  i: longint;
  f: PDecl;
begin
  if t^.kind = tyArray then
    exit(containsAlignedRecord(t^.elementType));
  if t^.kind = tyRecord then
  begin
    if fieldAlignment(t) <> 0 then
      exit(true);
    for i := 0 to t^.allFields.count - 1 do
    begin
      f := PDecl(listGet(t^.allFields, i));
      if (f^.fieldType <> nil) and containsAlignedRecord(f^.fieldType) then
        exit(true);
    end;
  end;
  containsAlignedRecord := false;
end;

{ A type in t whose values a constant's byte image cannot hold (one that is
  not an ordinal, real or short string, or an array or record of those),
  or nil. (The constants of records laid out with a field alignment are
  their bytes.) }
function notByteImage(t: PType): PType;
var
  i: longint;
  f: PDecl;
  bad: PType;
begin
  notByteImage := nil;
  if t^.kind = tyArray then
    exit(notByteImage(t^.elementType));
  if t^.kind = tyRecord then
  begin
    for i := 0 to t^.allFields.count - 1 do
    begin
      f := PDecl(listGet(t^.allFields, i));
      if f^.fieldType <> nil then
      begin
        bad := notByteImage(f^.fieldType);
        if bad <> nil then
          exit(bad);
      end;
    end;
    exit;
  end;
  if isOrdinal(t) or (t^.kind in [tyReal, tyShortString, tyError]) then
    exit;
  notByteImage := t;
end;

function containsBitPacked(t: PType): boolean;
var
  i: longint;
  f: PDecl;
begin
  if t^.kind = tyArray then
    exit(t^.isBitPackedArray or containsBitPacked(t^.elementType));
  if t^.kind = tyRecord then
  begin
    if t^.isBitPackedRecord then
      exit(true);
    for i := 0 to t^.allFields.count - 1 do
    begin
      f := PDecl(listGet(t^.allFields, i));
      if (f^.fieldType <> nil) and containsBitPacked(f^.fieldType) then
        exit(true);
    end;
  end;
  containsBitPacked := false;
end;

{ Does a value of the non-ordinal type t need no alignment (it can sit at
  any byte of a bitpacked record)? }
function isByteAligned(t: PType): boolean;
begin
  if isOrdinal(t) then { as an array's element: stored in a byte? }
    exit((getBase(t)^.kind in [tyBoolean, tyChar]) or
         ((t^.kind = tyInteger) and (t^.bits = 8)));
  if t^.kind = tyShortString then
    exit(true);
  if t^.kind = tyRecord then
    exit(t^.isBitPackedRecord);
  if t^.kind = tyArray then
    exit(hasBitElements(t) or isByteAligned(t^.elementType));
  isByteAligned := false;
end;

{ Is e a variable a type helper's method can change (not a method's
  result: r.M, r.M.f)? }
function isHelperVariable(e: PNode): boolean;
begin
  if e^.kind = nkField then
  begin
    if (e^.fieldDecl <> nil) and (e^.fieldDecl^.kind = dkVar) then
      exit(true);
    if (e^.fieldDecl = nil) or (e^.fieldDecl^.kind <> dkField) then
      exit(false);
    exit(isObject(e^.base) or isHelperVariable(e^.base));
  end;
  if (e^.kind = nkIndex) and (e^.idxProp = nil) and (e^.base^.typ <> nil) and
     not (e^.base^.typ^.kind in [tyDynArray, tyPointer]) then
    exit(isHelperVariable(e^.base));
  isHelperVariable := isVariableReference(e);
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
          if (e^.idxProp <> nil) or (e^.base^.typ^.kind in [tyDynArray, tyPointer]) then
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
           (e^.refDecl^.kind = dkVar) and
           ((e^.refDecl^.varKind = vkConstParam) or e^.refDecl^.isReadOnly) then
          exit(e^.refDecl);
        exit(nil);
      end;
    end;
end;

{ --- Owned variables (objfpc): which may have been moved ---

  The statements of a routine with owned variables are followed in the
  order they run, with the state of each owned variable: it may hold its
  object (valid), it may have been moved (moved), or both, after paths
  that differ. A use of a variable that may have been moved is an error.
  Unreachable code has neither bit of any variable.

  A loop is followed once. entry[i] is the outermost enclosing loop
  (counted from 1) whose every earlier iteration may have left variable i
  as it is, or noLoop: a use of the variable then is recorded in those
  loops, and reported when the loop turns out to end with it moved. }

const
  maxOwned = 32;
  noLoop = 255;
  { set in valid of every state that is reached (a routine may have no
    owned variable yet: they can be declared among the statements) }
  liveBit = 4294967296; { 1 shl maxOwned }

type
  TOwnedState = record
    valid, moved: int64; { bit i: variable i }
    entry: array[0..maxOwned - 1] of byte;
  end;
  POwnedLoop = ^TOwnedLoop;
  TOwnedLoop = record
    breaks, continues: TOwnedState;
    used: int64; { variables used as the loop found them }
    entryUse: array[0..maxOwned - 1] of TLoc;
    prev: POwnedLoop;
  end;
  { a try statement's protected part: every state in it, joined (an
    exception may leave it anywhere) }
  POwnedTry = ^TOwnedTry;
  TOwnedTry = record
    seen: TOwnedState;
    prev: POwnedTry;
  end;
  POwnedFlow = ^TOwnedFlow;
  TOwnedFlow = record
    { the owned variables, and (null safety) the references followed: for
      those, valid means "may be non-nil" and moved "may be nil" }
    vars: array[0..maxOwned - 1] of PDecl;
    isNull: array[0..maxOwned - 1] of boolean;
    n: longint;
    s: TOwnedState;
    moveLoc: array[0..maxOwned - 1] of TLoc;
    { tick of the last move or assignment }
    changed: array[0..maxOwned - 1] of longint;
    tick: longint;
    loop: POwnedLoop;
    depth: longint;
    tryPart: POwnedTry;
  end;

  PPScope = ^PScope;
  PPtrList = ^TPtrList;

var
  flow: POwnedFlow;   { of the block whose statements are checked }
  ownedTarget: PNode; { the assignment target being checked }
  { the variables the statements of that block declare (nkVarDecl) }
  inlineVars: PPtrList;
  { the scope of the variables declared in the statement sequence being
    checked, made when the first one is (see directSequence) }
  seqScope: PPScope;

procedure ownedClear(var st: TOwnedState);
var
  i: longint;
begin
  st.valid := 0;
  st.moved := 0;
  for i := 0 to maxOwned - 1 do
    st.entry[i] := noLoop;
end;

function ownedLive(const st: TOwnedState): boolean;
begin
  ownedLive := (st.valid <> 0) or (st.moved <> 0);
end;

procedure ownedJoinInto(var a: TOwnedState; const b: TOwnedState);
var
  i: longint;
begin
  a.valid := a.valid or b.valid;
  a.moved := a.moved or b.moved;
  for i := 0 to maxOwned - 1 do
    if b.entry[i] < a.entry[i] then
      a.entry[i] := b.entry[i];
end;

{ e, if it is an owned variable (in parentheses or not). }
function ownedVarOf(e: PNode): PDecl;
begin
  while e^.kind = nkParen do
    e := e^.inner;
  if (e^.kind = nkNameRef) and (e^.refDecl <> nil) and (e^.refDecl^.kind = dkVar) and
     e^.refDecl^.isOwned then
    ownedVarOf := e^.refDecl
  else
    ownedVarOf := nil;
end;

function ownedIndex(v: PDecl): longint;
var
  i: longint;
begin
  ownedIndex := -1; { an enclosing routine's: not followed }
  if (flow = nil) or not v^.isOwned then
    exit;
  for i := 0 to flow^.n - 1 do
    if (flow^.vars[i] = v) and not flow^.isNull[i] then
      exit(i);
end;

procedure ownedGet(var st: TOwnedState);
begin
  if flow <> nil then
    st := flow^.s
  else
    ownedClear(st);
end;

procedure ownedSet(const st: TOwnedState);
begin
  if flow <> nil then
    flow^.s := st;
end;

procedure ownedJoin(const st: TOwnedState);
begin
  if flow <> nil then
    ownedJoinInto(flow^.s, st);
end;

{ What follows is not reached (after exit, break, continue, raise). }
procedure ownedDead;
begin
  if flow <> nil then
    ownedClear(flow^.s);
end;

{ A use of the owned variable v: an error if it may have been moved. }
procedure ownedUse(v: PDecl; r: TRange);
var
  i, d: longint;
  bit: int64;
  l: POwnedLoop;
begin
  i := ownedIndex(v);
  if i < 0 then
    exit;
  if not ownedLive(flow^.s) then
    exit;
  bit := int64(1) shl i;
  if (flow^.s.moved and bit) <> 0 then
  begin
    ms('owned variable ''');
    maN(v^.ident.spelling);
    if (flow^.s.valid and bit) <> 0 then
      ma(''' may have been moved')
    else
      ma(''' was moved');
    errR(r.b, r);
    ms('moved here');
    note(flow^.moveLoc[i]);
    flow^.s.moved := flow^.s.moved and not bit; { reported once }
    flow^.s.valid := flow^.s.valid or bit;
    exit;
  end;
  d := flow^.depth;
  l := flow^.loop;
  while (l <> nil) and (d >= flow^.s.entry[i]) do
  begin
    if (l^.used and bit) = 0 then
    begin
      l^.used := l^.used or bit;
      l^.entryUse[i] := r.b;
    end;
    l := l^.prev;
    d := d - 1;
  end;
end;

procedure ownedChanged(i: longint);
var
  t: POwnedTry;
begin
  flow^.s.entry[i] := noLoop;
  flow^.tick := flow^.tick + 1;
  flow^.changed[i] := flow^.tick;
  t := flow^.tryPart;
  while t <> nil do
  begin
    ownedJoinInto(t^.seen, flow^.s);
    t := t^.prev;
  end;
end;

{ v := object }
procedure ownedAssign(v: PDecl);
var
  i: longint;
  bit: int64;
begin
  i := ownedIndex(v);
  if i < 0 then
    exit;
  if not ownedLive(flow^.s) then
    exit;
  bit := int64(1) shl i;
  flow^.s.valid := flow^.s.valid or bit;
  flow^.s.moved := flow^.s.moved and not bit;
  ownedChanged(i);
end;

{ move v }
procedure ownedMove(v: PDecl; loc: TLoc);
var
  i: longint;
  bit: int64;
begin
  i := ownedIndex(v);
  if i < 0 then
    exit;
  if not ownedLive(flow^.s) then
    exit;
  bit := int64(1) shl i;
  flow^.s.valid := flow^.s.valid and not bit;
  flow^.s.moved := flow^.s.moved or bit;
  flow^.moveLoc[i] := loc;
  ownedChanged(i);
end;

{ v, an owned variable of the block being checked, is declared. }
procedure ownedDeclare(v: PDecl);
begin
  if flow = nil then
    exit;
  if flow^.n = maxOwned then
  begin
    ms('too many owned variables in one routine (at most ');
    maI(maxOwned);
    ma(')');
    errR(v^.loc, identRange(v^.ident));
    exit;
  end;
  flow^.vars[flow^.n] := v;
  flow^.isNull[flow^.n] := false;
  flow^.moveLoc[flow^.n] := 0;
  flow^.changed[flow^.n] := 0;
  flow^.n := flow^.n + 1;
  ownedAssign(v); { nil: nothing to free, not moved }
end;

procedure ownedLoopBegin(var l: TOwnedLoop);
var
  i: longint;
begin
  if flow = nil then
    exit;
  ownedClear(l.breaks);
  ownedClear(l.continues);
  l.used := 0;
  l.prev := flow^.loop;
  flow^.loop := @l;
  flow^.depth := flow^.depth + 1;
  if ownedLive(flow^.s) then
    for i := 0 to flow^.n - 1 do
      if flow^.s.entry[i] > flow^.depth then
        flow^.s.entry[i] := flow^.depth;
end;

{ The loop l ends: before is the state in which it runs no (more)
  iterations. A variable that an iteration may leave moved, and that an
  iteration uses as it finds it, is used after a move. }
procedure ownedLoopEnd(var l: TOwnedLoop; const before: TOwnedState);
var
  i: longint;
  back: TOwnedState;
begin
  if flow = nil then
    exit;
  back := flow^.s;
  ownedJoinInto(back, l.continues);
  for i := 0 to flow^.n - 1 do
    if (((back.moved and l.used) and (int64(1) shl i)) <> 0) and flow^.isNull[i] then
    begin
      ms('''');
      maN(flow^.vars[i]^.ident.spelling);
      ma(''' may be nil here: the loop may set it to nil before its next ' +
         'iteration; check it in the loop');
      err(l.entryUse[i]);
    end
    else if ((back.moved and l.used) and (int64(1) shl i)) <> 0 then
    begin
      ms('owned variable ''');
      maN(flow^.vars[i]^.ident.spelling);
      ma(''' may have been moved by an earlier iteration of the loop');
      err(l.entryUse[i]);
      ms('moved here');
      note(flow^.moveLoc[i]);
    end;
  ownedJoinInto(back, before);
  ownedJoinInto(back, l.breaks);
  for i := 0 to flow^.n - 1 do
    if (back.entry[i] <> noLoop) and (back.entry[i] >= flow^.depth) then
      back.entry[i] := noLoop;
  flow^.loop := l.prev;
  flow^.depth := flow^.depth - 1;
  flow^.s := back;
end;

procedure ownedBreak(isContinue: boolean);
begin
  if flow = nil then
    exit;
  if flow^.loop = nil then
    exit;
  if isContinue then
    ownedJoinInto(flow^.loop^.continues, flow^.s)
  else
    ownedJoinInto(flow^.loop^.breaks, flow^.s);
  ownedDead;
end;

procedure ownedTryBegin(var t: TOwnedTry);
begin
  if flow = nil then
    exit;
  t.seen := flow^.s;
  t.prev := flow^.tryPart;
  flow^.tryPart := @t;
end;

procedure ownedTryEnd(var t: TOwnedTry);
begin
  if flow <> nil then
    flow^.tryPart := t.prev;
end;

{ --- Null safety (objfpc, $nullsafe on) ---

  In a block whose statements start where $nullsafe on, a reference of a
  class, interface or class-reference type written there without ? is
  never nil: what is assigned or passed to it, and the object whose member
  is used, must not be nil. A value may be nil when it is nil, a variable,
  field or function result declared T?, a ?., or a followed variable that
  the flow of the statements does not show to be non-nil: the block's T?
  variables and parameters (narrowed by x <> nil, Assigned(x), x is T and
  assignments), its non-null variables until they are first assigned, a
  function's result and a constructor's own non-null fields (which must be
  assigned by its end). Declarations written outside null-safe code are
  trusted: their values are not nil, and nil may be given to them. They
  are followed in the owned variables' flow, with valid "may be non-nil"
  and moved "may be nil". }

const
  nnNotNil = 0;
  nnMaybeNil = 1;
  nnIsNil = 2;
  { what needs a value not nil (requireNotNil) }
  nwAssign = 1;
  nwParam = 2;
  nwMember = 3;
  nwWith = 4;

var
  nullSafeHere: boolean;

function isNullRef(t: PType): boolean;
begin
  isNullRef := (t <> nil) and (t^.kind in [tyClass, tyInterface, tyClassRef]);
end;

{ The type written for d (a variable, field, property or function's
  result), if any. }
function nullSpec(d: PDecl): PTypeSpec;
begin
  nullSpec := nil;
  case d^.kind of
    dkVar: nullSpec := d^.varSpec;
    dkField: nullSpec := d^.fieldSpec;
    dkProperty: nullSpec := d^.propSpec;
    dkRoutine: nullSpec := d^.resultSpec;
  end;
end;

{ Is d declared T? (its value may be nil)? }
function declNullable(d: PDecl): boolean;
var
  ts: PTypeSpec;
begin
  ts := nullSpec(d);
  declNullable := (ts <> nil) and ts^.tsNullable;
end;

{ Is d a reference that must not be given nil (written without ? in
  null-safe code; not a lambda's result, whose type is not written)? }
function declNonNull(d: PDecl): boolean;
var
  ts: PTypeSpec;
begin
  ts := nullSpec(d);
  declNonNull := (ts <> nil) and ts^.tsNullSafe and not ts^.tsNullable;
end;

function nullTypeOf(d: PDecl): PType;
begin
  nullTypeOf := nil;
  case d^.kind of
    dkVar: nullTypeOf := d^.varType;
    dkField: nullTypeOf := d^.fieldType;
    dkProperty: nullTypeOf := d^.propType;
  end;
end;

function nullIndex(d: PDecl): longint;
var
  i: longint;
begin
  nullIndex := -1;
  if (flow = nil) or (d = nil) then
    exit;
  for i := 0 to flow^.n - 1 do
    if (flow^.vars[i] = d) and flow^.isNull[i] then
      exit(i);
end;

procedure nullSet(i: longint; mayNil, mayNonNil: boolean);
var
  bit: int64;
begin
  if not ownedLive(flow^.s) then
    exit;
  bit := int64(1) shl i;
  if mayNil then
    flow^.s.moved := flow^.s.moved or bit
  else
    flow^.s.moved := flow^.s.moved and not bit;
  if mayNonNil then
    flow^.s.valid := flow^.s.valid or bit
  else
    flow^.s.valid := flow^.s.valid and not bit;
  ownedChanged(i);
end;

{ d is followed from here (if there is room: the owned variables keep
  some), with these states. }
procedure nullTrack(d: PDecl; mayNil, mayNonNil: boolean);
var
  i: longint;
begin
  if (flow = nil) or (flow^.n >= maxOwned - 8) or (nullIndex(d) >= 0) then
    exit;
  i := flow^.n;
  flow^.n := flow^.n + 1;
  flow^.vars[i] := d;
  flow^.isNull[i] := true;
  flow^.moveLoc[i] := 0;
  flow^.changed[i] := 0;
  nullSet(i, mayNil, mayNonNil);
end;

{ d is not nil here (a condition said so). }
procedure nullNarrow(d: PDecl);
var
  i: longint;
  bit: int64;
begin
  i := nullIndex(d);
  if i < 0 then
    exit;
  if not ownedLive(flow^.s) then
    exit;
  bit := int64(1) shl i;
  flow^.s.moved := flow^.s.moved and not bit;
  flow^.s.valid := flow^.s.valid or bit;
  flow^.s.entry[i] := noLoop;
end;

{ A use of followed variable i that needs it not nil: may it be nil?
  (once: reported once, then it counts as assigned) }
function nullUse(i: longint; loc: TLoc; once: boolean): boolean;
var
  bit: int64;
  d: longint;
  l: POwnedLoop;
begin
  nullUse := false;
  if not ownedLive(flow^.s) then
    exit;
  bit := int64(1) shl i;
  if (flow^.s.moved and bit) <> 0 then
  begin
    if once then
    begin
      flow^.s.moved := flow^.s.moved and not bit;
      flow^.s.valid := flow^.s.valid or bit;
    end;
    exit(true);
  end;
  d := flow^.depth;
  l := flow^.loop;
  while (l <> nil) and (d >= flow^.s.entry[i]) do
  begin
    if (l^.used and bit) = 0 then
    begin
      l^.used := l^.used or bit;
      l^.entryUse[i] := loc;
    end;
    l := l^.prev;
    d := d - 1;
  end;
end;

function stripParens(e: PNode): PNode;
begin
  while e^.kind = nkParen do
    e := e^.inner;
  stripParens := e;
end;

{ The declaration a reference is to: a variable or routine (x), a member
  (F, Self.F, o.F); nil otherwise. }
function nullDeclOf(e: PNode): PDecl;
begin
  nullDeclOf := nil;
  if e^.kind = nkNameRef then
    nullDeclOf := e^.refDecl
  else if e^.kind = nkField then
    nullDeclOf := e^.fieldDecl;
end;

{ e is F or Self.F (in a method). }
function isSelfMember(e: PNode): boolean;
begin
  isSelfMember := false;
  if e^.kind = nkNameRef then
    isSelfMember := (e^.refDecl <> nil) and (e^.refDecl^.kind = dkField) and
                    ((e^.refWith = nil) or e^.refWith^.isSelf)
  else if (e^.kind = nkField) and (e^.base^.kind = nkNameRef) then
    isSelfMember := (e^.base^.ref.name = intern('self')) and (e^.fieldDecl <> nil) and
                    (e^.fieldDecl^.kind = dkField);
end;

{ The followed declaration e refers to, or -1. }
function nullIndexOf(e: PNode): longint;
var
  d: PDecl;
begin
  nullIndexOf := -1;
  e := stripParens(e);
  d := nullDeclOf(e);
  if d = nil then
    exit;
  if (d^.kind = dkField) and not isSelfMember(e) then
    exit;
  if (e^.kind = nkNameRef) and (e^.refWith <> nil) and not e^.refWith^.isSelf then
    exit;
  nullIndexOf := nullIndex(d);
end;

{ May e (checked) be nil here? (A followed reference: as the flow has it,
  without reporting.) nnNotNil, nnMaybeNil or nnIsNil. }
function nilness(e: PNode): longint;
var
  t: PType;
  i, a, b: longint;
  bit: int64;
  d: PDecl;
begin
  e := stripParens(e);
  t := e^.typ;
  if (t <> nil) and (t^.kind = tyNil) then
    exit(nnIsNil);
  if not isNullRef(t) then
    exit(nnNotNil);
  i := nullIndexOf(e);
  if i >= 0 then
  begin
    bit := int64(1) shl i;
    if not ownedLive(flow^.s) or ((flow^.s.moved and bit) = 0) then
      exit(nnNotNil);
    if (flow^.s.valid and bit) <> 0 then
      exit(nnMaybeNil);
    exit(nnIsNil);
  end;
  if e^.kind = nkSafeNav then
    exit(nnMaybeNil);
  if e^.kind = nkCoalesce then
    exit(nilness(e^.coRight));
  if e^.kind = nkCondExpr then
  begin
    a := nilness(e^.ceThen);
    b := nilness(e^.ceElse);
    if a = b then
      exit(a);
    exit(nnMaybeNil);
  end;
  if (e^.kind = nkBinary) and (e^.bop = boAs) then
    exit(nilness(e^.lhs));
  d := nullDeclOf(e);
  if e^.kind = nkCall then
    d := e^.callDecl;
  if (e^.kind = nkIndirectCall) and (e^.calleeExpr^.kind = nkField) then
    d := e^.calleeExpr^.fieldDecl;
  if (d <> nil) and declNullable(d) then
    nilness := nnMaybeNil
  else
    nilness := nnNotNil;
end;

{ How e is named in a diagnostic (appended to msg). }
procedure maNullDesc(e: PNode);
begin
  e := stripParens(e);
  if e^.kind = nkNameRef then
  begin
    ma('''');
    maN(e^.ref.spelling);
    ma('''');
  end
  else if e^.kind = nkField then
  begin
    ma('''');
    maN(e^.fieldName.spelling);
    ma('''');
  end
  else if e^.kind = nkCall then
  begin
    ma('the result of ''');
    maN(e^.callee.spelling);
    ma('''');
  end
  else if (e^.kind = nkIndirectCall) and (e^.calleeExpr^.kind = nkField) then
  begin
    ma('the result of ''');
    maN(e^.calleeExpr^.fieldName.spelling);
    ma('''');
  end
  else if e^.kind = nkSafeNav then
    ma('the value of ''?.''')
  else if (e^.typ <> nil) and (e^.typ^.kind = tyNil) then
    ma('nil')
  else
    ma('this value');
end;

{ The name of followed e (a variable, or a field of Self). }
function nullName(e: PNode): PName;
begin
  e := stripParens(e);
  if e^.kind = nkNameRef then
    nullName := e^.ref.spelling
  else
    nullName := e^.fieldName.spelling;
end;

{ What needs the value (why: nwAssign, nwParam, nwMember, nwWith; name:
  the variable's, parameter's or member's; t: its type), with "but ". }
procedure maNullWhy(why: longint; name: PName; t: PType; withBut: boolean);
begin
  if withBut then
    ma('but ');
  case why of
    nwAssign:
      begin
        ma('''');
        maN(name);
        ma(''' is of the non-null type ');
        maT(t);
      end;
    nwParam:
      begin
        ma('parameter ''');
        maN(name);
        ma(''' is of the non-null type ');
        maT(t);
      end;
    nwMember:
      begin
        ma('its member ''');
        maN(name);
        ma(''' is used');
      end;
    nwWith: ma('its members are used in ''with''');
  end;
end;

function nullHasAlt(why: longint): boolean;
begin
  nullHasAlt := (why = nwAssign) or (why = nwMember);
end;

{ Another way out, with ", or ". }
procedure maNullAlt(why: longint; name: PName; t: PType; withOr: boolean);
begin
  if not nullHasAlt(why) then
    exit;
  if withOr then
    ma(', or ');
  if why = nwAssign then
  begin
    ma('declare ''');
    maN(name);
    ma(''' as ''');
    bufAddTypeName(msg, t);
    ma('?''');
  end
  else
  begin
    ma('use ''?.');
    maN(name);
    ma('''');
  end;
end;

{ Where e must not be nil: an error if it may be. }
procedure requireNotNil(e: PNode; why: longint; name: PName; t: PType);
var
  x: PNode;
  i: longint;
  d: PDecl;
  unassigned, isNil: boolean;
begin
  if not nullSafeHere or (e^.typ = nil) or isError(e^.typ) then
    exit;
  x := stripParens(e);
  if x^.typ^.kind = tyNil then
  begin
    ms('nil cannot be used here: ');
    maNullWhy(why, name, t, false);
    if nullHasAlt(why) then
    begin
      ma(' (');
      maNullAlt(why, name, t, false);
      ma(' if it may be nil)');
    end;
    errR(x^.b, nodeRange(x));
    exit;
  end;
  if not isNullRef(x^.typ) then
    exit;
  i := nullIndexOf(x);
  if i >= 0 then
  begin
    d := flow^.vars[i];
    unassigned := not declNullable(d) and
                  not ((d^.kind = dkVar) and (d^.varKind in [vkValueParam, vkVarParam,
                       vkConstParam]) and (d^.varSpec = nil));
    isNil := (flow^.s.valid and (int64(1) shl i)) = 0;
    if not nullUse(i, x^.b, unassigned) then
      exit;
    ms('');
    maNullDesc(x);
    if unassigned and ((d^.kind <> dkVar) or (d^.varSpec <> nil)) then
      ma(' may not have been assigned yet (a reference of a non-null type ' +
         'must be assigned before it is used)')
    else if isNil then
    begin
      ma(' is nil here, ');
      maNullWhy(why, name, t, true);
    end
    else
    begin
      ma(' may be nil here, ');
      maNullWhy(why, name, t, true);
      ma('; check it first (if ');
      maN(nullName(x));
      ma(' <> nil then ...)');
      maNullAlt(why, name, t, true);
    end;
    errR(x^.b, nodeRange(x));
    exit;
  end;
  if nilness(x) = nnNotNil then
    exit;
  ms('');
  maNullDesc(x);
  ma(' may be nil here, ');
  maNullWhy(why, name, t, true);
  ma('; check it first in a local variable');
  maNullAlt(why, name, t, true);
  errR(x^.b, nodeRange(x));
end;

{ target := value (or the initialization of a variable): value must not be
  nil if target is a non-null reference; a followed target gets its
  nil-ness. }
procedure nullAssign(target: PNode; td: PDecl; value: PNode);
var
  tt: PType;
  i, n: longint;
begin
  if not nullSafeHere or (td = nil) then
    exit;
  if (value^.typ = nil) or isError(value^.typ) then
    exit;
  tt := nullTypeOf(td);
  if not isNullRef(tt) then
    exit;
  if declNonNull(td) then
    requireNotNil(value, nwAssign, td^.ident.spelling, tt);
  if target <> nil then
    i := nullIndexOf(target)
  else
    i := nullIndex(td);
  if i < 0 then
    exit;
  n := nilness(value);
  nullSet(i, n <> nnNotNil, n <> nnIsNil);
end;

function nullListHas(const l: TPtrList; d: PDecl): boolean;
var
  i: longint;
begin
  nullListHas := false;
  for i := 0 to l.count - 1 do
    if listGet(l, i) = d then
      exit(true);
end;

{ The followed declarations a condition shows to be non-nil when it is
  true (whenTrue) or false. }
procedure nullFacts(c: PNode; whenTrue: boolean; var out_: TPtrList);
var
  l, r: TPtrList;
  i: longint;
  d: PDecl;
  both: boolean;

  function varOf(e: PNode): PDecl;
  var
    k: longint;
  begin
    k := nullIndexOf(e);
    if k >= 0 then
      varOf := flow^.vars[k]
    else
      varOf := nil;
  end;

  function isNilNode(e: PNode): boolean;
  begin
    e := stripParens(e);
    isNilNode := (e^.typ <> nil) and (e^.typ^.kind = tyNil);
  end;

begin
  c := stripParens(c);
  if (c^.kind = nkUnary) and (c^.uop = uoNot) then
  begin
    nullFacts(c^.operand, not whenTrue, out_);
    exit;
  end;
  if c^.kind = nkBinary then
  begin
    if (c^.bop = boNe) or (c^.bop = boEq) then
    begin
      if whenTrue <> (c^.bop = boNe) then
        exit;
      d := nil;
      if isNilNode(c^.rhs) then
        d := varOf(c^.lhs)
      else if isNilNode(c^.lhs) then
        d := varOf(c^.rhs);
      if d <> nil then
        listAdd(out_, d);
      exit;
    end;
    if c^.bop = boIs then
    begin
      d := varOf(c^.lhs);
      if (d <> nil) and whenTrue then
        listAdd(out_, d);
      exit;
    end;
    if (c^.bop = boAnd) or (c^.bop = boOr) then
    begin
      both := (c^.bop = boAnd) = whenTrue; { each side holds }
      listInit(l);
      listInit(r);
      nullFacts(c^.lhs, whenTrue, l);
      nullFacts(c^.rhs, whenTrue, r);
      if both then
      begin
        for i := 0 to l.count - 1 do
          listAdd(out_, listGet(l, i));
        for i := 0 to r.count - 1 do
          listAdd(out_, listGet(r, i));
      end
      else
        for i := 0 to l.count - 1 do
          if nullListHas(r, PDecl(listGet(l, i))) then
            listAdd(out_, listGet(l, i));
      exit;
    end;
    exit;
  end;
  if (c^.kind = nkCall) and whenTrue and (c^.args.count = 1) and
     (c^.callDecl <> nil) and (c^.callDecl^.kind = dkBuiltin) and
     (c^.callDecl^.builtin = bkAssigned) then
  begin
    d := varOf(PNode(listGet(c^.args, 0)));
    if d <> nil then
      listAdd(out_, d);
  end;
end;

{ x passed for var or out parameter p: the callee may rely on it not being
  nil (var p: T), and leaves it as p's type says. }
procedure nullVarArg(arg: PNode; p: PDecl);
var
  i: longint;
begin
  if not nullSafeHere or not isNullRef(p^.varType) then
    exit;
  if not p^.isOut and declNonNull(p) then
    requireNotNil(arg, nwParam, p^.ident.spelling, p^.varType);
  i := nullIndexOf(arg);
  if i >= 0 then
    nullSet(i, not declNonNull(p), true);
end;

procedure nullNarrowBy(c: PNode; whenTrue: boolean);
var
  facts: TPtrList;
  i: longint;
begin
  if not nullSafeHere or (flow = nil) then
    exit;
  listInit(facts);
  nullFacts(c, whenTrue, facts);
  for i := 0 to facts.count - 1 do
    nullNarrow(PDecl(listGet(facts, i)));
end;

{ The object of the owned variable ov would be kept (or returned)
  through e without move. }
procedure diagOwnedEscape(ov: PDecl; e: PNode; returned: boolean);
begin
  ms('the object of owned variable ''');
  maN(ov^.ident.spelling);
  if returned then
    ma(''' can only be returned with ''move ')
  else
    ma(''' can only be stored with ''move ');
  maN(ov^.ident.spelling);
  ma('''');
  errR(e^.b, nodeRange(e));
end;

{ Reports a change to a const parameter; false if reported. }
function checkModifiable(e: PNode): boolean;
var
  v: PDecl;
begin
  v := ownedVarOf(e);
  if (v <> nil) and (e <> ownedTarget) then
  begin
    { (the callee could replace the object without freeing it) }
    ms('owned variable ''');
    maN(v^.ident.spelling);
    ma(''' cannot be passed as a variable parameter');
    errR(e^.b, nodeRange(e));
    exit(false);
  end;
  v := constParamRoot(e);
  if v = nil then
    exit(true);
  if (v^.owner <> nil) and v^.owner^.isCopyFactory then
  begin
    ms('cannot modify ''');
    maN(v^.ident.spelling);
    ma(''', a copy made by this lambda (copies are read-only)');
  end
  else
  begin
    if v^.isReadOnly then
      ms('cannot modify constant ''')
    else
      ms('cannot modify constant parameter ''');
    maN(v^.ident.spelling);
    ma('''');
  end;
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

{ Is a value of type t kept as the 64 bits of a qword (read unsigned)? }
function isQWordValue(t: PType): boolean;
begin
  isQWordValue := (t <> nil) and isQWord(getBase(t));
end;

function asReal(const c: TConstant): double;
begin
  if c.value.kind = ckReal then
    asReal := c.value.realVal
  else if isQWordValue(c.ty) then
    asReal := uToReal(c.value.intVal)
  else
    asReal := c.value.intVal;
end;

{ A widechar's code as a char: '?' above 255 (Free Pascal's conversion). }
function wideToChar(v: int64): int64;
begin
  if v > 255 then
    wideToChar := ord('?')
  else
    wideToChar := v;
end;

{ A widechar constant above 255 that becomes a char or a string: a warning,
  as Free Pascal gives ('?' in its place). }
procedure checkWideToChar(target: PType; e: PNode);
var
  c: TConstant;
begin
  if (e^.typ = nil) or (getBase(e^.typ)^.kind <> tyWideChar) or
     not ((getBase(target)^.kind = tyChar) or
          (isStringish(target) and not isCharLike(target))) then
    exit;
  if fold(e, c) and (c.value.kind = ckOrdinal) and (c.value.intVal > 255) then
  begin
    ms('the widechar #');
    maI(c.value.intVal);
    ma(' becomes ''?'' in ');
    maT(target);
    diagReportBuf(sevWarning, e^.b, msg, nodeRange(e), noRange);
  end;
end;

{ Does constant v of type vt lie within t's values? (qword values are
  unsigned.) }
function constantInRange(v: int64; vt, t: PType): boolean;
var
  bd: TOrdinalBounds;
begin
  if (getBase(vt)^.kind = tyWideChar) and (getBase(t)^.kind = tyChar) then
    v := wideToChar(v);
  if isQWord(t) then
    exit(isQWordValue(vt) or (v >= 0));
  if isQWordValue(vt) and (v < 0) then { 2^63 or more }
    exit(false);
  bd := getOrdinalBounds(t);
  constantInRange := (v >= bd.lo) and (v <= bd.hi);
end;

{ The order of label value v of a selector of type selTy: qword's values
  compare unsigned. }
function caseKey(v: int64; selTy: PType): int64;
begin
  {$Q-}{$R-}
  if isQWordValue(selTy) then
    caseKey := v xor (-9223372036854775807 - 1)
  else
    caseKey := v;
  {$Q+}{$R+}
end;

{ T(v) of a value of type from: integer types wrap to their width
  (byte(300) = 44), and widechar to 16 bits; a widechar above 255 is '?' as
  a char (as Free Pascal converts it); other ordinal types keep the value. }
function castOrdinalValue(v: int64; from, t: PType): int64;
var
  i: PType;
  mask, u: int64;
begin
  if getBase(t)^.kind = tyWideChar then
    exit(v and 65535);
  if (getBase(t)^.kind = tyChar) and (getBase(from)^.kind = tyWideChar) then
    exit(wideToChar(v));
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
  cmp, u64: boolean;
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
        if e^.strWide <> 0 then
          exit(mkConst(t, constOrdinal(e^.strWide), c));
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
    nkField: { TFoo.C }
      begin
        if e^.constRef <> nil then
          exit(fold(e^.constRef, c));
        exit;
      end;
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
            exit(mkConst(t, constOrdinal(castOrdinalValue(not v.value.intVal, t, t)), c));
          end;
        end;
      end;
    nkBinary:
      begin
        if not fold(e^.lhs, l) or not fold(e^.rhs, r) or
           (l.value.kind = ckString) or (r.value.kind = ckString) then
          exit;
        if not isOrdinal(t) and (t^.kind <> tyReal) then
          exit; { 'a' + 'b': a string (not folded) }
        isReal := (l.value.kind = ckReal) or (r.value.kind = ckReal);
        a := l.value.intVal;
        b := r.value.intVal;
        x := asReal(l);
        y := asReal(r);
        { wrap around in 64 bits; results that do not fit their type are
          reported by evaluateConstant }
        wide := isIntegerLike(t) and (getBase(t)^.bits = 64);
        { qword operands: div, mod and comparisons are unsigned }
        u64 := not isReal and (e^.operandType <> nil) and isQWord(e^.operandType);
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
              if u64 then
              begin
                if b = 0 then
                  exit;
                exit(mkConst(t, constOrdinal(uDiv(a, b)), c));
              end;
              if (b = 0) or ((a = low(int64)) and (b = -1)) then
                exit;
              exit(mkConst(t, constOrdinal(a div b), c));
            end;
          boMod:
            begin
              if u64 then
              begin
                if b = 0 then
                  exit;
                exit(mkConst(t, constOrdinal(uMod(a, b)), c));
              end;
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
              else if u64 then
                case e^.bop of
                  boEq: cmp := a = b;
                  boNe: cmp := a <> b;
                  boLt: cmp := uCompare(a, b) < 0;
                  boLe: cmp := uCompare(a, b) <= 0;
                  boGt: cmp := uCompare(a, b) > 0;
                else
                  cmp := uCompare(a, b) >= 0;
                end
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
          exit(mkConst(t, constOrdinal(castOrdinalValue(v.value.intVal, v.ty, t)), c));
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
          if (bk = bkHigh) and isQWord(at) then { 2^64-1: its 64 bits }
            exit(mkConst(t, constOrdinal(-1), c));
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
            else if isQWordValue(v.ty) then { already non-negative }
              exit(mkConst(t, constOrdinal(v.value.intVal), c))
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
  if (c.value.kind = ckOrdinal) and isIntegerLike(c.ty) and
     not isQWordValue(c.ty) then { (any 64 bits are a qword) }
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
function lookupMemberPath(const quals: TPtrList; const name: TIdent): PDecl; forward;

var
  tupleTypes: TPtrList; { see getTupleType }
  tupleTypesInit: boolean;
  hiddenVars: longint;  { see makeHiddenVar }

{ The name of a tuple's n-th element that has none: ItemN. }
function tupleItemName(n: longint; loc: TLoc): TIdent;
var
  id: TIdent;
begin
  id.name := intern('item' + intToStr(n));
  id.spelling := intern('Item' + intToStr(n));
  id.loc := loc;
  tupleItemName := id;
end;

{ The tuple type with these element types (of PType) and names (of PIdent;
  none: Item1, Item2, ...). There is one for each such list, so that tuple
  types written alike are the same type. }
function getTupleType(const types, names: TPtrList): PType;
var
  own, all: TPtrList;
  i, k: longint;
  r: PType;
  same: boolean;
  f: PDecl;
  fl: PFieldList;
  id: PIdent;
begin
  if not tupleTypesInit then
  begin
    listInit(tupleTypes);
    tupleTypesInit := true;
  end;
  listInit(own);
  for i := 0 to types.count - 1 do
    if names.count = 0 then
      listAdd(own, newIdent(tupleItemName(i + 1, noLoc)))
    else
      listAdd(own, listGet(names, i));
  for k := 0 to tupleTypes.count - 1 do
  begin
    r := PType(listGet(tupleTypes, k));
    same := r^.allFields.count = types.count;
    i := 0;
    while same and (i < types.count) do
    begin
      f := PDecl(listGet(r^.allFields, i));
      same := (f^.fieldType = PType(listGet(types, i))) and
              (f^.ident.name = PIdent(listGet(own, i))^.name);
      i := i + 1;
    end;
    if same then
      exit(r);
  end;
  listInit(all);
  fl := PFieldList(arenaAlloc(sizeof(TFieldList)));
  listInit(fl^.fixed);
  fl^.variant := nil;
  for i := 0 to types.count - 1 do
  begin
    id := PIdent(listGet(own, i));
    f := newDecl(dkField, id^, id^.loc);
    f^.fieldSpec := nil;
    f^.fieldType := PType(listGet(types, i));
    listAdd(all, f);
    listAdd(fl^.fixed, f);
  end;
  r := newRecordType(fl, all, false);
  r^.isTupleRecord := true;
  listAdd(tupleTypes, r);
  getTupleType := r;
end;

{ A hidden variable of type t of the block being checked (a tuple is built
  in one); nil outside the statements of a block. }
function makeHiddenVar(t: PType; loc: TLoc): PDecl;
var
  id: TIdent;
  v: PDecl;
begin
  if inlineVars = nil then
    exit(nil);
  hiddenVars := hiddenVars + 1;
  id.name := intern('$tuple' + intToStr(hiddenVars));
  id.spelling := id.name;
  id.loc := loc;
  v := newDecl(dkVar, id, loc);
  v^.varSpec := nil;
  v^.varKind := vkVariable;
  v^.isInlineVar := true;
  v^.varType := t;
  v^.owner := curRoutine;
  v^.isUsed := true;
  listAdd(inlineVars^, v);
  makeHiddenVar := v;
end;

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
  tupTypes, tupIds: TPtrList;
  es: PTypeSpec;
  named: boolean;
  d, cd, prev: PDecl;
  t, elem, idx, res: PType;
  i, j: longint;
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
        if ts^.tquals.count > 0 then { TOuter.TInner }
        begin
          d := lookupMemberPath(ts^.tquals, ts^.tname);
          if d = nil then
            exit;
          if d^.kind = dkType then
          begin
            warnHints(d, ts^.tname);
            if d^.declType <> nil then
              buildType := d^.declType;
            exit;
          end;
          ms('''');
          maN(ts^.tname.spelling);
          ma(''' is a ' + kindName(d) + ', not a type');
          errR(ts^.tname.loc, identRange(ts^.tname));
          noteDeclaredHere(d);
          exit;
        end;
        d := lookup(ts^.tname.name);
        if (d <> nil) and (d^.kind = dkType) then
          warnHints(d, ts^.tname);
        if d = nil then
        begin
          if isGenericName(ts^.tname.name) then
          begin
            ms('''');
            maN(ts^.tname.spelling);
            ma(''' is a generic; give its type arguments: specialize ');
            maN(ts^.tname.spelling);
            ma('<...>');
            errR(ts^.tname.loc, identRange(ts^.tname));
            exit;
          end;
          ms('unknown type name ''');
          maN(ts^.tname.spelling);
          ma('''');
          maDidYouMean(ts^.tname.name);
          errR(ts^.tname.loc, identRange(ts^.tname));
          exit;
        end;
        if d^.kind = dkType then
        begin
          if (d^.declType <> nil) and (d^.declType^.kind = tyClass) and
             (d^.declType^.classHelperFor <> nil) then
          begin
            ms(helperKindName(d^.declType) + ' ''');
            maN(ts^.tname.spelling);
            ma(''' cannot be used as a type');
            errR(ts^.tname.loc, identRange(ts^.tname));
            exit;
          end;
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
    tsAsync:
      begin
        t := nil;
        if ts^.asyncInner <> nil then
        begin
          t := resolveType(ts^.asyncInner, nil);
          if isError(t) then
            exit(errorType);
        end;
        if ts^.asyncIterator then
        begin
          if (t^.kind = tyUntyped) or containsFile(t) then
          begin
            ms('the values of an iterator cannot be ');
            maT(t);
            errR(ts^.asyncInner^.b, specRange(ts^.asyncInner));
            exit(errorType);
          end;
          exit(iteratorType(t));
        end;
        exit(taskType(t));
      end;
    tsSpecialize:
      begin
        d := specializeType(ts^.spName, ts^.spArgs);
        if d = nil then
          exit;
        if d^.declType <> nil then
          exit(d^.declType);
        { the specialization being declared, inside itself: as its name }
        es := newTypeSpec(tsNamed, ts^.spName.loc, ts^.spName.loc);
        es^.tname := d^.ident;
        es^.tname.loc := ts^.spName.loc;
        listInit(es^.tquals);
        buildType := resolveType(es, nil);
      end;
    tsTuple:
      begin
        listInit(tupTypes);
        listInit(tupIds);
        ok := true;
        for i := 0 to ts^.tupSpecs.count - 1 do
        begin
          es := PTypeSpec(listGet(ts^.tupSpecs, i));
          t := resolveType(es, nil);
          if isError(t) then
            ok := false
          else if containsFile(t) then
          begin
            ms('the elements of a tuple cannot be files');
            errR(es^.b, specRange(es));
            ok := false;
          end;
          id := PIdent(listGet(ts^.tupNames, i));
          named := id^.name <> nil;
          if not named then
            id := newIdent(tupleItemName(tupIds.count + 1, es^.b));
          for j := 0 to tupIds.count - 1 do
            if PIdent(listGet(tupIds, j))^.name = id^.name then
            begin
              ms('duplicate element name ''');
              maN(id^.spelling);
              ma('''');
              if named then
                errR(id^.loc, specRange(es))
              else
                errR(es^.b, specRange(es));
              ok := false;
              break;
            end;
          listAdd(tupTypes, t);
          listAdd(tupIds, id);
        end;
        if ok then
          buildType := getTupleType(tupTypes, tupIds);
      end;
    tsEnum:
      begin
        { (integer, string): every name is a type's, so it is a tuple type }
        if ts^.enumTuple <> nil then
        begin
          ok := true;
          for i := 0 to ts^.enumNames.count - 1 do
          begin
            d := lookup(PIdent(listGet(ts^.enumNames, i))^.name);
            if (d = nil) or (d^.kind <> dkType) then
              ok := false;
          end;
          if ok then
            exit(resolveType(ts^.enumTuple, nil));
        end;
        t := newEnumType;
        for i := 0 to ts^.enumNames.count - 1 do
        begin
          id := PIdent(listGet(ts^.enumNames, i));
          cd := newDecl(dkConst, id^, id^.loc);
          cd^.constType := t;
          cd^.constValue := constOrdinal(t^.enumValues.count);
          if not ts^.enumScoped then
            declare(cd)
          else { reached only as TColor.Red: not in the scope }
            for j := 0 to t^.enumValues.count - 1 do
            begin
              prev := PDecl(listGet(t^.enumValues, j));
              if prev^.ident.name = id^.name then
              begin
                ms('redefinition of ''');
                maN(id^.spelling);
                ma('''');
                errR(id^.loc, identRange(id^));
                ms('previous definition is here');
                note(prev^.loc);
                break;
              end;
            end;
          listAdd(t^.enumValues, cd);
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
        if getBase(hi.ty)^.kind = tyWideChar then { 'a'..#$100: of widechar }
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
        begin
          t := newArrayType(PType(listGet(indexes, i)), t, ts^.arrPacked);
          t^.isBitPackedArray := ts^.arrBitPacked;
        end;
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
             not ((ts^.rtOfObject or ts^.rtReference) and
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
        begin
          res := newRoutineType(ts^.rtParams, res, ts^.rtCallConv = ccCDecl);
          res^.routineReference := ts^.rtReference;
          buildType := res;
        end;
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
        if ts^.elemSpec = nil then { file (Turbo/FPC): untyped }
          exit(newFileType(nil));
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
        t := newRecordType(ts^.fields, all, ts^.recPacked);
        t^.recordPackRecords := ts^.recPackRecords;
        if ts^.recBitPacked then
        begin
          t^.isBitPackedRecord := true;
          for i := 0 to all.count - 1 do { ordinals, and what needs no alignment }
          begin
            cd := PDecl(listGet(all, i));
            if (cd^.fieldType <> nil) and not isError(cd^.fieldType) and
               not isOrdinal(cd^.fieldType) and not isByteAligned(cd^.fieldType) then
            begin
              ms('a field of type ');
              maT(cd^.fieldType);
              ma(' in a bitpacked record is not supported yet');
              errR(cd^.loc, identRange(cd^.ident));
            end;
          end;
        end;
        buildType := t;
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
    tsHelper:
      begin
        if ts^.helperIsRecord then
          ms('a record helper must be declared in a ''type'' section')
        else
          ms('a type helper must be declared in a ''type'' section');
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
  if ts^.tsNullable and not isError(t) then { T? }
  begin
    if not isNullRef(t) then
    begin
      ms('''?'' makes a class, interface or class-reference type nullable; ');
      maT(t);
      ma(' is not one');
      errR(ts^.b, specRange(ts));
      t := errorType;
    end
    else if declName <> nil then
    begin
      ms('a nullable type cannot be given a name; write ''');
      bufAddTypeName(msg, t);
      ma('?'' where it is used');
      errR(ts^.b, specRange(ts));
      t := errorType;
    end;
  end;
  if (declName <> nil) and not isError(t) and not isBuiltinType(t) and
     (ts^.kind <> tsSpecialize) and { (so is a specialization) }
     not ((t^.kind = tyRecord) and t^.isTupleRecord) and { (a tuple type is shared by its uses) }
     (t^.kind <> tyTask) and (t^.kind <> tyIterator) then { (and so are tasks and iterators) }
    setDeclName(t, declName);
  ts^.resolved := t;
  resolveType := t;
end;

{ --- Declarations --- }

{ The name of the type declared as id: TOuter.TInner for a nested one. }
function typeDeclName(const id: TIdent): PName;
begin
  if nestPrefix = nil then
    typeDeclName := id.spelling
  else
    typeDeclName := intern(nameStr(nestPrefix) + '.' + nameStr(id.spelling));
end;

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
procedure declareClassVar(c: PType; v: PDecl); forward;
procedure declareInterface(td: PDecl); forward;
procedure declareAdvancedRecord(td: PDecl); forward;
procedure declareHelper(td: PDecl); forward;

procedure declareType(td: PDecl);
begin
  if td^.tgGeneric then
  begin
    declareGenericType(td);
    exit;
  end;
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
  if td^.typeSpec^.kind = tsHelper then
  begin
    declareHelper(td);
    exit;
  end;
  if td^.typeSpec^.kind = tsInterface then
  begin
    declareInterface(td);
    exit;
  end;
  td^.declType := resolveType(td^.typeSpec, typeDeclName(td^.ident));
  declare(td);
end;

function memberTableOf(t: PType): PType; forward;
procedure checkMemberAccess(d: PDecl; owner: PType; const id: TIdent); forward;
procedure maDidYouMeanMember(c: PType; name: PName); forward;

{ A type declared in a class or advanced record (in cur, its member
  scope). }
procedure declareNestedType(td: PDecl);
begin
  if td^.typeSpec^.kind = tsHelper then
  begin
    if td^.typeSpec^.helperIsRecord then
      ms('record helpers cannot be declared in a class or record')
    else
      ms('type helpers cannot be declared in a class or record');
    errR(td^.ident.loc, identRange(td^.ident));
    td^.declType := errorType;
    declare(td);
    exit;
  end;
  if td^.typeSpec^.kind = tsInterface then
  begin
    ms('interfaces cannot be declared in a class or record yet');
    errR(td^.ident.loc, identRange(td^.ident));
    td^.declType := errorType;
    declare(td);
    exit;
  end;
  declareType(td);
end;

function isForwardClassDecl(d: PDecl): boolean;
begin
  isForwardClassDecl := (d <> nil) and (d^.kind = dkType) and
                        (d^.typeSpec^.kind = tsClass) and d^.typeSpec^.isForwardClass;
end;

{ Members of outer declared with td: a nested class or record knows its
  outer one; an enumeration's values are members too. }
procedure addNestedMembers(outer, td: PDecl; var members: TPtrList; var own: TIntMap);
var
  t, mt: PType;
  i: longint;
  v: PDecl;
begin
  t := td^.declType;
  if (t <> nil) and (t^.kind = tyClass) then
    t^.classOuter := PType(outer)
  else
  begin
    mt := memberTableOf(t);
    if mt <> nil then
      mt^.classOuter := PType(outer);
  end;
  if (t = nil) or (t^.kind <> tyEnum) or (td^.typeSpec^.kind <> tsEnum) or
     td^.typeSpec^.enumScoped then { (a scoped one's: TOuter.TEnum.V) }
    exit;
  for i := 0 to t^.enumValues.count - 1 do
  begin
    v := PDecl(listGet(t^.enumValues, i));
    v^.vis := td^.vis;
    if mapGet(own, v^.ident.name^.id) = nil then
    begin
      mapPut(own, v^.ident.name^.id, v);
      listAdd(members, v);
    end;
  end;
end;

{ The member name of the class or advanced record that quals name (TOuter,
  TOuter.TInner, ...); nil after a diagnostic. }
function lookupMemberPath(const quals: TPtrList; const name: TIdent): PDecl;
var
  c, owner, t: PType;
  d: PDecl;
  i: longint;
  id: TIdent;
begin
  lookupMemberPath := nil;
  c := nil;
  for i := 0 to quals.count do
  begin
    if i < quals.count then
      id := PIdent(listGet(quals, i))^
    else
      id := name;
    if c = nil then
    begin
      d := lookup(id.name);
      if d = nil then
      begin
        diagUndeclared(id);
        exit;
      end;
      d^.isUsed := true;
    end
    else
    begin
      owner := nil;
      d := findMember(c, id.name, owner);
      if d = nil then
      begin
        ms('no member named ''');
        maN(id.spelling);
        if c^.classRecord <> nil then
          ma(''' in record ')
        else
          ma(''' in class ');
        maT(c);
        maDidYouMeanMember(c, id.name);
        errR(id.loc, identRange(id));
        exit;
      end;
      checkMemberAccess(d, owner, id);
    end;
    if i = quals.count then
      exit(d);
    t := nil;
    if d^.kind = dkType then
      t := d^.declType;
    c := nil;
    if (t <> nil) and (t^.kind = tyClass) then
      c := t
    else
      c := memberTableOf(t);
    if c = nil then
    begin
      if (t = nil) or not isError(t) then
      begin
        ms('''');
        maN(id.spelling);
        ma(''' is not a class or record with members');
        errR(id.loc, identRange(id));
      end;
      exit;
    end;
  end;
end;

{ Resolves the pointer types since mark whose targets are visible now
  (declared in a class); the others stay pending. }
procedure resolvePendingPointersFrom(mark: longint);
var
  i, j: longint;
  pp: PPendingPointer;
  d: PDecl;
begin
  i := mark;
  while i < pendingPointers.count do
  begin
    pp := PPendingPointer(listGet(pendingPointers, i));
    d := lookup(pp^.spec^.tname.name);
    if (d <> nil) and (d^.kind = dkType) and (d^.declType <> nil) then
    begin
      pp^.ptr^.pointee := d^.declType;
      for j := i to pendingPointers.count - 2 do
        listSet(pendingPointers, j, listGet(pendingPointers, j + 1));
      listPop(pendingPointers);
    end
    else
      i := i + 1;
  end;
end;

{ ts in the heading of a method of c: the nested types of c and of the
  classes around it are visible. }
function resolveTypeIn(c: PType; ts: PTypeSpec): PType;
var
  saved: PScope;
  chain: TPtrList;
  o: PType;
  i: longint;
begin
  listInit(chain);
  o := c;
  while o <> nil do
  begin
    listAdd(chain, o);
    o := o^.classOuter;
  end;
  saved := cur;
  for i := chain.count - 1 downto 0 do
    cur := newNestedScope(cur, PType(listGet(chain, i)));
  resolveTypeIn := resolveType(ts, nil);
  cur := saved;
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
procedure checkRaceDecl(q: longint; t: PType; const id: TIdent; const what: string); forward;
function checkConstInit(init: PNode; t: PType; var folded: TConstValue): boolean; forward;

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
  setDeclName(c, typeDeclName(td^.ident));
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
  { a record's method gets the record by reference, a type helper's the
    value it extends; in a class method, Self is the class (a class
    reference) }
  if c^.classRecord <> nil then
  begin
    self_^.varKind := vkVarParam;
    self_^.varType := c^.classRecord;
  end
  else if c^.classHelperFor <> nil then
  begin
    self_^.varKind := vkVarParam;
    self_^.varType := c^.classHelperFor;
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

{ The class constructor or destructor r of class or advanced record c: one
  of each, without parameters or directives, in a type declared at program
  or unit level (as in Free Pascal); static, so without Self. }
procedure defineClassInit(c: PType; r: PDecl);
var
  kind, what, directive: string;
  prev: PDecl;
begin
  if r^.classInit = ciConstructor then
    kind := 'class constructor'
  else
    kind := 'class destructor';
  if c^.classRecord <> nil then
    what := 'record'
  else
    what := 'class';
  r^.ownerClass := c;
  r^.parent := nil;
  r^.level := 1;
  resolveSignature(r);
  if curRoutine <> nil then
  begin
    ms('a ' + kind + ' needs a ' + what + ' declared at program or unit level');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if r^.classInit = ciConstructor then
    prev := c^.classCtor
  else
    prev := c^.classDtor;
  if prev <> nil then
  begin
    ms('only one ' + kind + ' can be declared in ' + what + ' ');
    maT(c);
    errR(r^.ident.loc, identRange(r^.ident));
    noteDeclaredHere(prev);
    exit;
  end;
  if r^.params.count > 0 then
  begin
    ms('a ' + kind + ' cannot have parameters');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  directive := '';
  if r^.isVirtual then
    directive := 'virtual'
  else if r^.isOverride then
    directive := 'override'
  else if r^.isAbstract then
    directive := 'abstract'
  else if r^.hasOverloadDirective then
    directive := 'overload'
  else if r^.isInline then
    directive := 'inline'
  else if r^.callConv <> ccDefault then
    directive := callConvName(r^.callConv);
  if directive <> '' then
  begin
    ms('''' + directive + ''' cannot be used with a ' + kind);
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  if r^.classInit = ciConstructor then
    c^.classCtor := r
  else
    c^.classDtor := r;
end;

{ generic function M<T>(...) in class c: a member found only by its key
  (M<N>), not checked; its specializations are made from its
  implementation (instantiateMethod). Not virtual: each specialization is a
  method of its own. }
procedure defineGenericMethod(c: PType; r: PDecl);
var
  i, j: longint;
  pi, pj: PIdent;
  bad: string;
begin
  r^.ownerClass := c;
  r^.parent := nil;
  for i := 0 to r^.typeParams.count - 1 do
    for j := 0 to i - 1 do
    begin
      pi := PIdent(listGet(r^.typeParams, i));
      pj := PIdent(listGet(r^.typeParams, j));
      if pi^.name = pj^.name then
      begin
        ms('duplicate type parameter ''');
        maN(pi^.spelling);
        ma('''');
        errR(pi^.loc, identRange(pi^));
      end;
    end;
  bad := '';
  if r^.isVirtual or r^.isOverride or r^.isAbstract then
    bad := 'a generic method cannot be virtual, override or abstract (each ' +
           'specialization is a method of its own)'
  else if (r^.methodKind = mtConstructor) or (r^.methodKind = mtDestructor) then
    bad := 'a constructor or a destructor cannot be generic'
  else if r^.methodKind = mtOperator then
    bad := 'an operator cannot be generic';
  if bad <> '' then
  begin
    ms(bad);
    errR(r^.ident.loc, identRange(r^.ident));
  end;
end;

procedure defineMethod(c, parentClass: PType; r, inh: PDecl; owner: PType;
                       var vtable: TPtrList);
var
  x, match: PDecl;
  bad: string;
begin
  r^.ownerClass := c;
  r^.parent := nil;
  r^.level := 1;
  resolveSignature(r);
  { a static class method has no Self: it is called like a routine }
  if r^.isStatic then
  begin
    bad := '';
    if not r^.isClassMethod then
      bad := 'only class methods can be ''static'''
    else if r^.isVirtual then
      bad := '''static'' cannot be used with ''virtual'''
    else if r^.isOverride then
      bad := '''static'' cannot be used with ''override'''
    else if r^.isAbstract then
      bad := '''static'' cannot be used with ''abstract''';
    if bad <> '' then
    begin
      ms(bad);
      errR(r^.ident.loc, identRange(r^.ident));
      r^.isStatic := false;
    end;
  end;
  if not r^.isStatic then
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
    if (a^.varType <> b^.varType) or (a^.varKind <> b^.varKind) or
       (a^.isConstRef <> b^.isConstRef) then
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
  if p^.propIsClass then
    ma(' (use a static class ')
  else
    ma(' (use a ');
  if isRead then
    ma('function (')
  else
    ma('procedure (');
  first := true;
  for i := 0 to p^.propParams.count - 1 do
  begin
    q := PDecl(listGet(p^.propParams, i));
    if not first then
      ma('; ');
    first := false;
    if q^.isConstRef then
      ma('constref ')
    else if q^.varKind = vkConstParam then
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
{ The type of accessor d of property p when it is storage: a class
  property's class variable, an instance property's field; else nil. }
function storageType(p, d: PDecl): PType;
begin
  storageType := nil;
  if p^.propIsClass then
  begin
    if (d^.kind = dkVar) and d^.isClassVar then
      storageType := d^.varType;
  end
  else if d^.kind = dkField then
    storageType := d^.fieldType;
end;

function readFits(p, x: PDecl; t: PType; leading: longint): boolean;
begin
  readFits := x^.isFunction and (x^.params.count = leading) and
              indexParamsMatch(p, x) and (x^.resultType = t) and
              (not p^.propIsClass or x^.isStatic);
end;

function writeFits(p, x: PDecl; t: PType; leading: longint): boolean;
var
  prm: PDecl;
begin
  writeFits := false;
  if (x^.methodKind = mtMethod) and not x^.isFunction and (x^.asyncType = nil) and
     (x^.iterType = nil) and
     (x^.params.count = leading + 1) and indexParamsMatch(p, x) and
     (not p^.propIsClass or x^.isStatic) then
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
    ok := (plain and (storageType(p, rd) = t)) or
          ((rd^.kind = dkRoutine) and readFits(p, rd, t, leading));
    if not ok and not isError(t) and paramsOK then
    begin
      ms('''');
      maN(p^.readName.spelling);
      if p^.propIsClass then
        ma(''' cannot be read as class property ''')
      else
        ma(''' cannot be read as property ''');
      maN(p^.ident.spelling);
      ma(''' of type ');
      maT(t);
      if plain and p^.propIsClass then
        ma(' (use a class variable of that type or a static class function ' +
           'without parameters returning it)')
      else if plain then
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
    ok := (plain and (storageType(p, wr) = t)) or
          ((wr^.kind = dkRoutine) and writeFits(p, wr, t, leading));
    if not ok and not isError(t) and paramsOK then
    begin
      ms('''');
      maN(p^.writeName.spelling);
      if p^.propIsClass then
        ma(''' cannot be written as class property ''')
      else
        ma(''' cannot be written as property ''');
      maN(p^.ident.spelling);
      ma(''' of type ');
      maT(t);
      if plain and p^.propIsClass then
        ma(' (use a class variable of that type or a static class procedure ' +
           'with one parameter of it)')
      else if plain then
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
function findInterfaceProperty(i: PType; name: PName): PDecl;
var
  k: longint;
  p: PDecl;
begin
  findInterfaceProperty := nil;
  while i <> nil do
  begin
    for k := 0 to i^.intfProps.count - 1 do
    begin
      p := PDecl(listGet(i^.intfProps, k));
      if p^.ident.name = name then
        exit(p);
    end;
    i := i^.intfParent;
  end;
end;

{ specialize IGen<...> in a class's or interface's heading: the interface
  it is, or nil after a diagnostic. }
function specializedInterface(const id: TIdent; spec: PTypeSpec): PType;
var
  t: PType;
begin
  specializedInterface := nil;
  t := resolveType(spec, nil);
  if isError(t) then
    exit;
  if t^.kind <> tyInterface then
  begin
    ms('');
    maT(t);
    ma(' is not an interface type');
    errR(spec^.b, specRange(spec));
    exit;
  end;
  if not t^.intfDefined then
  begin
    ms('interface ');
    maT(t);
    ma(' is not defined yet');
    errR(id.loc, specRange(spec));
    exit;
  end;
  specializedInterface := t;
end;

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

{ property P: T read GetP [write SetP] in interface i (objfpc): its
  accessors are methods of i (or of its ancestors), a function without
  parameters returning T and a procedure taking one T. Array properties,
  index and default specifiers are not supported. False after an error. }
function defineInterfaceProperty(i: PType; p: PDecl; const earlier: TPtrList): boolean;
var
  t: PType;
  k: longint;
  dup: boolean;
  rd, wr: PDecl;

  function accessor(const n: TIdent; isRead: boolean): PDecl;
  var
    r: PDecl;
    fits: boolean;
  begin
    accessor := nil;
    if not identValid(n) then
      exit;
    r := findInterfaceMethod(i, n.name);
    fits := false;
    if r <> nil then
    begin
      if isRead then
        fits := r^.isFunction and (r^.params.count = 0) and (r^.resultType = t)
      else
        fits := not r^.isFunction and (r^.params.count = 1) and
                (PDecl(listGet(r^.params, 0))^.varKind <> vkVarParam) and
                (PDecl(listGet(r^.params, 0))^.varType = t);
    end;
    if not fits then
    begin
      ms('''');
      maN(n.spelling);
      ma(''' must be a method of interface ');
      maT(i);
      if isRead then
        ma(': a function without parameters returning ')
      else
        ma(': a procedure with one parameter of type ');
      maT(t);
      errR(n.loc, identRange(n));
      exit;
    end;
    accessor := r;
  end;

begin
  defineInterfaceProperty := false;
  t := resolveType(p^.propSpec, nil);
  p^.propType := t;
  if (p^.propParams.count > 0) or (p^.propIndex <> nil) or p^.propIsDefault or
     p^.propIsClass or (p^.propDefaultValue <> nil) then
  begin
    ms('the properties of an interface are plain ones (no array property, ' +
       'index, default, class or stored value) for now');
    errR(p^.ident.loc, identRange(p^.ident));
    exit;
  end;
  dup := (findInterfaceMethod(i, p^.ident.name) <> nil) or
         (findInterfaceProperty(i, p^.ident.name) <> nil);
  for k := 0 to earlier.count - 1 do
    if PDecl(listGet(earlier, k))^.ident.name = p^.ident.name then
      dup := true;
  if dup then
  begin
    ms('''');
    maN(p^.ident.spelling);
    ma(''' is already declared in interface ');
    maT(i);
    errR(p^.ident.loc, identRange(p^.ident));
    exit;
  end;
  if isError(t) then
    exit;
  rd := accessor(p^.readName, true);
  wr := accessor(p^.writeName, false);
  if (identValid(p^.readName) and (rd = nil)) or
     (identValid(p^.writeName) and (wr = nil)) then
    exit;
  if (rd = nil) and (wr = nil) then
  begin
    ms('property ''');
    maN(p^.ident.spelling);
    ma(''' needs ''read'' or ''write''');
    errR(p^.ident.loc, identRange(p^.ident));
    exit;
  end;
  p^.readDecl := rd;
  p^.writeDecl := wr;
  defineInterfaceProperty := true;
end;

procedure defineInterfaceMembers(i: PType; ts: PTypeSpec);
var
  parentIntf: PType;
  own, all, props, defined: TPtrList;
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
    if ts^.intfParentSpec <> nil then { interface(specialize IBase<T>) }
      parentIntf := specializedInterface(ts^.intfParentName, ts^.intfParentSpec)
    else if ts^.intfParentName.name = intern(lowerStr(nameStr(i^.declName))) then
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
  listInit(props);
  for k := 0 to ts^.intfMemberList.count - 1 do
  begin
    r := PDecl(listGet(ts^.intfMemberList, k));
    if r^.kind = dkProperty then
    begin
      listAdd(props, r); { (after the methods: they name them) }
      continue;
    end;
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
  listInit(defined);
  for k := 0 to props.count - 1 do
    if defineInterfaceProperty(i, PDecl(listGet(props, k)), defined) then
      listAdd(defined, listGet(props, k));
  i^.intfProps := defined;
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
  pt: PType;
  parentClass, owner, pc, it: PType;
  members, fields, vtable, listed, listedWhere, impls: TPtrList;
  own: TIntMap;
  i, k, j: longint;
  m, prev, inh, defaultProp, overloadOf, td, fwd: PDecl;
  pi: PInterfaceImpl;
  savedScope: PScope;
  savedPrefix: PName;
  pendingClassMark, pendingPointerMark: longint;

  { the interfaces class(TParent, I1, I2) lists }
  procedure addListed(id: PIdent; spec: PTypeSpec);
  var
    t: PType;
    j: longint;
  begin
    if spec <> nil then
      t := specializedInterface(id^, spec)
    else
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
  if ts^.classParentSpec <> nil then { class(specialize TBase<T>) }
  begin
    pt := resolveType(ts^.classParentSpec, nil);
    if pt^.kind = tyInterface then
      addListed(@ts^.parentName, ts^.classParentSpec)
    else if (pt^.kind = tyClass) and (pt^.classHelperFor = nil) and
            (pt^.classRecord = nil) then
      parentClass := pt
    else if not isError(pt) then
    begin
      ms('');
      maT(pt);
      ma(' is not a class or interface type');
      errR(ts^.classParentSpec^.b, specRange(ts^.classParentSpec));
    end;
  end
  else if identValid(ts^.parentName) then
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
      addListed(@ts^.parentName, nil)
    else
    begin
      pc := lookupClass(ts^.parentName);
      if pc <> nil then
        parentClass := pc;
    end;
  end;
  for k := 0 to ts^.interfaceNames.count - 1 do
    if k < ts^.classIntfSpecs.count then
      addListed(PIdent(listGet(ts^.interfaceNames, k)),
                PTypeSpec(listGet(ts^.classIntfSpecs, k)))
    else
      addListed(PIdent(listGet(ts^.interfaceNames, k)), nil);
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
  { nested constants and types are visible to the members after them, and
    so are the ancestors' }
  savedScope := cur;
  savedPrefix := nestPrefix;
  cur := newScope(newNestedScope(cur, parentClass));
  nestPrefix := c^.declName;
  pendingClassMark := pendingClasses.count;
  pendingPointerMark := pendingPointers.count;
  for i := 0 to ts^.memberList.count - 1 do
  begin
    m := PDecl(listGet(ts^.memberList, i));
    if (m^.kind = dkRoutine) and (m^.classInit <> ciNone) then
    begin
      defineClassInit(c, m); { not a member }
      continue;
    end;
    if (m^.kind = dkRoutine) and (m^.memberKey <> nil) then
    begin
      prev := PDecl(mapGet(own, m^.memberKey^.id));
      if prev <> nil then
      begin
        ms('redefinition of generic method ''');
        maN(m^.ident.spelling);
        ma('''');
        errR(m^.ident.loc, identRange(m^.ident));
        ms('previous definition is here');
        note(prev^.loc);
        continue;
      end;
      defineGenericMethod(c, m);
      mapPut(own, m^.memberKey^.id, m);
      listAdd(members, m);
      continue;
    end;
    fwd := PDecl(mapGet(own, m^.ident.name^.id));
    if isForwardClassDecl(fwd) and (m^.kind = dkType) then
    begin
      declareNestedType(m); { its definition }
      mapPut(own, m^.ident.name^.id, m);
      for j := 0 to members.count - 1 do
        if listGet(members, j) = pointer(fwd) then
          listSet(members, j, m);
      continue;
    end;
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
      dkConst:
        declareConst(m);
      dkType:
        begin
          declareNestedType(m);
          addNestedMembers(PDecl(c), m, members, own);
        end;
      dkField:
        begin
          m^.fieldType := resolveType(m^.fieldSpec, nil);
          if containsFile(m^.fieldType) then
          begin
            ms('a field of a class cannot be a file');
            errR(m^.ident.loc, identRange(m^.ident));
          end;
          if m^.race = rqIsolated then
          begin
            ms('a field cannot be isolated (only variables can); its object ' +
               'belongs to whoever has the object');
            errR(m^.ident.loc, identRange(m^.ident));
          end
          else
            checkRaceDecl(m^.race, m^.fieldType, m^.ident, 'field');
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
        declareClassVar(c, m);
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
  { a forward class declared in the class must be defined in it; pointers
    declared in it may point to its later types }
  for i := pendingClassMark to pendingClasses.count - 1 do
  begin
    td := PDecl(listGet(pendingClasses, i));
    ms('class ''');
    maN(td^.ident.spelling);
    ma(''' is declared forward but never defined');
    errR(td^.ident.loc, identRange(td^.ident));
    listInit(impls);
    defineClass(td^.declType, tobjectType, impls,
                tobjectType^.classAllFields, tobjectType^.classVTable,
                curModule);
  end;
  while pendingClasses.count > pendingClassMark do
    listPop(pendingClasses);
  resolvePendingPointersFrom(pendingPointerMark);
  cur := savedScope;
  nestPrefix := savedPrefix;
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

{ A class variable of class, advanced record or helper c (class var):
  static storage, one for c and its descendants. }
procedure declareClassVar(c: PType; v: PDecl);
var
  folded: TConstValue;
begin
  v^.varType := resolveType(v^.varSpec, nil);
  v^.varClass := c;
  if containsFile(v^.varType) then
  begin
    ms('a class variable cannot be a file');
    errR(v^.ident.loc, identRange(v^.ident));
  end;
  if v^.init <> nil then { a typed constant }
  begin
    v^.isUsed := true;
    constInitWhat := 'typed constant value';
    if not isError(v^.varType) and checkConstInit(v^.init, v^.varType, folded) then
      v^.initValue := folded;
  end;
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
    if (m^.kind = dkRoutine) and (m^.classInit <> ciNone) then
    begin
      defineClassInit(c, m); { not a member }
      continue;
    end;
    if (m^.kind = dkConst) or (m^.kind = dkType) then
    begin
      { (declared with the record: see declareAdvancedRecord) }
      prev := PDecl(mapGet(own, m^.ident.name^.id));
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
      mapPut(own, m^.ident.name^.id, m);
      listAdd(members, m);
      if m^.kind = dkType then
        addNestedMembers(PDecl(c), m, members, own);
      continue;
    end;
    if (m^.kind = dkRoutine) and (m^.memberKey <> nil) then
    begin
      prev := PDecl(mapGet(own, m^.memberKey^.id));
      if prev <> nil then
      begin
        ms('redefinition of generic method ''');
        maN(m^.ident.spelling);
        ma('''');
        errR(m^.ident.loc, identRange(m^.ident));
        ms('previous definition is here');
        note(prev^.loc);
        continue;
      end;
      defineGenericMethod(c, m);
      if m^.isClassMethod then
        m^.isStatic := true; { no Self }
      mapPut(own, m^.memberKey^.id, m);
      listAdd(members, m);
      continue;
    end;
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
    if m^.kind = dkVar then { class var }
    begin
      declareClassVar(c, m);
      continue;
    end;
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
  name, savedPrefix: PName;
  memberScope, savedScope: PScope;
  i: longint;
  m: PDecl;
begin
  { its nested constants and types first: its fields may use them }
  name := typeDeclName(td^.ident);
  memberScope := newScope(cur);
  savedScope := cur;
  savedPrefix := nestPrefix;
  cur := memberScope;
  nestPrefix := name;
  for i := 0 to td^.typeSpec^.recMembers.count - 1 do
  begin
    m := PDecl(listGet(td^.typeSpec^.recMembers, i));
    if m^.kind = dkConst then
      declareConst(m)
    else if m^.kind = dkType then
      declareNestedType(m);
  end;
  saved := namedAdvancedRecord;
  namedAdvancedRecord := td^.typeSpec;
  t := resolveType(td^.typeSpec, name);
  namedAdvancedRecord := saved;
  cur := savedScope;
  td^.declType := t;
  declare(td); { its methods' headings may name it }
  cur := memberScope;
  if t^.kind = tyRecord then
    defineRecordMembers(t, td^.typeSpec);
  cur := savedScope;
  nestPrefix := savedPrefix;
end;

{ The scope key of the helper active for t (hidden: not a listed name). }
function helperKey(t: PType): PName;
begin
  helperKey := intern('#helper:' + intToStr(t^.id));
end;

procedure defineHelperMethod(c: PType; r: PDecl);
begin
  r^.ownerClass := c;
  r^.parent := nil;
  r^.level := 1;
  resolveSignature(r);
  if r^.isVirtual or r^.isOverride or r^.isAbstract then
  begin
    ms('the methods of a ' + helperKindName(c) +
       ' cannot be ''virtual'', ''override'' or ''abstract''');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  if r^.isClassMethod then
    r^.isStatic := true { no Self }
  else
    makeSelf(r, c);
end;

{ TH = type helper [(Parent)] for T ... end: a member table (a class type
  that is not a type of values) whose methods' Self is a T, by reference;
  from here on the helper active for T. A record helper is the same, for a
  record type T. }
procedure declareHelper(td: PDecl);
var
  c, t, parentHelper: PType;
  ts: PTypeSpec;
  members, noList: TPtrList;
  own: TIntMap;
  memberScope, savedScope: PScope;
  i: longint;
  m, prev, overloadOf, pd: PDecl;

  procedure empty; { (its methods' bodies are not checked) }
  begin
    listInit(noList);
    defineClass(c, nil, noList, noList, noList, curModule);
    td^.declType := errorType;
  end;

begin
  ts := td^.typeSpec;
  c := newClassType;
  setDeclName(c, typeDeclName(td^.ident));
  c^.classModule := curModule;
  c^.classRecordHelper := ts^.helperIsRecord;
  td^.declType := c;
  declare(td);
  if curRoutine <> nil then
  begin
    ms(helperKindName(c) + ' ''');
    maN(td^.ident.spelling);
    ma(''' must be declared at program or unit level');
    errR(td^.ident.loc, identRange(td^.ident));
    empty;
    exit;
  end;
  t := resolveType(ts^.helperFor, nil);
  if isError(t) then
  begin
    empty;
    exit;
  end;
  if t^.kind in [tyClass, tyClassRef, tyInterface] then
  begin
    ms('type helpers for classes and interfaces are not supported yet');
    errR(ts^.helperFor^.b, specRange(ts^.helperFor));
    empty;
    exit;
  end;
  if ts^.helperIsRecord and (t^.kind <> tyRecord) then
  begin
    ms('a record helper can only extend a record type, not ');
    maT(t);
    ma(' (use a type helper)');
    errR(ts^.helperFor^.b, specRange(ts^.helperFor));
    empty;
    exit;
  end;
  c^.classHelperFor := t;
  parentHelper := nil;
  if identValid(ts^.helperParent) then
  begin
    pd := lookup(ts^.helperParent.name);
    if (pd = nil) or (pd^.kind <> dkType) or (pd^.declType = nil) or
       (pd^.declType^.kind <> tyClass) or (pd^.declType^.classHelperFor = nil) then
    begin
      ms('''');
      maN(ts^.helperParent.spelling);
      { (a record helper can extend a type helper, and the reverse) }
      if c^.classRecordHelper then
        ma(''' is not a helper')
      else
        ma(''' is not a type helper');
      errR(ts^.helperParent.loc, identRange(ts^.helperParent));
    end
    else if pd^.declType^.classHelperFor <> t then
    begin
      ms('''');
      maN(ts^.helperParent.spelling);
      ma(''' is a helper for ');
      maT(pd^.declType^.classHelperFor);
      ma(', not for ');
      maT(t);
      errR(ts^.helperParent.loc, identRange(ts^.helperParent));
    end
    else
      parentHelper := pd^.declType;
  end;
  memberScope := newScope(cur);
  savedScope := cur;
  cur := memberScope;
  listInit(members);
  mapInit(own);
  for i := 0 to ts^.helperMembers.count - 1 do
  begin
    m := PDecl(listGet(ts^.helperMembers, i));
    if (m^.kind = dkRoutine) and (m^.memberKey <> nil) then
    begin
      prev := PDecl(mapGet(own, m^.memberKey^.id));
      if prev <> nil then
      begin
        ms('redefinition of generic method ''');
        maN(m^.ident.spelling);
        ma('''');
        errR(m^.ident.loc, identRange(m^.ident));
        ms('previous definition is here');
        note(prev^.loc);
        continue;
      end;
      defineGenericMethod(c, m);
      if m^.isClassMethod then
        m^.isStatic := true; { no Self }
      mapPut(own, m^.memberKey^.id, m);
      listAdd(members, m);
      continue;
    end;
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
    if m^.kind = dkConst then
    begin
      declareConst(m);
      continue;
    end;
    if m^.kind = dkVar then { class var }
    begin
      declareClassVar(c, m);
      continue;
    end;
    if m^.kind = dkRoutine then
    begin
      defineHelperMethod(c, m);
      if (overloadOf <> nil) and not addOverload(overloadOf, m) then
      begin
        m^.ownerClass := nil; { rejected: not a member }
        listPop(members);
      end;
      continue;
    end;
    if m^.propIsDefault then
    begin
      ms('a ' + helperKindName(c) + ' cannot have a default property');
      errR(m^.loc, identRange(m^.ident));
    end;
    definePropertyAccessors(c, parentHelper, m, own);
  end;
  cur := savedScope;
  listInit(noList);
  defineClass(c, parentHelper, members, noList, noList, curModule);
  scopeSet(cur, helperKey(t), td);
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
  c, rt, convRT, t, declared: PType;
  m, p, x, match, td: PDecl;
  i: longint;
  what, kind: string;
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
  if r^.outerClassNames.count > 0 then { TOuter.TInner.Bar }
  begin
    td := lookupMemberPath(r^.outerClassNames, r^.methodClassName);
    if (td <> nil) and (td^.kind <> dkType) then
      td := nil;
    t := nil;
    if td <> nil then
      t := td^.declType;
    if (t <> nil) and (t^.kind = tyClass) then
      c := t
    else
      c := memberTableOf(t);
    if c = nil then
    begin
      if (td <> nil) and not isError(t) then
      begin
        ms('''');
        maN(r^.methodClassName.spelling);
        ma(''' is not a class or record with members');
        errR(r^.methodClassName.loc, identRange(r^.methodClassName));
      end;
      exit;
    end;
  end
  else
  begin
    td := lookup(r^.methodClassName.name);
    if (td <> nil) and (td^.kind = dkType) then
      c := memberTableOf(td^.declType); { an advanced record }
    if c = nil then
      c := lookupClass(r^.methodClassName);
  end;
  if c = nil then
    exit;
  if c^.classRecord <> nil then
    what := 'record'
  else
    what := 'class';
  if r^.classInit <> ciNone then { class constructor TFoo.Create }
  begin
    if r^.classInit = ciConstructor then
    begin
      m := c^.classCtor;
      kind := 'class constructor';
    end
    else
    begin
      m := c^.classDtor;
      kind := 'class destructor';
    end;
    if (m = nil) or (m^.ident.name <> r^.ident.name) then
    begin
      ms(what + ' ');
      maT(c);
      ma(' has no ' + kind + ' ''');
      maN(r^.ident.spelling);
      ma('''');
      errR(r^.ident.loc, identRange(r^.ident));
      if m <> nil then
        noteDeclaredHere(m);
      exit;
    end;
    if c^.classModule <> curModule then
    begin
      ms('''');
      maQualified(c, m^.ident);
      ma(''' must be implemented where its ' + what + ' is declared');
      errR(r^.ident.loc, identRange(r^.ident));
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
    if r^.params.count > 0 then
    begin
      ms('a ' + kind + ' cannot have parameters');
      errR(r^.ident.loc, identRange(r^.ident));
    end;
    m^.body := r^.body;
    m^.resultName := r^.resultName;
    m^.implDecl := r;
    r^.canonical := m;
    checkRoutineBody(m);
    exit;
  end;
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
      p^.varType := resolveTypeIn(c, p^.varSpec);
    end;
    convRT := nil;
    if isConversionOperator(m) and (r^.resultSpec <> nil) then
      convRT := resolveTypeIn(c, r^.resultSpec);
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
  if (m^.methodKind <> r^.methodKind) or
     ((m^.isFunction or (m^.asyncType <> nil) or (m^.iterType <> nil)) <> r^.isFunction) or
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
      p^.varType := resolveTypeIn(c, p^.varSpec);
    end;
    rt := nil;
    if r^.resultSpec <> nil then
      rt := resolveTypeIn(c, r^.resultSpec);
    declared := m^.resultType;
    if m^.asyncType <> nil then
      declared := m^.asyncType;
    if m^.iterType <> nil then
      declared := m^.iterType;
    if not sameSignature(m, r) or ((rt <> nil) and (rt <> declared)) or
       not sameDefaults(m, r) then
    begin
      ms('heading of ''');
      maQualified(c, m^.ident);
      ma(''' does not match its declaration');
      errR(r^.ident.loc, identRange(r^.ident));
      noteDeclaredHere(m);
    end;
  end;
  if r^.staticDirLoc <> noLoc then
  begin
    if not r^.isClassMethod or (r^.methodKind <> mtMethod) then
    begin
      ms('''static'' is only allowed on class methods');
      err(r^.staticDirLoc);
    end
    else if not m^.isStatic then
    begin
      ms('''');
      maQualified(c, m^.ident);
      ma(''' was not declared ''static''');
      err(r^.staticDirLoc);
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
  m^.resultName := r^.resultName; { (as where its body is) }
  m^.implDecl := r;
  r^.canonical := m;
  if r^.isInline then
    m^.isInline := true;
  checkRoutineBody(m);
end;

{ c's class constructor and destructor have bodies. }
procedure checkClassInitImplemented(c: PType);

  procedure check1(r: PDecl);
  begin
    if (r = nil) or (r^.body <> nil) then
      exit;
    if r^.classInit = ciConstructor then
      ms('class constructor ''')
    else
      ms('class destructor ''');
    maQualified(c, r^.ident);
    ma(''' is declared but never implemented');
    errR(r^.ident.loc, identRange(r^.ident));
  end;

begin
  check1(c^.classCtor);
  check1(c^.classDtor);
end;

{ Every method of the class or advanced record d, and of those it
  declares, has a body. }
procedure checkTypeMethodsImplemented(d: PDecl);
var
  j: longint;
  r: PDecl;
  c: PType;
begin
  if d^.typeSpec^.kind = tsHelper then
  begin
    c := d^.declType;
    if (c = nil) or (c^.kind <> tyClass) then
      exit;
    for j := 0 to c^.classMembers.count - 1 do
    begin
      r := PDecl(listGet(c^.classMembers, j));
      if (r^.kind = dkRoutine) and (r^.body = nil) and
         not ((r^.memberKey <> nil) and (r^.genericDef <> nil)) then
      begin
        ms('method ''');
        maQualified(c, r^.ident);
        ma(''' is declared but never implemented');
        errR(r^.ident.loc, identRange(r^.ident));
      end;
    end;
    exit;
  end;
  c := memberTableOf(d^.declType);
  if c <> nil then { an advanced record's }
  begin
    checkClassInitImplemented(c);
    for j := 0 to c^.classMembers.count - 1 do
    begin
      r := PDecl(listGet(c^.classMembers, j));
      if (r^.kind = dkRoutine) and (r^.body = nil) and
         not ((r^.memberKey <> nil) and (r^.genericDef <> nil)) then
      begin
        ms('method ''');
        maQualified(c, r^.ident);
        ma(''' is declared but never implemented');
        errR(r^.ident.loc, identRange(r^.ident));
      end
      else if r^.kind = dkType then
        checkTypeMethodsImplemented(r);
    end;
    exit;
  end;
  if (d^.typeSpec^.kind <> tsClass) or d^.typeSpec^.isForwardClass then
    exit;
  c := d^.declType;
  if (c = nil) or (c^.kind <> tyClass) then
    exit;
  checkClassInitImplemented(c);
  for j := 0 to d^.typeSpec^.memberList.count - 1 do
  begin
    r := PDecl(listGet(d^.typeSpec^.memberList, j));
    if (r^.kind = dkRoutine) and (r^.ownerClass = c) and not r^.isAbstract and
       (r^.body = nil) and (r^.classInit = ciNone) and
       not ((r^.memberKey <> nil) and (r^.genericDef <> nil)) then
    begin
      ms('method ''');
      maQualified(c, r^.ident);
      ma(''' is declared but never implemented');
      errR(r^.ident.loc, identRange(r^.ident));
    end
    else if r^.kind = dkType then
      checkTypeMethodsImplemented(r);
  end;
end;

{ Every method of the classes declared in b has a body. }
procedure checkMethodsImplemented(b: PBlock);
var
  i: longint;
  d: PDecl;
begin
  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    if d^.kind = dkType then
      checkTypeMethodsImplemented(d);
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
    ((w^.helper <> nil) or (classOfWith(w) <> nil) or
     (memberTableOf(w^.rec^.typ) <> nil));
end;

{ Is d an instance member (a field, a property, a method that is not a
  class method or constructor)? Class methods cannot use them. }
function isInstanceMember(d: PDecl): boolean;
begin
  isInstanceMember := (d^.kind = dkField) or
                      ((d^.kind = dkProperty) and not d^.propIsClass) or
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
    { (private, from another module: a specialization's own methods) }
    if (d^.vis = vsStrictPrivate) or (d^.vis = vsPrivate) then
    begin
      if m^.ownerClass = owner then
        exit;
    end
    else if inheritsFrom(m^.ownerClass, owner) then
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
{ A TGUID typed constant (or initialized variable) from a string constant,
  as in Free Pascal: a GUID in braces, XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
  (hexadecimal digits), or '' (all zeros). folded receives its 16 bytes
  (Data1, Data2 and Data3 little-endian, then Data4). }
function checkGuidInit(init: PNode; var folded: TConstValue): boolean;
const
  groupPos: array[0..4] of longint = (1, 10, 15, 20, 25);
  groupDigits: array[0..4] of longint = (8, 4, 4, 4, 12);
  groupByte: array[0..4] of longint = (0, 4, 6, 8, 10);
  groupLE: array[0..4] of boolean = (true, true, true, false, false);
var
  c: TConstant;
  isStr, isCh, ok: boolean;
  bytes: array[0..15] of char;
  s: PCharArray;
  g, i, n, h: longint;
  v: int64;
  ch: char;
begin
  checkGuidInit := false;
  if not evaluateConstant(init, constInitWhat, c) then
    exit;
  isStr := c.value.kind = ckString;
  isCh := (c.value.kind = ckOrdinal) and (getBase(c.ty)^.kind = tyChar);
  if not isStr and not isCh then
  begin
    ms('cannot initialize ');
    maT(tguidT);
    ma(' with a value of type ');
    maT(c.ty);
    errR(init^.b, nodeRange(init));
    exit;
  end;
  for i := 0 to 15 do
    bytes[i] := #0;
  ok := isStr;
  if isStr and (c.value.strVal^.len > 0) then
  begin
    s := c.value.strVal^.chars;
    ok := (c.value.strVal^.len = 38) and (s^[0] = '{') and (s^[37] = '}') and
          (s^[9] = '-') and (s^[14] = '-') and (s^[19] = '-') and (s^[24] = '-');
    g := 0;
    while ok and (g <= 4) do
    begin
      v := 0;
      for i := groupPos[g] to groupPos[g] + groupDigits[g] - 1 do
      begin
        ch := s^[i];
        if (ch >= '0') and (ch <= '9') then
          h := ord(ch) - ord('0')
        else if (ch >= 'a') and (ch <= 'f') then
          h := ord(ch) - ord('a') + 10
        else if (ch >= 'A') and (ch <= 'F') then
          h := ord(ch) - ord('A') + 10
        else
        begin
          ok := false;
          break;
        end;
        v := (v shl 4) or h;
      end;
      if ok then
      begin
        n := groupDigits[g] div 2;
        for i := 0 to n - 1 do
          if groupLE[g] then
            bytes[groupByte[g] + i] := chr((v shr (8 * i)) and 255)
          else
            bytes[groupByte[g] + n - 1 - i] := chr((v shr (8 * i)) and 255);
      end;
      g := g + 1;
    end;
  end;
  if not ok then
  begin
    ms('improper GUID syntax: expected ' +
       '''{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}'' (hexadecimal digits) or ''''');
    errR(init^.b, nodeRange(init));
    exit;
  end;
  folded := constString(internChars(PCharArray(@bytes), 0, 16));
  checkGuidInit := true;
end;

function checkValueFor(targetTy: PType; value: PNode): PType; forward;

{ A typed constant (or initialized variable) of procedural type rt: nil, a
  routine (@F) declared at program or unit level or a static method, or a
  lambda that uses nothing of the routines around it and has no object. }
function checkRoutineConstInit(init: PNode; rt: PType): boolean;
var
  vt, ignoredT: PType;
  l: PDecl;

  procedure bad(const why: string);
  begin
    ms(constInitWhat + ' must be a constant: ' + why);
    errR(init^.b, nodeRange(init));
  end;

begin
  checkRoutineConstInit := false;
  if init^.kind = nkNil then
  begin
    ignoredT := check(init);
    exit(true);
  end;
  if init^.kind <> nkAddrOf then
  begin
    ms('typed constants of type ');
    maT(rt);
    ma(' can only be initialized with nil, a routine or a lambda');
    errR(init^.b, nodeRange(init));
    exit;
  end;
  vt := checkValueFor(rt, init);
  if isError(vt) then
    exit;
  if not isAssignable(rt, vt) then
  begin
    ms('cannot initialize ');
    maT(rt);
    ma(' with a value of type ');
    maT(vt);
    errR(init^.b, nodeRange(init));
    exit;
  end;
  if init^.addrMethod <> nil then
  begin
    bad('the object of a method pointer is not');
    exit;
  end;
  l := init^.addrLambda;
  if l <> nil then
  begin
    if identValid(l^.lambdaCapture) then
    begin
      ms(constInitWhat + ' must be a constant: this lambda uses ''');
      maN(l^.lambdaCapture.spelling);
      ma(''' of the routine around it');
      errR(init^.b, nodeRange(init));
      exit;
    end;
    if l^.isMethodLambda and (l^.lambdaSelf <> nil) then
    begin
      bad('this lambda''s object is Self');
      exit;
    end;
  end;
  checkRoutineConstInit := true;
end;

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
      { (1, 'a') for a tuple: its elements in turn, each then named }
      if t^.isTupleRecord and not recordForm then
      begin
        if init^.aggElems.count <> t^.allFields.count then
        begin
          ms('initializer for ');
          maT(t);
          ma(' must list ');
          maI(t^.allFields.count);
          ma(' elements, separated by '',''');
          errR(init^.b, nodeRange(init));
          exit;
        end;
        for i := 0 to init^.aggElems.count - 1 do
        begin
          el := PAggElement(listGet(init^.aggElems, i));
          el^.field := PDecl(listGet(t^.allFields, i))^.ident;
          el^.field.loc := el^.value^.b;
        end;
        recordForm := true;
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

  if t = tguidT then { a GUID string }
    exit(checkGuidInit(init, folded));
  if t^.kind = tyRoutine then
    exit(checkRoutineConstInit(init, t));
  if t^.kind = tyDynArray then { only nil }
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
    if not constantInRange(c.value.intVal, c.ty, t) then
    begin
      ms('value ');
      maV(c.ty, c.value.intVal);
      ma(' is out of range for ');
      maT(t);
      errR(init^.b, nodeRange(init));
      exit;
    end;
  end;
  checkWideToChar(t, init);
  folded := c.value;
  if (c.value.kind = ckOrdinal) and (getBase(c.ty)^.kind = tyWideChar) and
     (getBase(t)^.kind = tyChar) then
    folded.intVal := wideToChar(folded.intVal);
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
  if isBitPackedComponent(e) then
    exit;
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

function externalString(e: PNode; const what, ofWhat: string; var s: PName): boolean; forward;

{ x: T; [cvar;] external ['lib'] [name 'sym'];  -- a C variable, at program
  or unit level; the library and symbol: string constants. }
procedure checkExternalVar(v: PDecl);
var
  s: PName;
begin
  v^.isUsed := true;
  if not v^.varExternal then
  begin
    ms('''cvar'' without ''external'' (a variable for C code to use) is not ' +
       'supported yet');
    errR(v^.ident.loc, identRange(v^.ident));
    exit;
  end;
  if curRoutine <> nil then
  begin
    ms('external variable ''');
    maN(v^.ident.spelling);
    ma(''' must be declared at program or unit level');
    errR(v^.ident.loc, identRange(v^.ident));
  end
  else if (v^.absTarget <> nil) or (v^.init <> nil) then
  begin
    ms('external variable ''');
    maN(v^.ident.spelling);
    if v^.absTarget <> nil then
      ma(''' cannot be ''absolute''')
    else
      ma(''' cannot be initialized');
    errR(v^.ident.loc, identRange(v^.ident));
  end;
  if v^.varLibExpr <> nil then
    if externalString(v^.varLibExpr, 'library name', 'variable', s) then
      v^.varLibName := s;
  if v^.varNameExpr <> nil then
    if externalString(v^.varNameExpr, 'symbol name', 'variable', s) then
      v^.varExtName := s;
end;

{ x: T owned: a variable of a routine (or one declared among the
  statements), of a class type. }
{ Can values of type t be shared between tasks? A task, a token, or an
  object of a class derived from TSharedObject (which synchronizes what it
  shares). }
function isThreadSafe(t: PType): boolean;
begin
  if t^.kind in [tyTask, tyToken] then
    exit(true);
  isThreadSafe := (t^.kind = tyClass) and (sharedObjectType <> nil) and
                  inheritsFrom(t, sharedObjectType);
end;

{ Does a value of type t give access to data that others may change (an
  object that is not thread-safe, a pointer, a dynamic array, an
  interface)? }
function sharesMutableData(t: PType): boolean;
begin
  if isThreadSafe(t) then
    exit(false);
  sharesMutableData := t^.kind in [tyClass, tyPointer, tyDynArray, tyInterface];
end;

{ shared T: T thread-safe; isolated T: T a reference (a class, a dynamic
  array or a pointer), which move hands over. }
procedure checkRaceDecl(q: longint; t: PType; const id: TIdent; const what: string);
begin
  if (q = rqNone) or isError(t) then
    exit;
  if (q = rqShared) and not isThreadSafe(t) then
  begin
    ms('shared ' + what + ' ''');
    maN(id.spelling);
    ma(''' must be of a thread-safe type (a class derived from TSharedObject, ' +
       'such as TAtomic<T>, TLocked<T> or TChannel<T>; a task; a TCancelToken), not ');
    maT(t);
    errR(id.loc, identRange(id));
  end;
  if (q = rqIsolated) and not (t^.kind in [tyClass, tyDynArray, tyPointer]) then
  begin
    ms('isolated ' + what + ' ''');
    maN(id.spelling);
    ma(''' must be of a class, dynamic array or pointer type, not ');
    maT(t);
    errR(id.loc, identRange(id));
  end;
end;

procedure checkOwnedDecl(v: PDecl);
var
  ok: boolean;
begin
  if v^.isOwned then
  begin
    ok := false;
    if (curRoutine = nil) and not v^.isInlineVar then
    begin
      ms('''owned'' is only allowed on the local variables of a routine');
      errR(v^.ident.loc, identRange(v^.ident));
    end
    else if not isError(v^.varType) and (v^.varType^.kind <> tyClass) then
    begin
      ms('owned variable ''');
      maN(v^.ident.spelling);
      ma(''' must be of a class type, not ');
      maT(v^.varType);
      errR(v^.ident.loc, identRange(v^.ident));
    end
    else if (v^.absTarget <> nil) or (v^.init <> nil) then
    begin
      ms('owned variable ''');
      maN(v^.ident.spelling);
      if v^.absTarget <> nil then
        ma(''' cannot be ''absolute''')
      else
        ma(''' cannot be initialized');
      errR(v^.ident.loc, identRange(v^.ident));
    end
    else
      ok := not isError(v^.varType);
    v^.isOwned := ok;
    if ok then
      markHasTry; { freed in a finally part around the routine's body }
  end;
  checkRaceDecl(v^.race, v^.varType, v^.ident, 'variable');
end;

procedure declareVar(v: PDecl);
var
  folded: TConstValue;
  unimaged: PType;
begin
  v^.varType := resolveType(v^.varSpec, nil);
  v^.owner := curRoutine;
  checkOwnedDecl(v);
  if v^.varExternal or v^.varCVar then
    checkExternalVar(v);
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
    unimaged := nil;
    if containsAlignedRecord(v^.varType) then
      unimaged := notByteImage(v^.varType);
    if containsBitPacked(v^.varType) then
    begin
      if v^.isInitVar then
        ms('initialized variables')
      else
        ms('typed constants');
      ma(' of bitpacked types are not supported yet');
      errR(v^.init^.b, nodeRange(v^.init));
    end
    else if unimaged <> nil then
    begin
      if v^.isInitVar then
        ms('initialized variables')
      else
        ms('typed constants');
      ma(' of packed records or records under {$packrecords} with values ' +
         'of type ');
      maT(unimaged);
      ma(' are not supported yet');
      errR(v^.init^.b, nodeRange(v^.init));
    end
    else if not isError(v^.varType) and checkConstInit(v^.init, v^.varType, folded) then
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
    if not constantInRange(c.value.intVal, c.ty, t) then
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
  { iterator T: an iterator routine, whose body is a procedure's that
    yields T values }
  if resultTy^.kind = tyIterator then
  begin
    r^.iterType := resultTy;
    r^.isFunction := false;
    if r^.isExternal then
    begin
      ms('an external routine cannot be an iterator');
      errR(r^.ident.loc, identRange(r^.ident));
    end;
    exit;
  end;
  { async T / async: an async routine, whose body is a function's
    returning T or a procedure's }
  if resultTy^.kind = tyTask then
  begin
    r^.asyncType := resultTy;
    if r^.isExternal then
    begin
      ms('an external routine cannot be async');
      errR(r^.ident.loc, identRange(r^.ident));
    end;
    if resultTy^.taskResult = nil then
    begin
      r^.isFunction := false;
      exit;
    end;
    resultTy := resultTy^.taskResult;
    if not isError(resultTy) and not isValidResultType(resultTy) and
       (resultTy^.kind <> tyShortString) and
       not ((resultTy^.kind = tyRecord) and not containsFile(resultTy)) then
    begin
      ms('the result of a task cannot be ');
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
       (compareOut and ((p^.isOut <> q^.isOut) or
                        (p^.isConstRef <> q^.isConstRef))) then
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
             (t^.kind = tyToken) or { (the runtime's token, not counted by the call) }
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
function externalString(e: PNode; const what, ofWhat: string; var s: PName): boolean;
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
  ms('the ' + what + ' of an external ' + ofWhat + ' must be a string constant');
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
  if (r^.parent <> nil) and not allowNested and not r^.isLambda then
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
    if externalString(r^.libraryExpr, 'library name', 'routine', s) then
      r^.libraryName := s;
  if r^.nameExpr <> nil then
    if externalString(r^.nameExpr, 'symbol name', 'routine', s) then
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

{ The scope key of a generic routine: its name and type parameter count
  (f<1>), so it does not clash with routines named f. }
function genericKey(name: PName; n: longint): PName;
begin
  genericKey := intern(nameStr(name) + '<' + intToStr(n) + '>');
end;

{ generic function F<T>...: declared (under its key), not checked. }
procedure declareGenericMethodBody(r: PDecl); forward;

procedure declareGeneric(r: PDecl);
var
  i, j: longint;
  pi, pj: PIdent;
  key: PName;
  prev: PDecl;
begin
  if identValid(r^.methodClassName) and (curRoutine = nil) then
  begin
    declareGenericMethodBody(r);
    exit;
  end;
  if curRoutine <> nil then
  begin
    ms('generic routines must be declared at program or unit level');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  for i := 0 to r^.typeParams.count - 1 do
    for j := 0 to i - 1 do
    begin
      pi := PIdent(listGet(r^.typeParams, i));
      pj := PIdent(listGet(r^.typeParams, j));
      if pi^.name = pj^.name then
      begin
        ms('duplicate type parameter ''');
        maN(pi^.spelling);
        ma('''');
        errR(pi^.loc, identRange(pi^));
      end;
    end;
  key := genericKey(r^.ident.name, r^.typeParams.count);
  prev := scopeLookupOwn(cur, key);
  if (prev = nil) and (curUnitInterface <> nil) then
    prev := scopeLookupOwn(curUnitInterface, key);
  if (prev <> nil) and (prev^.kind <> dkRoutine) then
    prev := nil;
  r^.genericScope := cur;
  r^.isUsed := true;
  if (prev <> nil) and (prev^.body = nil) and (prev^.genericDef = nil) and
     (r^.body <> nil) then
  begin
    { the definition of a forward or interface heading }
    prev^.genericDef := r;
    prev^.genericScope := cur;
    exit;
  end;
  if prev <> nil then
  begin
    ms('redefinition of generic ' + kindName(r) + ' ''');
    maN(r^.ident.spelling);
    ma('''');
    errR(r^.ident.loc, identRange(r^.ident));
    ms('previous definition is here');
    note(prev^.loc);
    exit;
  end;
  scopeInsert(cur, key, r);
end;

{ The generic declaration with the body: r, or the definition of a forward
  or interface heading. }
function genericDefinition(r: PDecl): PDecl;
begin
  if r^.genericDef <> nil then
    genericDefinition := r^.genericDef
  else
    genericDefinition := r;
end;

{ A forward or interface heading without its definition (yet). }
function isUndefinedHeading(r: PDecl): boolean;
begin
  isUndefinedHeading := (r^.body = nil) and
                        not (r^.isGeneric and (genericDefinition(r)^.body <> nil));
end;

{ specialize name<args>: the routine for those type arguments, made from
  the generic's declaration on first use (its heading resolved now, its
  body checked later); nil after a diagnostic. }
function instantiateGeneric(const name: TIdent; g: PDecl; const types: TPtrList): PDecl; forward;
function instantiateMethod(const name: TIdent; g: PDecl; const types: TPtrList): PDecl; forward;

{ generic function TFoo.M<T>(...) begin ... end: the definition of the
  generic method M<N> of TFoo; its heading must match the declaration's in
  its shape (type parameters, parameters' kinds and number, function or
  procedure), the types being checked in each specialization. }
procedure declareGenericMethodBody(r: PDecl);
var
  c, t: PType;
  td, m, a, b: PDecl;
  i: longint;
  same: boolean;
begin
  c := nil;
  if r^.outerClassNames.count > 0 then
  begin
    td := lookupMemberPath(r^.outerClassNames, r^.methodClassName);
    if (td <> nil) and (td^.kind = dkType) then
    begin
      t := td^.declType;
      if (t <> nil) and (t^.kind = tyClass) then
        c := t;
      if (c = nil) and (t <> nil) then
        c := memberTableOf(t); { an advanced record }
    end;
  end
  else
  begin
    td := lookup(r^.methodClassName.name);
    if (td <> nil) and (td^.kind = dkType) and (td^.declType <> nil) then
      c := memberTableOf(td^.declType); { an advanced record }
    if c = nil then
      c := lookupClass(r^.methodClassName);
  end;
  if c = nil then
    exit;
  m := findOwnMember(c, genericKey(r^.ident.name, r^.typeParams.count));
  if (m <> nil) and (m^.kind <> dkRoutine) then
    m := nil;
  if m = nil then
  begin
    if c^.classRecord <> nil then
      ms('record ''')
    else if c^.classHelperFor <> nil then
      ms(helperKindName(c) + ' ''')
    else
      ms('class ''');
    bufAddTypeName(msg, c);
    ma(''' has no generic method ''');
    maN(r^.ident.spelling);
    ma(''' with ' + intToStr(r^.typeParams.count) + ' type parameter');
    if r^.typeParams.count <> 1 then
      ma('s');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if c^.classModule <> curModule then
  begin
    ms('method ''');
    maQualified(c, m^.ident);
    ma(''' must be implemented where its class is declared');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  if m^.genericDef <> nil then
  begin
    ms('redefinition of ''');
    maQualified(c, m^.ident);
    ma('''');
    errR(r^.ident.loc, identRange(r^.ident));
    exit;
  end;
  same := (m^.isFunction = r^.isFunction) and (m^.isClassMethod = r^.isClassMethod) and
          (m^.params.count = r^.params.count);
  i := 0;
  while same and (i < r^.typeParams.count) do
  begin
    same := PIdent(listGet(r^.typeParams, i))^.name = PIdent(listGet(m^.typeParams, i))^.name;
    i := i + 1;
  end;
  i := 0;
  while same and (i < r^.params.count) do
  begin
    a := PDecl(listGet(m^.params, i));
    b := PDecl(listGet(r^.params, i));
    same := (a^.varKind = b^.varKind) and (a^.isOut = b^.isOut) and
            (a^.isConstRef = b^.isConstRef) and (a^.ident.name = b^.ident.name);
    i := i + 1;
  end;
  if not same then
  begin
    ms('heading of ''');
    maQualified(c, m^.ident);
    ma(''' does not match its declaration');
    errR(r^.ident.loc, identRange(r^.ident));
    noteDeclaredHere(m);
    exit;
  end;
  r^.genericScope := cur;
  r^.ownerClass := c;
  m^.genericDef := r;
  m^.genericScope := cur;
  r^.isUsed := true;
end;

{ The generic methods named name of class c or its ancestors (the nearest
  of each number of type parameters). }
procedure genericMethodsNamed(c: PType; name: PName; var found: TPtrList);
var
  n: longint;
  owner: PType;
  d: PDecl;
begin
  listInit(found);
  for n := 1 to 16 do
  begin
    owner := nil;
    d := findMember(c, genericKey(name, n), owner);
    if (d <> nil) and (d^.kind = dkRoutine) then
      listAdd(found, d);
  end;
end;

function specializeGenericW(const name: TIdent; const args: TPtrList; var w: PWithContext): PDecl;
var
  types: TPtrList;
  t: PType;
  g: PDecl;
  i, k: longint;
  otherGeneric: boolean;
begin
  specializeGenericW := nil;
  listInit(types);
  for i := 0 to args.count - 1 do
  begin
    t := resolveType(PTypeSpec(listGet(args, i)), nil);
    if isError(t) then
      exit;
    listAdd(types, t);
  end;
  g := scopeLookup(cur, genericKey(name.name, args.count), w);
  if (g <> nil) and (g^.kind <> dkRoutine) then
    g := nil;
  if (g <> nil) and (g^.memberKey <> nil) then { specialize M<T>(args) in a method }
    exit(instantiateMethod(name, g, types));
  otherGeneric := false; { f with another number of type parameters }
  k := 1;
  while (k <= 16) and (g = nil) do
  begin
    if lookup(genericKey(name.name, k)) <> nil then
      otherGeneric := true;
    k := k + 1;
  end;
  if g = nil then
  begin
    if (lookup(name.name) <> nil) or otherGeneric then
    begin
      ms('''');
      maN(name.spelling);
      ma(''' is not a generic routine with ' + intToStr(args.count) +
         ' type parameter');
      if args.count <> 1 then
        ma('s');
      errR(name.loc, identRange(name));
    end
    else
      diagUndeclared(name);
    exit;
  end;
  specializeGenericW := instantiateGeneric(name, g, types);
end;

function specializeGeneric(const name: TIdent; const args: TPtrList): PDecl;
var
  w: PWithContext;
begin
  w := nil;
  specializeGeneric := specializeGenericW(name, args, w);
end;

{ The specialization of the generic routine g for the type arguments types
  (see specializeGeneric). }
function instantiateGeneric(const name: TIdent; g: PDecl; const types: TPtrList): PDecl;
var
  def, r, td: PDecl;
  i, k, errors: longint;
  inst: PInstance;
  same: boolean;
  nb, sb, tb: TBuf;
  sc, savedScope: PScope;
  savedRoutine: PDecl;
  savedPrefix: PName;
  p: PIdent;
  id: TIdent;
begin
  instantiateGeneric := nil;
  g^.isUsed := true;
  def := genericDefinition(g);
  if def^.body = nil then
  begin
    ms('generic ' + kindName(g) + ' ''');
    maN(name.spelling);
    ma(''' is used before its definition');
    errR(name.loc, identRange(name));
    ms('declared here');
    note(g^.loc);
    exit;
  end;
  for i := 0 to instances.count - 1 do
  begin
    inst := PInstance(listGet(instances, i));
    if (inst^.g = g) and (inst^.types.count = types.count) then
    begin
      same := true;
      for k := 0 to types.count - 1 do
        if listGet(inst^.types, k) <> listGet(types, k) then
          same := false;
      if same then
        exit(inst^.r);
    end;
  end;
  r := reparseGeneric(def);
  if r = nil then
    exit;
  { F<integer, string> }
  bufInit(nb);
  bufInit(sb);
  bufAddName(nb, def^.ident.name);
  bufAddChar(nb, '<');
  bufAddName(sb, def^.ident.spelling);
  bufAddChar(sb, '<');
  for k := 0 to types.count - 1 do
  begin
    if k > 0 then
    begin
      bufAddChar(nb, ',');
      bufAdd(sb, ', ');
    end;
    bufInit(tb);
    bufAddTypeName(tb, PType(listGet(types, k)));
    for i := 0 to tb.len - 1 do
      bufAddChar(nb, lowerChar(bufCharAt(tb, i)));
    bufAddChars(sb, tb.data, 0, tb.len);
    bufFree(tb);
  end;
  bufAddChar(nb, '>');
  bufAddChar(sb, '>');
  id.name := internChars(nb.data, 0, nb.len);
  id.spelling := internChars(sb.data, 0, sb.len);
  id.loc := def^.ident.loc;
  bufFree(nb);
  bufFree(sb);
  r^.ident := id;
  inst := PInstance(arenaAlloc(sizeof(TInstance)));
  inst^.g := g;
  inst^.types := types;
  inst^.r := r;
  inst^.use := name.loc;
  listAdd(instances, inst);
  { its scope: the generic's, where the type parameters name the types }
  sc := newScope(PScope(def^.genericScope));
  for k := 0 to types.count - 1 do
  begin
    p := PIdent(listGet(def^.typeParams, k));
    td := newDecl(dkType, p^, p^.loc);
    td^.declType := PType(listGet(types, k));
    td^.isUsed := true;
    scopeInsert(sc, p^.name, td);
  end;
  inst^.scope := sc;
  savedScope := cur;
  savedRoutine := curRoutine;
  savedPrefix := nestPrefix;
  cur := sc;
  curRoutine := nil;
  nestPrefix := nil;
  errors := diagNumErrors;
  r^.parent := nil;
  r^.level := 1;
  resolveSignature(r);
  if diagNumErrors > errors then
  begin
    ms('in the specialization ''');
    maN(id.spelling);
    ma(''' requested here');
    note(name.loc);
  end;
  cur := savedScope;
  curRoutine := savedRoutine;
  nestPrefix := savedPrefix;
  r^.isUsed := true;
  inst^.parent := checkingInstance;
  listAdd(pendingInstances, inst);
  listAdd(moduleInstances, r);
  instantiateGeneric := r;
end;

{ The specialization of generic method g (of its class) for the type
  arguments types: its implementation parsed again, named M<types>, a
  method of the class (not a member) with g's directives; its heading
  resolved now, its body checked later. Nil after a diagnostic. }
function instantiateMethod(const name: TIdent; g: PDecl; const types: TPtrList): PDecl;
var
  def, r, td: PDecl;
  c: PType;
  i, k, errors: longint;
  inst: PInstance;
  same: boolean;
  nb, sb, tb: TBuf;
  sc, savedScope: PScope;
  savedRoutine: PDecl;
  savedPrefix: PName;
  p: PIdent;
  id: TIdent;
begin
  instantiateMethod := nil;
  g^.isUsed := true;
  c := g^.ownerClass;
  def := genericDefinition(g);
  if def = g then
  begin
    ms('generic method ''');
    maQualified(c, g^.ident);
    ma(''' is used before its implementation');
    errR(name.loc, identRange(name));
    noteDeclaredHere(g);
    exit;
  end;
  for i := 0 to instances.count - 1 do
  begin
    inst := PInstance(listGet(instances, i));
    if (inst^.g = g) and (inst^.types.count = types.count) then
    begin
      same := true;
      for k := 0 to types.count - 1 do
        if listGet(inst^.types, k) <> listGet(types, k) then
          same := false;
      if same then
        exit(inst^.r);
    end;
  end;
  r := reparseGeneric(def);
  if r = nil then
    exit;
  bufInit(nb);
  bufInit(sb);
  bufAddName(nb, g^.ident.name);
  bufAddChar(nb, '<');
  bufAddName(sb, g^.ident.spelling);
  bufAddChar(sb, '<');
  for k := 0 to types.count - 1 do
  begin
    if k > 0 then
    begin
      bufAddChar(nb, ',');
      bufAdd(sb, ', ');
    end;
    bufInit(tb);
    bufAddTypeName(tb, PType(listGet(types, k)));
    for i := 0 to tb.len - 1 do
      bufAddChar(nb, lowerChar(bufCharAt(tb, i)));
    bufAddChars(sb, tb.data, 0, tb.len);
    bufFree(tb);
  end;
  bufAddChar(nb, '>');
  bufAddChar(sb, '>');
  id.name := internChars(nb.data, 0, nb.len);
  id.spelling := internChars(sb.data, 0, sb.len);
  id.loc := def^.ident.loc;
  bufFree(nb);
  bufFree(sb);
  r^.ident := id;
  inst := PInstance(arenaAlloc(sizeof(TInstance)));
  inst^.g := g;
  inst^.types := types;
  inst^.r := r;
  inst^.use := name.loc;
  listAdd(instances, inst);
  { its scope: the implementation's, where the type parameters name the
    types }
  sc := newScope(PScope(def^.genericScope));
  for k := 0 to types.count - 1 do
  begin
    p := PIdent(listGet(def^.typeParams, k));
    td := newDecl(dkType, p^, p^.loc);
    td^.declType := PType(listGet(types, k));
    td^.isUsed := true;
    scopeInsert(sc, p^.name, td);
  end;
  inst^.scope := sc;
  savedScope := cur;
  savedRoutine := curRoutine;
  savedPrefix := nestPrefix;
  cur := sc;
  curRoutine := nil;
  nestPrefix := nil;
  errors := diagNumErrors;
  r^.parent := nil;
  r^.level := 1;
  r^.ownerClass := c;
  r^.methodKind := mtMethod;
  if g^.isClassMethod then
    r^.isClassMethod := true;
  if g^.isStatic then
    r^.isStatic := true;
  r^.vis := g^.vis;
  resolveSignature(r);
  if not r^.isStatic then
    makeSelf(r, c);
  if diagNumErrors > errors then
  begin
    ms('in the specialization ''');
    maQualified(c, id);
    ma(''' requested here');
    note(name.loc);
  end;
  cur := savedScope;
  curRoutine := savedRoutine;
  nestPrefix := savedPrefix;
  r^.isUsed := true;
  inst^.parent := checkingInstance;
  listAdd(pendingInstances, inst);
  listAdd(moduleInstances, r);
  instantiateMethod := r;
end;

{ Checks the bodies of the specializations made so far (and those they
  make), then adds the module's to its block. }
type
  { a specialization of generic type g for types (of PType): the type
    declaration td, declared in scope }
  PTypeInstance = ^TTypeInstance;
  TTypeInstance = record
    g, td: PDecl;
    types: TPtrList;
    scope: PScope;
  end;

var
  typeInstances: TPtrList; { of PTypeInstance }
  typeInstancesInit: boolean;
  { while a generic type's specialization is declared: its methods'
    implementations (of PDecl), whose bodies are checked later
    (checkInstances) }
  deferredBodies: PPtrList;

{ Is name the name of a generic (type or routine), with any number of type
  parameters? }
function isGenericName(name: PName): boolean;
var
  n: longint;
begin
  isGenericName := false;
  for n := 1 to 16 do
    if lookup(genericKey(name, n)) <> nil then
      exit(true);
end;

{ generic TList<T> = ...: declared (under its key), not checked. }
{ The scope key of the last generic type helper declared (see
  prevGenericHelper). }
function genericHelpersKey: PName;
begin
  genericHelpersKey := intern('#generichelpers');
end;

{ Is ts, the type generic helper td extends, a pattern that gives all its
  type parameters (and not a bare parameter: a helper for every type)? }
function genericHelperPatternOK(td: PDecl; ts: PTypeSpec): boolean;
var
  seen: array of boolean;
  k: longint;

  function walk(x: PTypeSpec): boolean;
  var
    m: longint;
  begin
    walk := false;
    if x^.kind = tsNamed then
    begin
      if x^.tquals.count = 0 then
        for m := 0 to td^.tgParams.count - 1 do
          if PIdent(listGet(td^.tgParams, m))^.name = x^.tname.name then
            seen[m] := true;
      exit(true);
    end;
    if x^.kind = tsDynArray then
      exit(walk(x^.dynSpec));
    if (x^.kind = tsAsync) and x^.asyncIterator then
      exit(walk(x^.asyncInner));
    if x^.kind = tsSpecialize then
    begin
      for m := 0 to x^.spArgs.count - 1 do
        if not walk(PTypeSpec(listGet(x^.spArgs, m))) then
          exit;
      exit(true);
    end;
  end;

begin
  genericHelperPatternOK := false;
  SetLength(seen, td^.tgParams.count);
  for k := 0 to td^.tgParams.count - 1 do
    seen[k] := false;
  if (ts^.kind = tsNamed) and (ts^.tquals.count = 0) then
    for k := 0 to td^.tgParams.count - 1 do
      if PIdent(listGet(td^.tgParams, k))^.name = ts^.tname.name then
        exit; { (a helper for every type) }
  if not walk(ts) then
    exit;
  for k := 0 to td^.tgParams.count - 1 do
    if not seen[k] then
      exit;
  genericHelperPatternOK := true;
end;

procedure declareGenericType(td: PDecl);
var
  i, j: longint;
  pi, pj: PIdent;
  key: PName;
  prev: PDecl;
begin
  if (td^.typeSpec^.kind = tsHelper) and (curRoutine = nil) then
  begin { a generic helper: also in the chain }
    if not genericHelperPatternOK(td, td^.typeSpec^.helperFor) then
    begin
      ms('a generic helper must extend a type built from all its type ' +
         'parameters, such as ''array of T'' or ''specialize TGen<T>''');
      errR(td^.typeSpec^.helperFor^.b, specRange(td^.typeSpec^.helperFor));
    end
    else
    begin
      td^.prevGenericHelper := lookup(genericHelpersKey);
      scopeSet(cur, genericHelpersKey, td);
    end;
  end;
  td^.declType := errorType;
  if curRoutine <> nil then
  begin
    ms('generic types must be declared at program or unit level');
    errR(td^.ident.loc, identRange(td^.ident));
    exit;
  end;
  for i := 0 to td^.tgParams.count - 1 do
    for j := 0 to i - 1 do
    begin
      pi := PIdent(listGet(td^.tgParams, i));
      pj := PIdent(listGet(td^.tgParams, j));
      if pi^.name = pj^.name then
      begin
        ms('duplicate type parameter ''');
        maN(pi^.spelling);
        ma('''');
        errR(pi^.loc, identRange(pi^));
      end;
    end;
  key := genericKey(td^.ident.name, td^.tgParams.count);
  prev := scopeLookupOwn(cur, key);
  if prev <> nil then
  begin
    ms('redefinition of generic ''');
    maN(td^.ident.spelling);
    ma('''');
    errR(td^.ident.loc, identRange(td^.ident));
    ms('previous definition is here');
    note(prev^.loc);
    exit;
  end;
  td^.tgScope := cur;
  td^.isUsed := true;
  scopeInsert(cur, key, td);
end;

procedure declareRoutine(r: PDecl); forward;

{ specialize name<args>, a type: the declaration of the type made of the
  generic type name for those type arguments, parsed again from the
  generic's tokens on first use and declared like any type (named
  Name<integer>), in the generic's scope plus the type parameters as type
  aliases; its methods' implementations are parsed again too, and their
  bodies checked later. Nil after a diagnostic. }
function specializeTypeFor(const name: TIdent; g: PDecl; const types: TPtrList): PDecl; forward;

function specializeType(const name: TIdent; const args: TPtrList): PDecl;
var
  types: TPtrList;
  t: PType;
  g: PDecl;
  i: longint;
begin
  specializeType := nil;
  if not typeInstancesInit then
  begin
    listInit(typeInstances);
    typeInstancesInit := true;
  end;
  listInit(types);
  for i := 0 to args.count - 1 do
  begin
    t := resolveType(PTypeSpec(listGet(args, i)), nil);
    if isError(t) then
      exit;
    listAdd(types, t);
  end;
  g := lookup(genericKey(name.name, args.count));
  if g <> nil then
    if (g^.kind <> dkType) or not g^.tgGeneric then
      g := nil;
  if g = nil then
  begin
    ms('''');
    maN(name.spelling);
    ma(''' is not a generic type with ' + intToStr(args.count) +
       ' type parameter');
    if args.count <> 1 then
      ma('s');
    errR(name.loc, identRange(name));
    exit;
  end;
  specializeType := specializeTypeFor(name, g, types);
end;

{ The specialization of generic type g for types (asked for at name), made
  on first use (see specializeType). }
function specializeTypeFor(const name: TIdent; g: PDecl; const types: TPtrList): PDecl;
var
  methods, bodies: TPtrList;
  inst, td, m: PDecl;
  i, k, errors, pointerMark: longint;
  ti: PTypeInstance;
  same: boolean;
  sb, tb: TBuf;
  sc, savedScope: PScope;
  savedRoutine: PDecl;
  savedPrefix: PName;
  savedDeferred: PPtrList;
  p: PIdent;
  id: TIdent;
  pending: PInstance;
begin
  specializeTypeFor := nil;
  if not typeInstancesInit then
  begin
    listInit(typeInstances);
    typeInstancesInit := true;
  end;
  g^.isUsed := true;
  for i := 0 to typeInstances.count - 1 do
  begin
    ti := PTypeInstance(listGet(typeInstances, i));
    if (ti^.g = g) and (ti^.types.count = types.count) then
    begin
      same := true;
      for k := 0 to types.count - 1 do
        if listGet(ti^.types, k) <> listGet(types, k) then
          same := false;
      if same then
        exit(ti^.td);
    end;
  end;
  listInit(methods);
  inst := reparseGenericType(g, methods);
  if inst = nil then
    exit;
  { TList<integer, string>; it is still found as TList, in its scope }
  bufInit(sb);
  bufAddName(sb, g^.ident.spelling);
  bufAddChar(sb, '<');
  for k := 0 to types.count - 1 do
  begin
    if k > 0 then
      bufAdd(sb, ', ');
    bufInit(tb);
    bufAddTypeName(tb, PType(listGet(types, k)));
    bufAddChars(sb, tb.data, 0, tb.len);
    bufFree(tb);
  end;
  bufAddChar(sb, '>');
  id.name := g^.ident.name;
  id.spelling := internChars(sb.data, 0, sb.len);
  id.loc := g^.ident.loc;
  bufFree(sb);
  inst^.ident := id;
  ti := PTypeInstance(arenaAlloc(sizeof(TTypeInstance)));
  ti^.g := g;
  ti^.td := inst;
  ti^.types := types;
  listAdd(typeInstances, ti);
  sc := newScope(PScope(g^.tgScope));
  for k := 0 to types.count - 1 do
  begin
    p := PIdent(listGet(g^.tgParams, k));
    td := newDecl(dkType, p^, p^.loc);
    td^.declType := PType(listGet(types, k));
    td^.isUsed := true;
    scopeInsert(sc, p^.name, td);
  end;
  ti^.scope := sc;
  savedScope := cur;
  savedRoutine := curRoutine;
  savedPrefix := nestPrefix;
  savedDeferred := deferredBodies;
  listInit(bodies);
  cur := sc;
  curRoutine := nil;
  nestPrefix := nil;
  deferredBodies := @bodies;
  errors := diagNumErrors;
  pointerMark := pendingPointers.count;
  declareType(inst);
  resolvePendingPointersFrom(pointerMark);
  for i := 0 to methods.count - 1 do
  begin
    m := PDecl(listGet(methods, i));
    if m^.kind = dkRoutine then
      declareRoutine(m);
  end;
  checkTypeMethodsImplemented(inst);
  if diagNumErrors > errors then
  begin
    ms('in the specialization ''');
    maN(id.spelling);
    ma(''' requested here');
    note(name.loc);
  end;
  cur := savedScope;
  curRoutine := savedRoutine;
  nestPrefix := savedPrefix;
  deferredBodies := savedDeferred;
  inst^.isUsed := true;
  for i := 0 to bodies.count - 1 do
  begin
    pending := PInstance(arenaAlloc(sizeof(TInstance)));
    pending^.g := g;
    pending^.r := PDecl(listGet(bodies, i));
    pending^.types := types;
    pending^.scope := sc;
    pending^.use := name.loc;
    pending^.parent := checkingInstance;
    listAdd(pendingInstances, pending);
  end;
  { the type and its methods' implementations are the module's }
  listAdd(moduleInstances, inst);
  for i := 0 to methods.count - 1 do
    listAdd(moduleInstances, listGet(methods, i));
  specializeTypeFor := inst;
end;

procedure checkInstances(b: PBlock);
var
  k, q, errors, savedChecking: longint;
  inst, qi: PInstance;
  savedScope: PScope;
begin
  k := 0;
  while (k < pendingInstances.count) and not diagShouldStop do
  begin
    inst := PInstance(listGet(pendingInstances, k));
    savedScope := cur;
    cur := inst^.scope;
    errors := diagNumErrors;
    savedChecking := checkingInstance;
    checkingInstance := k;
    checkRoutineBody(inst^.r);
    checkingInstance := savedChecking;
    { (the specializations that led here, innermost first) }
    q := k;
    while (q >= 0) and (diagNumErrors > errors) do
    begin
      qi := PInstance(listGet(pendingInstances, q));
      ms('in the specialization ''');
      if qi^.r^.ownerClass <> nil then
        maQualified(qi^.r^.ownerClass, qi^.r^.ident)
      else
        maN(qi^.r^.ident.spelling);
      ma(''' requested here');
      note(qi^.use);
      q := qi^.parent;
    end;
    cur := savedScope;
    k := k + 1;
  end;
  listClear(pendingInstances);
  for k := 0 to moduleInstances.count - 1 do
    listAdd(b^.decls, listGet(moduleInstances, k));
  listClear(moduleInstances);
end;

procedure declareRoutine(r: PDecl);
var
  fwd, p, x, match: PDecl;
  i: longint;
  rt, declared: PType;
  inInterface: boolean;
begin
  if r^.isLambda then { completed where it is used (completeLambda) }
    exit;
  if r^.isSpawnBlock then { completed where it is spawned (visitSpawn) }
    exit;
  if r^.isTemplateMember then { of a generic type: see specializeType }
  begin
    { its specializations see what is declared up to here (the unit's
      implementation, for a generic type of its interface) }
    fwd := lookup(genericKey(r^.methodClassName.name, r^.typeParams.count));
    if fwd <> nil then
      if (fwd^.kind = dkType) and fwd^.tgGeneric and (curRoutine = nil) and
         (PScope(fwd^.tgScope) <> cur) then
      begin
        fwd^.tgScope := cur;
        if typeInstancesInit then
          for i := 0 to typeInstances.count - 1 do
            if PTypeInstance(listGet(typeInstances, i))^.g = fwd then
              PTypeInstance(listGet(typeInstances, i))^.scope^.parent := cur;
      end;
    exit;
  end;
  r^.parent := curRoutine;
  if curRoutine <> nil then
    r^.level := curRoutine^.level + 1
  else
    r^.level := 1;
  if r^.isGeneric then
  begin
    declareGeneric(r);
    exit;
  end;
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
    if (fwd^.isFunction or (fwd^.asyncType <> nil) or (fwd^.iterType <> nil)) <>
       r^.isFunction then
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
      declared := fwd^.resultType;
      if fwd^.asyncType <> nil then
        declared := fwd^.asyncType;
      if fwd^.iterType <> nil then
        declared := fwd^.iterType;
      if not sameSignature(fwd, r) or ((rt <> nil) and (rt <> declared)) or
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
    fwd^.resultName := r^.resultName; { (as where its body is) }
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

{ Breaks ties between costs: widening an integer to 64 bits of the other
  signedness (byte to int64 rather than qword, longint to qword rather
  than int64), as Free Pascal prefers the same signedness. }
function signPenalty(p, a: PType): longint;
var
  pb, ab: PType;
begin
  signPenalty := 0;
  pb := getBase(p);
  ab := getBase(a);
  if (pb^.kind = tyInteger) and (ab^.kind = tyInteger) and (pb^.bits = 64) and
     (ab^.bits < 64) and (pb^.isSigned <> ab^.isSigned) then
    signPenalty := 1;
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
  if a^.kind = tyArray then { (not a bitpacked layout) }
    exit((a^.elementType = oa^.openElement) and not hasBitElements(a));
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

{ e is a lambda not completed yet: its routine, else nil. }
function pendingLambda(e: PNode): PDecl;
begin
  pendingLambda := nil;
  if (e^.kind = nkAddrOf) and (e^.addrLambda <> nil) then
    if not e^.addrLambda^.lambdaDone then
      pendingLambda := e^.addrLambda;
  if (e^.kind = nkCall) and (e^.copyLambda <> nil) then { one that copies }
    if not e^.copyLambda^.lambdaDone then
      pendingLambda := e^.copyLambda;
end;

{ The types written in lambda l (of its parameters and result), resolved
  once while overloads are chosen (kept in varType and resultType until
  completeLambda sets them). False after a diagnostic. }
function resolveLambdaSpecs(l: PDecl): boolean;
var
  i: longint;
  p: PDecl;
  t: PType;
begin
  resolveLambdaSpecs := false;
  for i := 0 to l^.params.count - 1 do
  begin
    p := PDecl(listGet(l^.params, i));
    if (p^.varSpec <> nil) and (p^.varType = nil) then
    begin
      t := resolveType(p^.varSpec, nil);
      if isError(t) then
        exit;
      p^.varType := t;
    end;
  end;
  if l^.isFunction and (l^.resultSpec <> nil) and (l^.resultType = nil) then
  begin
    t := resolveType(l^.resultSpec, nil);
    if isError(t) then
      exit;
    l^.resultType := t;
  end;
  resolveLambdaSpecs := true;
end;

{ The cost of passing lambda l for a parameter of type pt, by its shape
  (-1: it does not fit): a procedural type of the same kind (function or
  procedure), with as many parameters, of the types and kinds l writes,
  and the result type it writes. A parameter written with a type but
  without a kind takes the kind wanted, but fits a value parameter best
  (cost 1 for each other); one without either fits any parameter alike
  (so overloads that differ there are ambiguous). }
function lambdaCost(l: PDecl; pt: PType): longint;
var
  i, cost: longint;
  p, wp: PDecl;
  wantsValue: boolean;
begin
  lambdaCost := -1;
  if pt^.kind <> tyRoutine then
    exit;
  if ((pt^.routineResult <> nil) <> l^.isFunction) or
     (pt^.routineParams.count <> l^.params.count) then
    exit;
  if l^.isCopied and not pt^.routineReference then { (copies: a closure) }
    exit;
  cost := 0;
  for i := 0 to pt^.routineParams.count - 1 do
  begin
    p := PDecl(listGet(l^.params, i));
    wp := PDecl(listGet(pt^.routineParams, i));
    if p^.varSpec <> nil then
      if p^.varType <> wp^.varType then
        exit;
    wantsValue := (wp^.varKind = vkValueParam) and not wp^.isOut and not wp^.isConstRef;
    if p^.varKind = vkValueParam then
    begin
      if not wantsValue and (p^.varSpec <> nil) then
        cost := cost + 1;
    end
    else if (p^.varKind <> wp^.varKind) or (p^.isOut <> wp^.isOut) or
            (p^.isConstRef <> wp^.isConstRef) then
      exit;
  end;
  if l^.isFunction and (l^.resultSpec <> nil) then
    if l^.resultType <> pt^.routineResult then
      exit;
  lambdaCost := cost;
end;

{ A lambda's shape for diagnostics: lambda function(integer; ?): ? (? for
  types left out). }
procedure maLambdaShape(l: PDecl);
var
  i: longint;
  p: PDecl;
begin
  if l^.isFunction then
    ma('lambda function(')
  else
    ma('lambda procedure(');
  for i := 0 to l^.params.count - 1 do
  begin
    p := PDecl(listGet(l^.params, i));
    if i > 0 then
      ma('; ');
    if p^.isOut then
      ma('out ')
    else if p^.varKind = vkVarParam then
      ma('var ')
    else if p^.isConstRef then
      ma('constref ')
    else if p^.varKind = vkConstParam then
      ma('const ');
    if (p^.varSpec <> nil) and (p^.varType <> nil) then
      bufAddTypeName(msg, p^.varType)
    else
      ma('?');
  end;
  ma(')');
  if l^.isFunction then
  begin
    ma(': ');
    if (l^.resultSpec <> nil) and (l^.resultType <> nil) then
      bufAddTypeName(msg, l^.resultType)
    else
      ma('?');
  end;
end;

function callCost(r: PDecl; const args, argTys: TPtrList; var penalty: longint): longint;
var
  map: TPtrList;
  named: boolean;
  i, cost, c: longint;
  p: PDecl;
  pt, at: PType;
  arg: PNode;
begin
  penalty := 0;
  callCost := -1;
  named := hasNamedArgs(args);
  if named then
  begin
    if not mapArgs(r^.params, args, map, false, r^.ident) then
      exit;
  end
  else if not acceptsArgCount(r^.params, args.count) then
    exit;
  cost := 0;
  for i := 0 to args.count - 1 do
  begin
    if named then
      p := PDecl(listGet(map, i))
    else
      p := PDecl(listGet(r^.params, i));
    pt := p^.varType;
    at := PType(listGet(argTys, i));
    arg := argValue(PNode(listGet(args, i)));
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
    if pendingLambda(arg) <> nil then { its shape }
    begin
      c := lambdaCost(pendingLambda(arg), pt);
      if c < 0 then
        exit;
      cost := cost + c;
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
    penalty := penalty + signPenalty(pt, at);
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
    else if p^.isConstRef then
      ma('constref ')
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

function hasProceduralParam(const cands, args: TPtrList; i: longint): boolean;
var
  k: longint;
  p: PDecl;
begin
  hasProceduralParam := false;
  for k := 0 to cands.count - 1 do
  begin
    p := paramOfArg(PDecl(listGet(cands, k))^.params, args, i);
    if (p <> nil) and (p^.varType <> nil) and (p^.varType^.kind = tyRoutine) then
      exit(true);
  end;
end;

function hasConstArrayParam(const cands, args: TPtrList; i: longint): boolean;
var
  k: longint;
  p: PDecl;
begin
  hasConstArrayParam := false;
  for k := 0 to cands.count - 1 do
  begin
    p := paramOfArg(PDecl(listGet(cands, k))^.params, args, i);
    if (p <> nil) and (p^.varType <> nil) and
       (p^.varType^.kind in [tyArrayOfConst, tyOpenArray]) then
      exit(true);
  end;
end;

{ The parameters of r that a call with n arguments leaves to their default
  values. }
function defaultsUsed(r: PDecl; n: longint): longint;
begin
  if r^.params.count > n then
    defaultsUsed := r^.params.count - n
  else
    defaultsUsed := 0;
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
    if (getBase(t)^.kind = tyWideChar) or
       not (isIntegerLike(t) or (t^.kind = tyReal) or (getBase(t)^.kind = tyChar) or
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

{ The index of the type parameter of def that ts names, or -1. }
function typeParamNamed(def: PDecl; ts: PTypeSpec): longint;
var
  k: longint;
begin
  typeParamNamed := -1;
  if (ts = nil) or (ts^.kind <> tsNamed) or (ts^.tquals.count > 0) then
    exit;
  for k := 0 to def^.typeParams.count - 1 do
    if PIdent(listGet(def^.typeParams, k))^.name = ts^.tname.name then
      exit(k);
end;

{ name(args), where name is not declared but generic routines name<...>
  are: the specialization for the type arguments that the arguments give (a
  parameter of type T or array of T gives T the type of its argument;
  several arguments for one T, their common type), and for those still
  unknown the type want wanted of the result, if the result is a T. found:
  there is such a generic. Nil without one, and after a diagnostic. }
function inferFor(const name: TIdent; var generics: TPtrList; const args: TPtrList;
                  want: PType): PDecl; forward;
procedure completeLambda(a: PNode; want: PType); forward;

function inferSpecializationW(const name: TIdent; const args: TPtrList;
                              want: PType; var found: boolean;
                              var w: PWithContext): PDecl;
var
  generics: TPtrList;
  n: longint;
  g: PDecl;
  gw: PWithContext;
begin
  inferSpecializationW := nil;
  found := false;
  listInit(generics);
  for n := 1 to 16 do
  begin
    gw := nil;
    g := scopeLookup(cur, genericKey(name.name, n), gw);
    if (g <> nil) and (g^.kind = dkRoutine) then
    begin
      if generics.count = 0 then { (M(args) in a method: Self.M) }
        w := gw;
      listAdd(generics, g);
    end;
  end;
  if generics.count = 0 then
    exit;
  found := true;
  inferSpecializationW := inferFor(name, generics, args, want);
end;

function inferSpecialization(const name: TIdent; const args: TPtrList;
                             want: PType; var found: boolean): PDecl;
var
  w: PWithContext;
begin
  w := nil;
  inferSpecialization := inferSpecializationW(name, args, want, found, w);
end;

{ Of generics (routines or methods named name), the one whose parameters
  take args, specialized for the type arguments they (and want, the type
  wanted of the result) give; nil after a diagnostic. }
function inferFor(const name: TIdent; var generics: TPtrList; const args: TPtrList;
                  want: PType): PDecl;
var
  fit, map, bound: TPtrList;
  i, k, j: longint;
  g, def, p: PDecl;
  ok: boolean;
  a: PNode;
  ts, elem: PTypeSpec;
  at, str: PType;
  el: PSetElement;

  { T (number k) is also the type ty of the value v }
  procedure give(k: longint; ty: PType; v: PNode);
  var
    joined, old: PType;
  begin
    if isError(ty) then
    begin
      ok := false;
      exit;
    end;
    if (ty^.kind = tyStringLit) or isCharLiteral(v) then
      ty := str;
    if not isInferable(ty) then
      exit; { (nil, []: says nothing) }
    old := PType(listGet(bound, k));
    if old <> nil then
      joined := commonType(old, ty, str)
    else
      joined := ty;
    if joined = nil then
    begin
      ms('cannot infer the type parameter ''');
      maN(PIdent(listGet(def^.typeParams, k))^.spelling);
      ma(''' of ''');
      maN(name.spelling);
      ma(''': the arguments give both ');
      maT(old);
      ma(' and ');
      maT(ty);
      ma('; give the type: specialize ');
      maN(name.spelling);
      ma('<...>');
      errR(v^.b, nodeRange(v));
      ok := false;
      exit;
    end;
    listSet(bound, k, joined);
  end;


  { does ts (written in def) name one of def's type parameters, itself or
    in its type arguments? }
  function mentionsTypeParam(ts: PTypeSpec): boolean;
  var
    m: longint;
  begin
    mentionsTypeParam := false;
    if ts = nil then
      exit;
    if typeParamNamed(def, ts) >= 0 then
      exit(true);
    if ts^.kind = tsSpecialize then
    begin
      for m := 0 to ts^.spArgs.count - 1 do
        if mentionsTypeParam(PTypeSpec(listGet(ts^.spArgs, m))) then
          exit(true);
      exit;
    end;
    if ts^.kind = tsDynArray then
      mentionsTypeParam := mentionsTypeParam(ts^.dynSpec);
    if (ts^.kind = tsAsync) and ts^.asyncIterator then
      mentionsTypeParam := mentionsTypeParam(ts^.asyncInner);
  end;

  { ss, a parameter's type written in def (specialize TGen<...>), and at,
    the type of its argument v: if at is a specialization of TGen, the type
    arguments of def that ss's arguments name are at's (through give;
    nested specializations and dynamic arrays alike) }
  procedure inferFromSpecialization(ss: PTypeSpec; at: PType; v: PNode);
  var
    gs: PScope;
    gd: PDecl;
    ti: PTypeInstance;
    m, kk, q: longint;
    ta: PTypeSpec;
    tof: PType;
  begin
    gs := PScope(def^.genericScope);
    if (gs = nil) or not typeInstancesInit then
      exit;
    gd := scopeFind(gs, genericKey(ss^.spName.name, ss^.spArgs.count));
    if (gd = nil) or (gd^.kind <> dkType) then
      exit;
    for m := 0 to typeInstances.count - 1 do
    begin
      ti := PTypeInstance(listGet(typeInstances, m));
      if (ti^.g <> gd) or (ti^.td^.declType <> at) then
        continue;
      for kk := 0 to ss^.spArgs.count - 1 do
      begin
        ta := PTypeSpec(listGet(ss^.spArgs, kk));
        tof := PType(listGet(ti^.types, kk));
        q := typeParamNamed(def, ta);
        if q >= 0 then
          give(q, tof, v)
        else if ta^.kind = tsSpecialize then
          inferFromSpecialization(ta, tof, v)
        else if ta^.kind = tsDynArray then
        begin
          q := typeParamNamed(def, ta^.dynSpec);
          if (q >= 0) and (tof^.kind = tyDynArray) then
            give(q, tof^.dynElement, v);
        end;
      end;
      exit;
    end;
  end;

  { Lambda l (a), the argument for parameter p of generic def, whose type
    is a specialization of a generic procedural type g (specialize
    TFunc<T, R>): the type arguments of def it gives (into bound, through
    give). A type written in l gives the type argument there; l's
    parameters without one take the types known; then, if l's result is a
    type argument still unknown, l is completed for those parameter types
    and its body gives it. }
  procedure throughLambda(p: PDecl; a: PNode; l: PDecl);
  var
    ss, rs, ta: PTypeSpec;
    gs, savedCur: PScope;
    gt, rp, lp, v: PDecl;
    wantParams: TPtrList;
    known: boolean;
    q, jj, kk: longint;
    t, w, rt: PType;
    wantT: PType;

    { the type parameter of gt that ts names, or -1 }
    function gParam(ts: PTypeSpec): longint;
    var
      m: longint;
    begin
      gParam := -1;
      if (ts = nil) or (ts^.kind <> tsNamed) or (ts^.tquals.count > 0) then
        exit;
      for m := 0 to gt^.tgParams.count - 1 do
        if PIdent(listGet(gt^.tgParams, m))^.name = ts^.tname.name then
          exit(m);
    end;

    { the type of gt's type parameter jx as def gives it now: a type
      argument of def (nil while unknown), or a type named where def is
      declared }
    function typeOfArg(jx: longint; var kx: longint): PType;
    var
      r: PType;
    begin
      typeOfArg := nil;
      ta := PTypeSpec(listGet(ss^.spArgs, jx));
      kx := typeParamNamed(def, ta);
      if kx >= 0 then
        exit(PType(listGet(bound, kx)));
      if ta^.kind <> tsNamed then
        exit;
      savedCur := cur;
      cur := gs;
      r := resolveType(ta, nil);
      cur := savedCur;
      if not isError(r) then
        typeOfArg := r;
    end;

  begin
    ss := p^.varSpec;
    gs := PScope(def^.genericScope);
    if (ss = nil) or (ss^.kind <> tsSpecialize) or (gs = nil) then
      exit;
    gt := scopeFind(gs, genericKey(ss^.spName.name, ss^.spArgs.count));
    rs := nil;
    if (gt <> nil) and (gt^.kind = dkType) and gt^.tgGeneric then
      rs := gt^.typeSpec;
    if (rs = nil) or (rs^.kind <> tsRoutine) or (rs^.rtIsFunction <> l^.isFunction) or
       (rs^.rtParams.count <> l^.params.count) then
      exit;
    listInit(wantParams);
    known := true;
    q := 0;
    while (q < rs^.rtParams.count) and ok do
    begin
      rp := PDecl(listGet(rs^.rtParams, q));
      lp := PDecl(listGet(l^.params, q));
      jj := gParam(rp^.varSpec);
      kk := -1;
      t := nil;
      if jj >= 0 then
        t := typeOfArg(jj, kk);
      if lp^.varSpec <> nil then { written: it gives the type argument }
      begin
        w := resolveType(lp^.varSpec, nil);
        if kk >= 0 then
          give(kk, w, a);
        if isError(w) then
          t := nil
        else
          t := w;
      end;
      if t = nil then
        known := false;
      v := newDecl(dkVar, rp^.ident, rp^.ident.loc);
      v^.varKind := rp^.varKind;
      v^.isOut := rp^.isOut;
      v^.isConstRef := rp^.isConstRef;
      v^.varType := t;
      listAdd(wantParams, v);
      q := q + 1;
    end;
    if not ok or not l^.isFunction then
      exit;
    jj := gParam(rs^.rtResult);
    kk := -1;
    if jj < 0 then
      exit;
    t := typeOfArg(jj, kk);
    if kk < 0 then
      exit;
    if l^.resultSpec <> nil then { written }
    begin
      give(kk, resolveType(l^.resultSpec, nil), a);
      exit;
    end;
    if (listGet(bound, kk) <> nil) or not known then
      exit;
    wantT := newRoutineType(wantParams, nil, rs^.rtCallConv = ccCDecl);
    wantT^.routineOfObject := rs^.rtOfObject;
    wantT^.routineReference := rs^.rtReference;
    completeLambda(a, wantT);
    rt := l^.resultType;
    if l^.lambdaBad or (rt = nil) or isError(rt) then
    begin
      ok := false;
      exit;
    end;
    give(kk, rt, a);
  end;

begin
  inferFor := nil;
  if generics.count > 1 then { those that take these arguments }
  begin
    listInit(fit);
    for i := 0 to generics.count - 1 do
    begin
      g := PDecl(listGet(generics, i));
      if hasNamedArgs(args) then
        ok := mapArgs(genericDefinition(g)^.params, args, map, false, name)
      else
        ok := acceptsArgCount(genericDefinition(g)^.params, args.count);
      if ok then
        listAdd(fit, g);
    end;
    if fit.count <> 1 then
    begin
      ms('cannot tell which generic ''');
      maN(name.spelling);
      ma(''' is called; give the types: specialize ');
      maN(name.spelling);
      ma('<...>');
      errR(name.loc, identRange(name));
      exit;
    end;
    generics := fit;
  end;
  g := PDecl(listGet(generics, 0));
  def := genericDefinition(g);
  listInit(bound);
  for k := 0 to def^.typeParams.count - 1 do
    listAdd(bound, nil);
  str := ansiStringType;
  ok := true;
  i := 0;
  while (i < args.count) and ok do
  begin
    p := paramOfArg(def^.params, args, i);
    a := argValue(PNode(listGet(args, i)));
    i := i + 1;
    if (p = nil) or (a^.kind = nkFormatted) then
      continue;
    ts := p^.varSpec;
    k := typeParamNamed(def, ts);
    if k >= 0 then
    begin
      if not isRoutineName(a) then
        give(k, check(a), a);
      continue;
    end;
    elem := nil;
    if (ts <> nil) and (ts^.kind = tsOpenArray) then
      elem := ts^.openSpec
    else if (ts <> nil) and (ts^.kind = tsDynArray) then
      elem := ts^.dynSpec;
    k := typeParamNamed(def, elem);
    if k >= 0 then
    begin
      if a^.kind = nkSetCtor then { [a, b, c] }
      begin
        for j := 0 to a^.setElems.count - 1 do
        begin
          el := PSetElement(listGet(a^.setElems, j));
          if (el^.hi = nil) and ok then
            give(k, check(el^.lo), el^.lo);
        end;
        continue;
      end;
      at := check(a);
      if isError(at) then
        ok := false
      else if at^.kind = tyArray then
        give(k, at^.elementType, a)
      else if at^.kind = tyDynArray then
        give(k, at^.dynElement, a)
      else if at^.kind = tyOpenArray then
        give(k, at^.openElement, a);
    end
    { iterator T: T from the iterator's values }
    else if (ts <> nil) and (ts^.kind = tsAsync) and ts^.asyncIterator and
            (typeParamNamed(def, ts^.asyncInner) >= 0) then
    begin
      at := check(a);
      if isError(at) then
        ok := false
      else if at^.kind = tyIterator then
        give(typeParamNamed(def, ts^.asyncInner), at^.iterElement, a);
    end
    { specialize TGen<..., T, ...>: T from the specialization the argument
      is (a lambda's are the lambda pass's, below) }
    else if (ts <> nil) and (ts^.kind = tsSpecialize) and mentionsTypeParam(ts) and
            (pendingLambda(a) = nil) and not isRoutineName(a) then
    begin
      at := check(a);
      if isError(at) then
        ok := false
      else
        inferFromSpecialization(ts, at, a);
    end;
  end;
  if not ok then
    exit;
  { lambdas passed for a parameter of a generic procedural type (specialize
    TFunc<T, R>): the types written in them give type arguments; those of
    their parameters not written come from the type arguments known (T
    in), and a result not written from the body, checked with them (R
    out) }
  i := 0;
  while (i < args.count) and ok do
  begin
    p := paramOfArg(def^.params, args, i);
    a := argValue(PNode(listGet(args, i)));
    i := i + 1;
    if (p <> nil) and (a^.kind = nkAddrOf) and (a^.addrLambda <> nil) and
       not a^.addrLambda^.lambdaDone then
      throughLambda(p, a, a^.addrLambda);
  end;
  if not ok then
    exit;
  { the type wanted of the result, for a result of type T }
  k := typeParamNamed(def, def^.resultSpec);
  if (k >= 0) and (listGet(bound, k) = nil) and (want <> nil) then
    if not isError(want) and isInferable(want) then
      listSet(bound, k, want);
  for k := 0 to bound.count - 1 do
    if listGet(bound, k) = nil then
    begin
      ms('cannot infer the type parameter ''');
      maN(PIdent(listGet(def^.typeParams, k))^.spelling);
      ma(''' of ''');
      maN(name.spelling);
      ma(''' in this call: no argument gives it; give the type: specialize ');
      maN(name.spelling);
      ma('<...>');
      errR(name.loc, identRange(name));
      noteDeclaredHere(g);
      exit;
    end;
  if g^.memberKey <> nil then
    inferFor := instantiateMethod(name, g, bound)
  else
    inferFor := instantiateGeneric(name, g, bound);
end;

function resolveOverload(first: PDecl; const args: TPtrList; const callee: TIdent;
                         rng: TRange): PDecl;
var
  cands, argTys: TPtrList;
  i, cost, bestCost, penalty, bestPenalty, defaults, bestDefaults: longint;
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
    a := argValue(PNode(listGet(args, i)));
    if a^.kind = nkFormatted then
      exit(first); { reported with the call's arguments }
    { a lambda: chosen by its shape (lambdaCost), completed afterwards
      with the parameter's type }
    if pendingLambda(a) <> nil then
    begin
      if not resolveLambdaSpecs(pendingLambda(a)) then
        exit;
      listAdd(argTys, nil);
      continue;
    end;
    { [...] for an open array need not be a valid set }
    if (a^.kind = nkSetCtor) and hasConstArrayParam(cands, args, i) then
    begin
      listAdd(argTys, arrayOfConstT);
      continue;
    end;
    allowSlice := isSliceCall(a);
    { a routine's name: for a procedural parameter if a candidate has one
      here, else a call (a function without parameters) }
    if isRoutineName(a) and hasProceduralParam(cands, args, i) then
      t := nil
    else
      t := checkAllowingUntyped(a);
    allowSlice := false;
    if (t <> nil) and isError(t) then
      exit;
    listAdd(argTys, t);
  end;
  { the best conversions of the arguments written decide; then the fewest
    parameters left to their default values (a routine that takes just
    these arguments is preferred); a tie is an error }
  best := nil;
  bestCost := -1;
  bestPenalty := 0;
  bestDefaults := 0;
  tie := false;
  for i := 0 to cands.count - 1 do
  begin
    r := PDecl(listGet(cands, i));
    cost := callCost(r, args, argTys, penalty);
    if cost < 0 then
      continue;
    defaults := defaultsUsed(r, args.count);
    if (best = nil) or (cost < bestCost) or
       ((cost = bestCost) and (penalty < bestPenalty)) or
       ((cost = bestCost) and (penalty = bestPenalty) and
        (defaults < bestDefaults)) then
    begin
      best := r;
      bestCost := cost;
      bestPenalty := penalty;
      bestDefaults := defaults;
      tie := false;
    end
    else if (cost = bestCost) and (penalty = bestPenalty) and
            (defaults = bestDefaults) then
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
    if PNode(listGet(args, i))^.kind = nkNamedArg then
    begin
      maN(PNode(listGet(args, i))^.naName.spelling);
      ma(' := ');
    end;
    t := PType(listGet(argTys, i));
    if t <> nil then
      bufAddTypeName(msg, t)
    else if pendingLambda(argValue(PNode(listGet(args, i)))) <> nil then
      maLambdaShape(pendingLambda(argValue(PNode(listGet(args, i)))))
    else
      ma('routine');
  end;
  ma(')');
  errR(callee.loc, rng);
  for i := 0 to cands.count - 1 do
  begin
    r := PDecl(listGet(cands, i));
    cost := callCost(r, args, argTys, penalty);
    if (best = nil) or ((cost = bestCost) and (penalty = bestPenalty) and
                        (defaultsUsed(r, args.count) = bestDefaults)) then
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

{ Where the routine being checked ends (its end, or exit): its non-null
  result, and a constructor's non-null fields, must be set. }
procedure nullCheckEnd(loc: TLoc; atExit: boolean);
var
  i: longint;
  d: PDecl;
  t: PType;
  isResult: boolean;
begin
  if not nullSafeHere or (flow = nil) or not ownedLive(flow^.s) or (curRoutine = nil) then
    exit;
  for i := 0 to flow^.n - 1 do
  begin
    if not flow^.isNull[i] or ((flow^.s.moved and (int64(1) shl i)) = 0) then
      continue;
    d := flow^.vars[i];
    isResult := (d^.kind = dkVar) and (d^.varKind = vkResult);
    if not isResult and (d^.kind <> dkField) then
      continue;
    if not declNonNull(d) then
      continue;
    t := nullTypeOf(d);
    if isResult then
    begin
      ms('function ''');
      maN(curRoutine^.ident.spelling);
      ma(''' may return nil');
      if atExit then
        ma(' here');
      ma(': its result of the non-null type ');
      maT(t);
      ma(' may not be assigned');
    end
    else
    begin
      ms('constructor ''');
      maN(curRoutine^.ident.spelling);
      ma(''' may leave field ''');
      maN(d^.ident.spelling);
      ma(''' of the non-null type ');
      maT(t);
      ma(' nil');
      if atExit then
        ma(' here');
    end;
    ma(' (assign it on every path, or declare it ''');
    bufAddTypeName(msg, t);
    ma('?'')');
    err(loc);
    flow^.s.moved := flow^.s.moved and not (int64(1) shl i); { reported once }
  end;
end;

{ The block's references that are followed from its start: its
  variables, its routine's T? parameters, result and (a constructor) own
  non-null fields. }
procedure nullTrackBlock(b: PBlock);
var
  r, p, res, m, v: PDecl;
  c: PType;
  i: longint;
begin
  r := curRoutine;
  if (r <> nil) and (r^.body = b) then
  begin
    for i := 0 to r^.params.count - 1 do
    begin
      p := PDecl(listGet(r^.params, i));
      if isNullRef(p^.varType) and declNullable(p) then
        nullTrack(p, true, true);
    end;
    res := r^.resultVar;
    if (res <> nil) and isNullRef(res^.varType) and not r^.isLambda and
       (declNonNull(res) or declNullable(res)) then
      nullTrack(res, true, false); { (nil until assigned) }
    if r^.methodKind = mtConstructor then
    begin
      c := r^.ownerClass;
      if (c <> nil) and (c^.kind = tyClass) then
        for i := 0 to c^.classMembers.count - 1 do
        begin
          m := PDecl(listGet(c^.classMembers, i));
          if (m^.kind = dkField) and isNullRef(m^.fieldType) and declNonNull(m) then
            nullTrack(m, true, false);
        end;
    end;
  end;
  for i := 0 to b^.decls.count - 1 do
  begin
    v := PDecl(listGet(b^.decls, i));
    if (v^.kind = dkVar) and (v^.varKind = vkVariable) and isNullRef(v^.varType) and
       not ((v^.init <> nil) and not v^.isInitVar) and not v^.isClassVar and
       (v^.absTarget = nil) and not v^.varExternal and (v^.varSpec <> nil) and
       v^.varSpec^.tsNullSafe then
      nullTrack(v, true, false); { nil until assigned }
  end;
end;

procedure finishBlock(b: PBlock; warnUnused: boolean);
var
  i, savedHandlers, savedFinally: longint;
  d: PDecl;
  savedBody: PNode;
  savedFlow: POwnedFlow;
  blockFlow: TOwnedFlow;
  savedInline: PPtrList;
  blockInline: TPtrList;
  savedNullSafe: boolean;
begin
  { ISO 7185 6.1.4: every forward declaration needs a definition in the
    same block }
  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    if (d^.kind = dkRoutine) and d^.isForward and isUndefinedHeading(d) then
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
  savedFlow := flow;
  blockFlow.n := 0;
  blockFlow.tick := 0;
  blockFlow.loop := nil;
  blockFlow.depth := 0;
  blockFlow.tryPart := nil;
  ownedClear(blockFlow.s);
  blockFlow.s.valid := liveBit;
  for i := 0 to maxOwned - 1 do
    blockFlow.isNull[i] := false;
  flow := @blockFlow;
  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    if (d^.kind = dkVar) and d^.isOwned then
      ownedDeclare(d);
  end;
  savedNullSafe := nullSafeHere;
  nullSafeHere := b^.nullSafe and (mode = lmObjFPC);
  if nullSafeHere then
    nullTrackBlock(b);
  savedInline := inlineVars;
  listInit(blockInline);
  inlineVars := @blockInline;
  visitStmt(b^.body);
  if (curRoutine <> nil) and (curRoutine^.body = b) then
    nullCheckEnd(b^.body^.e - 3, false);
  nullSafeHere := savedNullSafe;
  inlineVars := savedInline;
  flow := savedFlow;
  { they are the block's too, after the others }
  for i := 0 to blockInline.count - 1 do
    listAdd(b^.decls, listGet(blockInline, i));
  listFree(blockInline);
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

{ An async routine's restrictions: its task may outlive its caller, so its
  arguments are copied (no var, out or constref parameters, no open or
  untyped ones), and it is not nested (but a spawn block, which its scope
  waits for) nor a virtual method, constructor or destructor. }
{ (The same for an iterator routine, whose iterator may outlive its caller
  too.) }
procedure checkAsyncRoutine(r: PDecl);
var
  i: longint;
  p: PDecl;
  t: PType;
  kind, why, rkind, its: string;
  isIt: boolean;
begin
  isIt := r^.iterType <> nil;
  if isIt then
  begin
    rkind := 'iterator';
    its := 'its iterator';
  end
  else
  begin
    rkind := 'async';
    its := 'its task';
  end;
  if (r^.parent <> nil) and not r^.isSpawnBlock and not r^.isLambda then
  begin
    ms(rkind + ' routine ''');
    maN(r^.ident.spelling);
    ma(''' must be declared at program or unit level, or be a method ' +
       '(' + its + ' may outlive the routine around it)');
    errR(r^.ident.loc, identRange(r^.ident));
  end;
  if r^.methodKind <> mtNone then
  begin
    why := '';
    if (r^.methodKind = mtConstructor) or (r^.methodKind = mtDestructor) then
    begin
      if isIt then
        why := 'a constructor or a destructor cannot be an iterator'
      else
        why := 'a constructor or a destructor cannot be async';
    end
    else if r^.isVirtual or r^.isAbstract or r^.isOverride or (r^.vmtIndex >= 0) then
      why := 'an ' + rkind + ' method cannot be virtual (not supported yet)'
    else if (r^.ownerClass <> nil) and (r^.ownerClass^.kind = tyClass) and
            ((r^.ownerClass^.classRecord <> nil) or (r^.ownerClass^.classHelperFor <> nil)) then
    begin
      if isIt then
        why := 'a record''s or helper''s method cannot be an iterator (its ' +
               'Self is passed by reference)'
      else
        why := 'a record''s or helper''s method cannot be async (its Self is ' +
               'passed by reference)';
    end;
    if why <> '' then
    begin
      ms(why);
      errR(r^.ident.loc, identRange(r^.ident));
    end;
  end;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    t := p^.varType;
    kind := '';
    if (p^.varKind = vkVarParam) and p^.isOut then
      kind := 'an ''out'''
    else if p^.varKind = vkVarParam then
      kind := 'a ''var'''
    else if p^.isConstRef then
      kind := 'a ''constref'''
    else if (t <> nil) and (t^.kind in [tyOpenArray, tyArrayOfConst]) then
      kind := 'an open array'
    else if (t <> nil) and (t^.kind = tyUntyped) then
      kind := 'an untyped';
    if kind <> '' then
    begin
      ms('parameter ''');
      maN(p^.ident.spelling);
      ma(''' of ' + rkind + ' routine ''');
      maN(r^.ident.spelling);
      ma(''' cannot be ' + kind + ' parameter: ' + its + ' may outlive the ' +
         'caller''s variable; pass a value');
      errR(p^.loc, identRange(p^.ident));
    end;
  end;
end;

procedure checkRoutineBody(r: PDecl);
var
  savedScope, base: PScope;
  outers: TPtrList;
  o: PType;
  savedRoutine, p, prev: PDecl;
  i: longint;
  selfRef: PNode;
  w: PWithContext;
begin
  if deferredBodies <> nil then { of a specialization being declared: later }
  begin
    listAdd(deferredBodies^, r);
    exit;
  end;
  if (r^.asyncType <> nil) or (r^.iterType <> nil) then
    checkAsyncRoutine(r);
  savedScope := cur;
  savedRoutine := curRoutine;
  { the method of a nested class or record also sees the constants and
    types of the classes around it }
  if r^.ownerClass <> nil then
  begin
    listInit(outers);
    o := r^.ownerClass^.classOuter;
    while o <> nil do
    begin
      listAdd(outers, o);
      o := o^.classOuter;
    end;
    for i := outers.count - 1 downto 0 do
      cur := newNestedScope(cur, PType(listGet(outers, i)));
  end;
  base := cur;
  if r^.isStatic then { a record's static method: its statics }
  begin
    cur := newClassScope(cur, r^.ownerClass, nil);
    cur^.routineBoundary := true;
  end
  else if (r^.methodKind <> mtNone) or
          (r^.isMethodLambda and (r^.ownerClass <> nil)) then
  begin
    { a method sees the members of its class through Self (a method-pointer
      lambda outside methods: a Self no one sees) }
    selfRef := newNode(nkNameRef, noLoc, noLoc);
    selfRef^.ref := r^.selfVar^.ident;
    selfRef^.refDecl := r^.selfVar;
    selfRef^.typ := r^.selfVar^.varType;
    w := PWithContext(arenaAlloc(sizeof(TWithContext)));
    w^.rec := selfRef;
    w^.isSelf := true;
    w^.routine := r;
    if r^.ownerClass^.classHelperFor <> nil then
      w^.helper := r^.ownerClass;
    { a helper's method for an advanced record also sees the record's
      methods and properties (the helper's own members first) }
    if memberTableOf(r^.ownerClass^.classHelperFor) <> nil then
    begin
      cur := newClassScope(cur, memberTableOf(r^.ownerClass^.classHelperFor), w);
      cur^.routineBoundary := true;
    end;
    cur := newClassScope(cur, r^.ownerClass, w);
    if cur^.parent = base then
      cur^.routineBoundary := true;
    { a record helper's method also sees the record's fields }
    if (r^.ownerClass^.classHelperFor <> nil) and
       (r^.ownerClass^.classHelperFor^.kind = tyRecord) then
      cur := newWithScope(cur, r^.ownerClass^.classHelperFor, w);
  end;
  cur := newScope(cur);
  if cur^.parent = base then
    cur^.routineBoundary := true;
  curRoutine := r;
  if ((r^.methodKind <> mtNone) and not r^.isStatic) or
     (r^.isMethodLambda and (r^.ownerClass <> nil)) then
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
  { $modeswitch result (objfpc): Result names the function result }
  if r^.resultName and r^.isFunction and (r^.resultVar <> nil) then
    scopeInsert(cur, intern('result'), r^.resultVar);
  checkBlock(r^.body);
  if r^.isFunction and not r^.resultAssigned and (r^.resultType <> nil) and
     not isError(r^.resultType) then
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
    if f^.kind = nkForIn then
    begin
      if f^.inVar^.refDecl <> target^.refDecl then
        continue;
    end
    else if f^.forVar^.refDecl <> target^.refDecl then
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
  loTy, hiTy: PType;
begin
  ok := true;
  loTy := nil;
  hiTy := nil;
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
    begin
      l^.loValue := c.value.intVal;
      loTy := c.ty;
    end
    else
    begin
      l^.hiValue := c.value.intVal;
      hiTy := c.ty;
    end;
  end;
  if l^.hi = nil then
  begin
    l^.hiValue := l^.loValue;
    hiTy := loTy;
  end;
  if ok and (caseKey(l^.loValue, selTy) > caseKey(l^.hiValue, selTy)) then
  begin
    ms('case label range is empty');
    errR(l^.lo^.b, caseLabelRange(l));
    ok := false;
  end;
  if ok and not isError(selTy) then
  begin
    if not constantInRange(l^.loValue, loTy, selTy) or
       not constantInRange(l^.hiValue, hiTy, selTy) then
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
    if (pa^.varKind <> pb^.varKind) or (pa^.isOut <> pb^.isOut) or
       (pa^.isConstRef <> pb^.isConstRef) then
      exit(false);
    ta := pa^.varType;
    tb := pb^.varType;
    if (ta <> nil) and (tb <> nil) and (ta^.kind = tyRoutine) and (tb^.kind = tyRoutine) then
    begin
      if (ta^.routineCDecl <> tb^.routineCDecl) or
         (ta^.routineOfObject <> tb^.routineOfObject) or
         (ta^.routineReference <> tb^.routineReference) or
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

{ The lambda a (function(x) => e), used where a value of type want is
  wanted (nil: none): its hidden routine is declared here, nested in the
  current routine and in the current scope. Parameter types left out are
  want's; the result type left out is want's, or else the type of the body
  (set by visitAssign). Then a is @routine. }
procedure noteLambdaUse(d: PDecl; w: PWithContext; const id: TIdent); forward;

{ Lambda r (at a) wanted as a method pointer: it gets a Self of its own, of
  the class of the method it is written in (the nearest routine around it
  with a Self, which must be an object), and that method's Self is its
  object. Reported (and r bad) if there is none. }
function makeMethodLambda(r: PDecl; a: PNode): boolean;
var
  m, outerSelf, self_: PDecl;
  id: TIdent;
begin
  m := curRoutine;
  while m <> nil do
  begin
    if ((m^.methodKind <> mtNone) and not m^.isStatic) or m^.isMethodLambda then
      break;
    m := m^.parent;
  end;
  outerSelf := nil;
  if m <> nil then
    outerSelf := m^.selfVar;
  if m = nil then { outside methods: no object (nil), no members }
  begin
    self_ := newDecl(dkVar, memberIdent('Self'), noLoc);
    self_^.varKind := vkValueParam;
    self_^.varType := tyUntypedPointerT;
    self_^.owner := r;
    r^.selfVar := self_;
    r^.isMethodLambda := true;
    r^.lambdaSelf := nil;
    exit(true);
  end;
  if (outerSelf = nil) or (outerSelf^.varType = nil) or
     (outerSelf^.varType^.kind <> tyClass) then
  begin
    ms('a lambda can be a method pointer (''of object'') only in an object''s ' +
       'method, whose Self is its object, or outside methods (its object is nil)');
    errR(a^.b, nodeRange(a));
    r^.lambdaBad := true;
    exit(false);
  end;
  r^.ownerClass := outerSelf^.varType;
  makeSelf(r, outerSelf^.varType);
  r^.isMethodLambda := true;
  r^.lambdaSelf := outerSelf;
  { its object: m's Self, where the lambda is made }
  if m <> curRoutine then
  begin
    outerSelf^.captured := true;
    id := memberIdent('Self');
    id.loc := a^.b;
    noteLambdaUse(outerSelf, nil, id);
  end;
  makeMethodLambda := true;
end;

procedure declareAndCheckLambda(r: PDecl); forward;

procedure completeLambda(a: PNode; want: PType);
var
  r, p, wp, res: PDecl;
  i: longint;
  t: PType;
begin
  r := a^.addrLambda;
  if (r = nil) or r^.lambdaDone then
    exit;
  r^.lambdaDone := true;
  if want <> nil then
    if want^.routineReference then
      r^.isClosure := true;
  if want <> nil then
    if want^.routineOfObject then
      if not makeMethodLambda(r, a) then
        exit;
  if want <> nil then
    if want^.routineParams.count <> r^.params.count then
    begin
      ms('this lambda has ' + intToStr(r^.params.count));
      if r^.params.count = 1 then
        ma(' parameter')
      else
        ma(' parameters');
      ma(', but ');
      maT(want);
      ma(' is wanted here');
      errR(a^.b, nodeRange(a));
      r^.lambdaBad := true;
    end;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    p^.owner := r;
    wp := nil;
    if want <> nil then
      if i < want^.routineParams.count then
        wp := PDecl(listGet(want^.routineParams, i));
    if p^.varSpec <> nil then
      t := resolveType(p^.varSpec, nil)
    else if (wp <> nil) and (wp^.varType <> nil) then
    begin
      t := wp^.varType;
      if p^.varKind = vkValueParam then { (x): as wanted }
      begin
        p^.varKind := wp^.varKind;
        p^.isOut := wp^.isOut;
        p^.isConstRef := wp^.isConstRef;
      end;
    end
    else
    begin
      r^.lambdaBad := true;
      if want = nil then
      begin
        ms('cannot infer the type of parameter ''');
        maN(p^.ident.spelling);
        ma(''' in this lambda: there is no expected type; add a type, for ' +
           'example function(');
        maN(p^.ident.spelling);
        ma(': Integer) => ...');
        errR(p^.loc, identRange(p^.ident));
      end;
      t := errorType;
    end;
    p^.varType := t;
  end;
  if r^.isFunction then
  begin
    t := nil; { (not known yet: the body's) }
    if r^.resultSpec <> nil then
      t := resolveType(r^.resultSpec, nil)
    else if want <> nil then
      t := want^.routineResult;
    r^.resultType := t;
    res := newDecl(dkVar, r^.ident, r^.ident.loc);
    res^.varSpec := r^.resultSpec;
    res^.varKind := vkResult;
    res^.varType := t;
    res^.owner := r;
    r^.resultVar := res;
  end;
  declareAndCheckLambda(r);
  if r^.isFunction and (r^.resultType = nil) then
  begin
    { (a block body that never sets Result: an expression body always gives
      a type, or an error) }
    ms('cannot tell the result type of this lambda: its body does not set ' +
       'Result; give it one: function(...): T => ...');
    errR(a^.b, nodeRange(a));
    r^.resultType := errorType;
    r^.resultVar^.varType := errorType;
  end;
end;

{ Lambda r (or a copy factory), its types known: declared here, nested in
  the current routine, and its body checked now, in the middle of a
  statement. }
procedure declareAndCheckLambda(r: PDecl);
var
  savedLoop: longint;
  savedSeq: pointer;
  savedSeqScope: PPScope;
  savedAssign, savedOwned, savedIndex, savedWanted: PNode;
  savedAddr: PType;
  savedKeep: boolean;
  savedCur: PScope;
begin
  r^.parent := curRoutine;
  if curRoutine <> nil then
    r^.level := curRoutine^.level + 1
  else
    r^.level := 1;
  r^.isUsed := true;
  { in the innermost scope of declarations (not that of a with statement
    or of a class's members, where it would not be found) }
  savedCur := cur;
  while ((cur^.withRecord <> nil) or (cur^.withClass <> nil) or
         (cur^.nestedOf <> nil)) and (cur^.parent <> nil) do
    cur := cur^.parent;
  declare(r);
  cur := savedCur;
  { its body is checked now, in the middle of a statement }
  savedLoop := loopDepth;
  savedSeq := directSequence;
  savedSeqScope := seqScope;
  savedAssign := curAssignTarget;
  savedOwned := ownedTarget;
  savedIndex := indexBase;
  savedWanted := wantedCall;
  savedAddr := addrOfTarget;
  savedKeep := keepCharLiterals;
  loopDepth := 0;
  directSequence := nil;
  seqScope := nil;
  curAssignTarget := nil;
  ownedTarget := nil;
  indexBase := nil;
  wantedCall := nil;
  addrOfTarget := nil;
  keepCharLiterals := false;
  checkRoutineBody(r);
  loopDepth := savedLoop;
  directSequence := savedSeq;
  seqScope := savedSeqScope;
  curAssignTarget := savedAssign;
  ownedTarget := savedOwned;
  indexBase := savedIndex;
  wantedCall := savedWanted;
  addrOfTarget := savedAddr;
  keepCharLiterals := savedKeep;
end;

{ The copy factory f of a lambda that copies what it uses, called by e
  where want is wanted: a closure must be. Its parameters, the copies, take
  the types of the variables they copy (read-only); its result is want, its
  body makes the lambda (completed for want). False after a diagnostic. }
function completeCopyFactory(f: PDecl; e: PNode; want: PType): boolean;
var
  i: longint;
  p, d, res: PDecl;
  t: PType;
  why: string;
  ok, keep: boolean;
  kept, keptArgs: TPtrList;
  w: PWithContext;
begin
  completeCopyFactory := false;
  f^.lambdaDone := true;
  ok := want <> nil;
  if ok then
    ok := (want^.kind = tyRoutine) and want^.routineReference;
  if not ok then
  begin
    ms('a lambda that copies what it uses must be a closure: a ''reference to'' ' +
       'type must be wanted here');
    errR(e^.b, nodeRange(e));
    f^.lambdaBad := true;
    exit;
  end;
  if f^.isCopyAll then { the names that are variables of routines }
  begin
    listInit(kept);
    listInit(keptArgs);
    for i := 0 to f^.params.count - 1 do
    begin
      w := nil;
      d := scopeLookup(cur, PDecl(listGet(f^.params, i))^.ident.name, w);
      keep := false;
      if (d <> nil) and (w = nil) then
        if (d^.kind = dkVar) and (d^.owner <> nil) and
           ((d^.init = nil) or d^.isInitVar) and not d^.isClassVar and
           (d^.varType <> nil) then
          keep := not (d^.varType^.kind in [tyOpenArray, tyArrayOfConst, tyUntyped]) and
                  not containsFile(d^.varType);
      if keep then
      begin
        listAdd(kept, listGet(f^.params, i));
        listAdd(keptArgs, listGet(e^.args, i));
      end;
    end;
    f^.params := kept;
    e^.args := keptArgs;
  end;
  ok := true;
  for i := 0 to f^.params.count - 1 do
  begin
    p := PDecl(listGet(f^.params, i));
    d := lookup(p^.ident.name);
    t := nil;
    if d <> nil then
    begin
      if d^.kind = dkVar then
        t := d^.varType
      else if d^.kind = dkField then
        t := d^.fieldType; { (an object's field, through Self or a with) }
    end;
    why := '';
    if t = nil then
      why := 'a variable, a parameter or a field'
    else if t^.kind in [tyOpenArray, tyArrayOfConst, tyUntyped] then
      why := 'of a type that can be copied, not an open or untyped parameter'
    else if containsFile(t) then
      why := 'of a type that can be copied, not a file';
    if why <> '' then
    begin
      ms('''');
      maN(p^.ident.spelling);
      ma(''' in the list of what a lambda copies must be ' + why);
      errR(p^.ident.loc, identRange(p^.ident));
      ok := false;
      t := errorType;
    end;
    p^.varType := t;
    p^.owner := f;
    p^.isReadOnly := true;
  end;
  if not ok then
  begin
    f^.lambdaBad := true;
    exit;
  end;
  f^.resultType := want;
  res := newDecl(dkVar, f^.ident, f^.ident.loc);
  res^.varSpec := nil;
  res^.varKind := vkResult;
  res^.varType := want;
  res^.owner := f;
  f^.resultVar := res;
  declareAndCheckLambda(f);
  completeCopyFactory := true;
end;

{ A use, in a lambda, of d (named id, found through w): if d belongs to a
  routine around the lambda (a variable, a member through Self or a with
  statement, a nested routine), the lambdas up to that routine must not
  outlive it (lambdaCapture). }
{ Why a closure cannot use v, a variable of a routine around it ('': it
  can): it is not kept in that routine's frame, or is not counted there. }
function closureCannotUse(v: PDecl): string;
var
  ofS: string;
  t: PType;
begin
  closureCannotUse := '';
  if v^.owner^.isLambda then
    ofS := 'the lambda around it'
  else
    ofS := '''' + nameStr(v^.owner^.ident.spelling) + '''';
  if v^.varKind = vkResult then
    exit('the result of ' + ofS);
  t := v^.varType;
  if v = v^.owner^.selfVar then
  begin
    if (t <> nil) and (t^.kind in [tyClass, tyClassRef]) then
      exit('');
    exit('through the Self of ' + ofS + ' (a record, passed by reference)');
  end;
  if (v^.varKind = vkVariable) or (t = nil) then
    exit('');
  if t^.kind in [tyOpenArray, tyArrayOfConst, tyUntyped] then
    exit('an open or untyped parameter of ' + ofS);
  if v^.isOut then
    exit('an ''out'' parameter of ' + ofS);
  if v^.varKind = vkVarParam then
    exit('a ''var'' parameter of ' + ofS);
  if ((v^.varKind = vkConstParam) or v^.isConstRef) and
     (isStructured(t) or isManaged(t) or v^.isConstRef) then
  begin
    if v^.isConstRef then
      exit('a ''constref'' parameter of ' + ofS);
    exit('a ''const'' parameter of ' + ofS);
  end;
end;

{ ...

  A closure (a lambda made a reference to value) may outlive them: their
  frames it uses are then kept on the heap (heapFrame), and it must not use
  what does not live in a frame (closureCannotUse). }
{ The innermost spawn block around the code being checked, if r (a
  routine; nil: the program) is outside it: the task that would share r's
  variables. nil otherwise. }
function spawnOutside(r: PDecl): PDecl;
var
  x: PDecl;
begin
  spawnOutside := nil;
  x := curRoutine;
  while x <> nil do
  begin
    if x = r then
      exit;
    if x^.isSpawnBlock then
      exit(x);
    x := x^.parent;
  end;
end;

{ d (used through w) in a spawn block: an isolated variable of another task
  is an error; under $modeswitch racechecks, so is anything mutable that is
  not shared: a variable of the routines around it or of the program,
  Self's members (unless its class is thread-safe), a routine nested in
  another (which may use its variables). }
procedure noteTaskUse(d: PDecl; w: PWithContext; const id: TIdent);
var
  s: PDecl;
  ct: PType;
  isSelfVar, readOnly: boolean;
begin
  if (d = nil) or (curRoutine = nil) then
    exit;
  if w <> nil then
  begin
    if w^.routine = nil then
      exit;
    s := spawnOutside(w^.routine);
    if (s = nil) or not s^.isRaceChecked then
      exit;
    if w^.isSelf then
    begin
      ct := w^.routine^.ownerClass;
      if ((ct <> nil) and isThreadSafe(ct)) or
         ((d^.kind = dkField) and (d^.race = rqShared)) then
        exit;
      ms('this spawned task uses ''');
      maN(id.spelling);
      ma(''' of Self, which other tasks may use at the same time; make it a ' +
         'shared field, or derive the class from TSharedObject');
      errR(id.loc, identRange(id));
    end
    else
    begin
      ms('this spawned task uses ''');
      maN(id.spelling);
      ma(''' through a ''with'' statement outside it, which other tasks may ' +
         'change at the same time');
      errR(id.loc, identRange(id));
    end;
    exit;
  end;
  if d^.kind = dkVar then
  begin
    s := spawnOutside(d^.owner);
    if s = nil then
      exit;
    if d^.race = rqIsolated then
    begin
      ms('isolated variable ''');
      maN(id.spelling);
      ma(''' belongs to one task; a spawned task cannot use it (hand it to an ' +
         'async routine with ''move ');
      maN(id.spelling);
      ma(''')');
      errR(id.loc, identRange(id));
      noteDeclaredHere(d);
      exit;
    end;
    if not s^.isRaceChecked or (d^.race = rqShared) or isError(d^.varType) then
      exit;
    isSelfVar := (d^.owner <> nil) and (d = d^.owner^.selfVar);
    readOnly := isSelfVar or d^.isReadOnly or
                ((d^.varKind = vkConstParam) and not d^.isConstRef);
    if readOnly and not sharesMutableData(d^.varType) then
      exit;
    ms('this spawned task uses ''');
    maN(id.spelling);
    ma(''', which is not shared: another task may change it at the same time; ' +
       'declare it shared (of a thread-safe type), or pass its value to an ' +
       'async routine');
    errR(id.loc, identRange(id));
    noteDeclaredHere(d);
    exit;
  end;
  if d^.kind = dkRoutine then
  begin
    if (d^.parent = nil) or d^.isLambda or d^.isSpawnBlock then
      exit;
    s := spawnOutside(d^.parent);
    if (s <> nil) and s^.isRaceChecked then
    begin
      ms('this spawned task calls ''');
      maN(id.spelling);
      ma(''', a routine nested in ''');
      maN(d^.parent^.ident.spelling);
      ma(''', which may use that routine''s variables while other tasks change them');
      errR(id.loc, identRange(id));
    end;
  end;
end;

{ e an isolated variable (by name): it cannot be copied, only moved. }
procedure checkIsolatedCopy(e: PNode; const where: string);
var
  v: PDecl;
begin
  while e^.kind = nkParen do
    e := e^.inner;
  if e^.kind <> nkNameRef then
    exit;
  v := e^.refDecl;
  if (v = nil) or (v^.kind <> dkVar) or (v^.race <> rqIsolated) then
    exit;
  ms('isolated variable ''');
  maN(e^.ref.spelling);
  ma(''' cannot be copied (' + where + '): its data belongs to one task; ' +
     'write ''move ');
  maN(e^.ref.spelling);
  ma(''' to hand it over');
  errR(e^.ref.loc, nodeRange(e));
end;

procedure noteLambdaUse(d: PDecl; w: PWithContext; const id: TIdent);
var
  owner, l, f, v, nested: PDecl;
  why: string;
begin
  noteTaskUse(d, w, id);
  if (curRoutine = nil) or not curRoutine^.isLambda or (d = nil) then
    exit;
  owner := nil;
  v := nil;
  nested := nil;
  if w <> nil then
  begin
    if w^.routine = curRoutine then
      exit;
    if not w^.isSelf then
    begin
      { its address is the enclosing routine's to compute, not the
        lambda's }
      ms('this lambda uses ''');
      maN(id.spelling);
      ma(''' through a ''with'' statement outside it; name the record instead');
      errR(id.loc, identRange(id));
      exit;
    end;
    owner := w^.routine;
    if owner <> nil then
      v := owner^.selfVar;
  end
  else if d^.kind = dkVar then
  begin
    if (d^.owner = nil) or (d^.owner = curRoutine) or
       ((d^.init <> nil) and not d^.isInitVar) or d^.isClassVar then
      exit;
    owner := d^.owner;
    v := d;
  end
  else if d^.kind = dkRoutine then
  begin
    if (d^.parent = nil) or (d = curRoutine) then
      exit;
    owner := d^.parent;
    nested := d;
  end
  else
    exit;
  l := curRoutine;
  while (l <> nil) and (l <> owner) do
  begin
    if l^.isMethodLambda then { called on its object, without a frame }
    begin
      ms('this method-pointer lambda uses ''');
      maN(id.spelling);
      ma(''' of the routine around it; it can use only its object''s members, ' +
         'its parameters and what is declared at program or unit level');
      errR(id.loc, identRange(id));
      exit;
    end;
    if l^.isLambda then
    begin
      if not identValid(l^.lambdaCapture) then
        l^.lambdaCapture := id;
      if l^.isClosure then
      begin
        if nested <> nil then
          if not nested^.isLambda then
          begin
            ms('this closure uses ''');
            maN(id.spelling);
            ma(''', a routine nested in ''');
            maN(owner^.ident.spelling);
            ma('''; a closure can only use routines declared at program or ' +
               'unit level');
            errR(id.loc, identRange(id));
            exit;
          end;
        why := '';
        if v <> nil then
          why := closureCannotUse(v);
        if why <> '' then
        begin
          ms('this closure uses ''');
          maN(id.spelling);
          ma(''', ' + why + ', which may not exist when the closure runs; ' +
             'copy it to a local variable first');
          errR(id.loc, identRange(id));
          exit;
        end;
        f := l^.parent;
        while f <> nil do
        begin
          f^.heapFrame := true;
          if f = owner then
            break;
          f := f^.parent;
        end;
      end;
    end;
    l := l^.parent;
  end;
end;

{ A lambda that uses what belongs to the routines around it must not
  outlive them: reported if value is one and is stored (local: in a local
  variable of the current routine, which is allowed). }
procedure checkLambdaEscape(value: PNode; local: boolean);
var
  r: PDecl;
begin
  if value^.kind <> nkAddrOf then
    exit;
  r := value^.addrLambda;
  if (r = nil) or local then
    exit;
  if r^.isClosure then
    exit;
  if not identValid(r^.lambdaCapture) then
    exit;
  ms('this lambda uses ''');
  maN(r^.lambdaCapture.spelling);
  ma(''' of the routine it is written in, so it must not outlive it: it ' +
     'can be passed as an argument or kept in a local variable, not stored ' +
     'here');
  errR(value^.b, nodeRange(value));
end;

procedure checkRoutineArg(arg: PNode; p: PDecl; formal: PType);
var
  kind: string;
  d, x: PDecl;
  params: TPtrList;
  res, xr: PType;
  found, cdeclConv: boolean;
  n: PNode;
  why: string;
  ofT, savedTarget: PType;
  la: PDecl;
begin
  la := nil;
  if arg^.kind = nkAddrOf then { a lambda: its types }
  begin
    la := arg^.addrLambda;
    completeLambda(arg, formal);
    if la <> nil then
      if la^.lambdaBad then
        exit;
  end;
  { @obj.M, or a method pointer; a closure (any value of the type) }
  if formal^.routineOfObject or formal^.routineReference then
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
    else if (p^.varKind = vkVarParam) and isBitPackedComponent(arg) then
      diagBitFieldAddress(arg)
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
  if not isISO and (arg^.kind = nkAddrOf) and (arg^.inner^.kind = nkField) then
  begin
    { @TFoo.M (static) }
    savedTarget := addrOfTarget;
    addrOfTarget := formal;
    arg^.typ := nil;
    ofT := check(arg);
    addrOfTarget := savedTarget;
    arg^.typ := p^.varType;
    if isError(ofT) then
      exit;
    x := arg^.addrCode;
    if x = nil then
    begin
      ms('argument for ' + kind + ' parameter ''');
      maN(p^.ident.spelling);
      ma(''' must be a ' + kind + ' or a static class method');
      errR(arg^.b, nodeRange(arg));
      exit;
    end;
    xr := nil;
    if x^.isFunction then
      xr := x^.resultType;
    if (isCDecl(x) <> formal^.routineCDecl) or
       not congruent(formal^.routineParams, formal^.routineResult, x^.params, xr) then
    begin
      ms('''');
      maN(arg^.inner^.fieldName.spelling);
      ma(''' does not match the heading of parameter ''');
      maN(p^.ident.spelling);
      ma(''' (');
      maT(formal);
      ma(')');
      errR(arg^.b, nodeRange(arg));
    end;
    exit;
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
    if la <> nil then
      ms('this lambda')
    else
    begin
      ms('''');
      maN(n^.ref.spelling);
      ma('''');
    end;
    ma(' does not match the heading of parameter ''');
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
    if isBitPackedComponent(target) then
      diagBitFieldAddress(target)
    else if not isVariableReference(target) then
      mustBeVariable
    else
      checkModifiable(target);
  end;
end;

procedure checkCallArgsOnly(const params: TPtrList; calleeDecl: PDecl;
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
  if argsFailed then { named arguments that do not fit: reported }
  begin
    argsFailed := false;
    for i := 0 to args.count - 1 do
      if argValue(PNode(listGet(args, i)))^.kind <> nkFormatted then
        check(argValue(PNode(listGet(args, i))));
    exit;
  end;
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
    if (pt <> nil) and ((arg^.kind = nkAddrOf) or (arg^.kind = nkTuple) or
                        ((pt^.kind = tyDynArray) and (arg^.kind = nkSetCtor))) then
      at := checkValueFor(pt, arg)
    else
      at := check(arg);
    if isError(at) or (pt = nil) or isError(pt) then
      continue;
    if p^.varKind = vkVarParam then
    begin
      if isBitPackedComponent(arg) then
      begin
        diagBitFieldAddress(arg);
        continue;
      end;
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
      begin
        checkNotForControlVar(arg);
        nullVarArg(arg, p);
      end;
      continue;
    end;
    checkWideToChar(pt, arg);
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
    end
    else
    begin
      if p^.isConstRef and (at = pt) and
         ((arg^.convOp = nil) or arg^.convSuspended) and
         isConstRefAddressable(arg) then
        arg^.constRefAddr := true; { its address (else a temporary's) }
      if nullSafeHere and isNullRef(pt) and declNonNull(p) then
        requireNotNil(arg, nwParam, p^.ident.spelling, pt);
    end;
  end;
end;

procedure checkCallArgs(const params: TPtrList; calleeDecl: PDecl;
                        const callee: TIdent; const args: TPtrList;
                        callRange: TRange);
var
  i: longint;
begin
  checkCallArgsOnly(params, calleeDecl, callee, args, callRange);
  if (calleeDecl <> nil) and (calleeDecl^.kind = dkRoutine) and
     ((calleeDecl^.asyncType <> nil) or (calleeDecl^.iterType <> nil)) then { (copies, which it keeps) }
    for i := 0 to args.count - 1 do
      checkIsolatedCopy(PNode(listGet(args, i)), 'passed to a task');
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
    { the definitions of its generic routines, which the units and the
      program using it specialize; they see the unit's interface only }
    impl := newScope(iface);
    importUnits(impl, u^.implUses);
    cur := impl;
    curUnitInterface := iface;
    for i := 0 to u^.impl^.decls.count - 1 do
    begin
      r := PDecl(listGet(u^.impl^.decls, i));
      if (r^.kind = dkRoutine) and r^.isGeneric then
        declareRoutine(r);
    end;
    curUnitInterface := nil;
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
    if (r^.kind = dkRoutine) and isUndefinedHeading(r) and not r^.isExternal then
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
  checkInstances(u^.impl);
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
  compoundResultRef := nil;
  compoundResultVar := nil;
  tobjectType := nil;
  exceptionType := nil;
  iunknownType := nil;
  interfacedType := nil;
  namedAdvancedRecord := nil;
  constInitWhat := 'typed constant value';
  nestPrefix := nil;
  listInit(instances);
  listInit(pendingInstances);
  checkingInstance := -1;
  listInit(moduleInstances);
  handlerDepth := 0;
  finallyLoopDepth := -1;
  nullSafeHere := false;
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

function standardNames(m: TLangMode): TPtrList;
var
  names: TPtrList;
begin
  semaInit(m);
  listInit(names);
  scopeNames(standard, names);
  standardNames := names;
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
  checkInstances(prog^.progBlock);
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
