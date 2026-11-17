unit pctypes;
{ Semantic types of the self-hosted compiler: the predeclared types and the
  type constructors (the C++ ASTContext), and the predicates, bounds and
  names of src/ast/Type.cpp. TType itself is declared in pcast. }

interface

uses pcsys, pcutil, pcdiag, pcast;

type
  TOrdinalBounds = record
    lo, hi: int64;
  end;

var
  { predeclared types (singletons) }
  tyErrorT, tyIntegerT, tyInt64T, tyCardinalT, tyByteT, tyWordT,
  tyShortIntT, tySmallIntT, tyRealT, tyBooleanT, tyCharT, tyNilT,
  tyEmptySetT, tyTextT, tyUntypedPointerT: PType;
  { unsigned 64-bit (FPC's qword): values are kept as their 64 bits (in
    an int64), so code that compares or prints them reads them unsigned
    (see isQWord) }
  tyQWordT: PType;
  { the type of untyped parameters (var x; const x): usable only through
    typecasts, @, and as arguments for untyped parameters }
  tyUntypedT: PType;
  { widechar (and unicodechar): a 16-bit character code }
  tyWideCharT: PType;

procedure initTypes;
function newType(kind: TTypeKind): PType;
function newIntegerType(bits: longint; signed: boolean; const name: string): PType;
function newEnumType: PType;
function newSubrangeType(host: PType; lo, hi: int64): PType;
function newArrayType(index, element: PType; packed_: boolean): PType;
function newRecordType(fl: PFieldList; const all: TPtrList; packed_: boolean): PType;
function newPointerType(pointee: PType): PType;
function newSetType(element: PType): PType;
function newFileType(element: PType): PType;
function newRoutineType(const params: TPtrList; result_: PType;
                        cdecl_: boolean): PType;
{ A class (objfpc), not defined yet: defineClass fills it in. }
function newClassType: PType;
function newClassRefType(c: PType): PType;
function newInterfaceType: PType;
procedure defineInterface(t, parentIntf: PType; const methods, allMethods: TPtrList;
                          module: pointer);
{ is interface i a or a descendant of a? }
function interfaceInheritsFrom(i, a: PType): boolean;
{ the interface implementation of class c for interface i, or nil }
function findInterface(c, i: PType): PInterfaceImpl;
procedure defineClass(t, parentClass: PType; const members, allFields,
                      vtable: TPtrList; module: pointer);
{ shared by element / length (all short string types are compatible) }
function setConstructorType(element: PType): PType;
function stringLiteralType(len: int64): PType;
function shortStringType(maxLen: longint): PType;
function ansiStringType: PType;
function openArrayType(element: PType): PType;
function dynArrayType(element: PType): PType;
{ async T (result nil: async), one for each; TCancelToken }
function taskType(result_: PType): PType;
function iteratorType(element: PType): PType;
function tokenType: PType;
{ The type of a method pointer: procedure ... of object. }
function newMethodPointerType(const params: TPtrList; result_: PType; cdecl_: boolean): PType;

{ The first name given to t in a type declaration. }
procedure setDeclName(t: PType; name: PName);
{ integer, real, boolean, char, nil and the error type: never renamed }
function isBuiltinType(t: PType): boolean;

function isOrdinal(t: PType): boolean;
{ The bits an ordinal type t takes in a bitpacked record or array, as in
  Free Pascal: an integer type's size, 1 for boolean, 8 for char, enough for
  the largest value of an enumeration or a subrange (two's complement for a
  subrange with negative values). }
function bitPackedWidth(t: PType): longint;
{ Are values of ordinal type t sign-extended from bitPackedWidth bits? }
function bitPackedSigned(t: PType): boolean;
{ a bitpacked array of ordinals: its elements take bitPackedWidth bits
  each, without addresses }
function hasBitElements(t: PType): boolean;
{ Is e (checked) an ordinal field or element of a bitpacked record or array
  (Free Pascal): a part of a byte, assignable but without an address? }
function isBitPackedComponent(e: PNode): boolean;
function isSimple(t: PType): boolean;
function isValidResultType(t: PType): boolean;
{ A reference to procedural type (objfpc)? }
function isClosureType(t: PType): boolean;
function isStructured(t: PType): boolean;
function getBase(t: PType): PType;
function isIntegerLike(t: PType): boolean;
function isSignedIntegerLike(t: PType): boolean;
function isNumeric(t: PType): boolean;
{ char or widechar, after stripping subranges }
function isCharLike(t: PType): boolean;
function isStringType(t: PType): boolean;
function getStringLength(t: PType): int64;
function isStringish(t: PType): boolean;
function isTextFile(t: PType): boolean;
function isEmptySet(t: PType): boolean;
{ The bits a value of set type t takes (bit i is member i): 32 when the
  element type's values are at most 31 (as in Free Pascal, and for the
  empty set), 64 when at most 63, else 256. }
function setStorageBits(t: PType): longint;
function isUntypedPointer(t: PType): boolean;
function containsFile(t: PType): boolean;
{ Is t, or does it contain, a procedural type? }
function containsRoutineType(t: PType): boolean;
{ Is t, or does it contain, a COM interface (reference counted)? }
function isManaged(t: PType): boolean;
{ A procedure or function ... of object? }
function isMethodPointer(t: PType): boolean;
{ Is t, or does it contain, a COM interface? }
function containsInterface(t: PType): boolean;
function findField(rec: PType; name: PName): PDecl;
{ Class members (objfpc): declared in class c itself; in c or its nearest
  ancestor declaring it (owner receives that class); is c a or a
  descendant of a? }
function findOwnMember(c: PType; name: PName): PDecl;
function findMember(c: PType; name: PName; var owner: PType): PDecl;
function inheritsFrom(c, a: PType): boolean;
function getOrdinalBounds(t: PType): TOrdinalBounds;
{ The largest alignment of record t's fields: 1 for a packed record, N
  under $packrecords N (as in Free Pascal); 0: the natural (C) layout. }
function fieldAlignment(t: PType): longint;
{ Is t qword (unsigned 64-bit), not a subrange of it? getOrdinalBounds
  gives it 0..high(int64): callers that need its whole range test this. }
function isQWord(t: PType): boolean;
{ All integer types count as one host (Turbo/FPC). }
function haveSameOrdinalHost(a, b: PType): boolean;
{ Do L and R compare as short strings (Turbo/FPC)? }
function needsStringCompare(l, r: PType): boolean;

{ Type::getName, appended to b. }
procedure bufAddTypeName(var b: TBuf; t: PType);
{ A value of ordinal type t as in diagnostics: 'c', chr(n), true, red, 42. }
procedure bufAddOrdinal(var b: TBuf; t: PType; v: int64);

implementation

var
  nextTypeId: longint;
  setTypes, stringLitTypes, shortStringTypes: TIntMap;
  theAnsiString: PType;
  openArrayTypes, dynArrayTypes: TPtrList; { one for each element type }
  taskTypes: TPtrList; { one for each result type }
  iterTypes: TPtrList; { one for each element type }
  theToken: PType;

function newType(kind: TTypeKind): PType;
var
  t: PType;
begin
  t := PType(arenaAlloc(sizeof(TType)));
  t^.kind := kind;
  nextTypeId := nextTypeId + 1;
  t^.id := nextTypeId;
  t^.declName := nil;
  newType := t;
end;

function newIntegerType(bits: longint; signed: boolean; const name: string): PType;
var
  t: PType;
begin
  t := newType(tyInteger);
  t^.bits := bits;
  t^.isSigned := signed;
  t^.intName := intern(name);
  newIntegerType := t;
end;

function newEnumType: PType;
var
  t: PType;
begin
  t := newType(tyEnum);
  listInit(t^.enumValues);
  newEnumType := t;
end;

function newSubrangeType(host: PType; lo, hi: int64): PType;
var
  t: PType;
begin
  t := newType(tySubrange);
  t^.host := host;
  t^.lo := lo;
  t^.hi := hi;
  newSubrangeType := t;
end;

function newArrayType(index, element: PType; packed_: boolean): PType;
var
  t: PType;
begin
  t := newType(tyArray);
  t^.indexType := index;
  t^.elementType := element;
  t^.isPackedArray := packed_;
  newArrayType := t;
end;

function newRecordType(fl: PFieldList; const all: TPtrList; packed_: boolean): PType;
var
  t: PType;
begin
  t := newType(tyRecord);
  t^.fieldList := fl;
  t^.allFields := all;
  t^.isPackedRecord := packed_;
  t^.isTupleRecord := false;
  newRecordType := t;
end;

function newPointerType(pointee: PType): PType;
var
  t: PType;
begin
  t := newType(tyPointer);
  t^.pointee := pointee;
  t^.untyped := false;
  newPointerType := t;
end;

function newSetType(element: PType): PType;
var
  t: PType;
begin
  t := newType(tySet);
  t^.setElement := element;
  newSetType := t;
end;

function newFileType(element: PType): PType;
var
  t: PType;
begin
  t := newType(tyFile);
  t^.fileElement := element;
  t^.isText := false;
  newFileType := t;
end;

function newRoutineType(const params: TPtrList; result_: PType;
                        cdecl_: boolean): PType;
var
  t: PType;
begin
  t := newType(tyRoutine);
  t^.routineParams := params;
  t^.routineResult := result_;
  t^.routineCDecl := cdecl_;
  t^.routineOfObject := false;
  t^.routineReference := false;
  newRoutineType := t;
end;

function newClassType: PType;
var
  t: PType;
begin
  t := newType(tyClass);
  t^.classParent := nil;
  listInit(t^.classMembers);
  listInit(t^.classAllFields);
  listInit(t^.classVTable);
  listInit(t^.classInterfaces);
  t^.classModule := nil;
  t^.classDefined := false;
  newClassType := t;
end;

function newClassRefType(c: PType): PType;
var
  t: PType;
begin
  t := newType(tyClassRef);
  t^.refClass := c;
  newClassRefType := t;
end;

function newInterfaceType: PType;
var
  t: PType;
begin
  t := newType(tyInterface);
  t^.intfParent := nil;
  listInit(t^.intfMethods);
  listInit(t^.intfAllMethods);
  t^.intfModule := nil;
  t^.intfDefined := false;
  t^.intfCOM := false;
  newInterfaceType := t;
end;

procedure defineInterface(t, parentIntf: PType; const methods, allMethods: TPtrList;
                          module: pointer);
begin
  t^.intfParent := parentIntf;
  t^.intfMethods := methods;
  t^.intfAllMethods := allMethods;
  t^.intfModule := module;
  t^.intfDefined := true;
end;

function interfaceInheritsFrom(i, a: PType): boolean;
begin
  interfaceInheritsFrom := false;
  while i <> nil do
  begin
    if i = a then
      exit(true);
    i := i^.intfParent;
  end;
end;

function findInterface(c, i: PType): PInterfaceImpl;
var
  k: longint;
  x: PInterfaceImpl;
begin
  findInterface := nil;
  for k := 0 to c^.classInterfaces.count - 1 do
  begin
    x := PInterfaceImpl(listGet(c^.classInterfaces, k));
    if x^.intf = i then
      exit(x);
  end;
end;

procedure defineClass(t, parentClass: PType; const members, allFields,
                      vtable: TPtrList; module: pointer);
begin
  t^.classParent := parentClass;
  t^.classMembers := members;
  t^.classAllFields := allFields;
  t^.classVTable := vtable;
  t^.classModule := module;
  t^.classDefined := true;
end;

procedure initTypes;
begin
  mapInit(setTypes);
  mapInit(stringLitTypes);
  mapInit(shortStringTypes);
  theAnsiString := newType(tyAnsiString);
  listInit(openArrayTypes);
  listInit(dynArrayTypes);
  listInit(taskTypes);
  listInit(iterTypes);
  theToken := nil;
  tyErrorT := newType(tyError);
  tyIntegerT := newIntegerType(32, true, 'integer');
  tyInt64T := newIntegerType(64, true, 'int64');
  tyCardinalT := newIntegerType(32, false, 'cardinal');
  tyByteT := newIntegerType(8, false, 'byte');
  tyWordT := newIntegerType(16, false, 'word');
  tyShortIntT := newIntegerType(8, true, 'shortint');
  tySmallIntT := newIntegerType(16, true, 'smallint');
  tyQWordT := newIntegerType(64, false, 'qword');
  tyRealT := newType(tyReal);
  tyBooleanT := newType(tyBoolean);
  tyCharT := newType(tyChar);
  tyWideCharT := newType(tyWideChar);
  tyNilT := newType(tyNil);
  tyUntypedT := newType(tyUntyped);
  tyEmptySetT := newSetType(nil);
  tyTextT := newFileType(tyCharT);
  tyTextT^.isText := true;
  tyUntypedPointerT := newPointerType(nil);
  tyUntypedPointerT^.untyped := true;
end;

function setConstructorType(element: PType): PType;
var
  t: PType;
begin
  if element = nil then
    exit(tyEmptySetT);
  t := PType(mapGet(setTypes, element^.id));
  if t = nil then
  begin
    t := newSetType(element);
    mapPut(setTypes, element^.id, t);
  end;
  setConstructorType := t;
end;

function stringLiteralType(len: int64): PType;
var
  t: PType;
begin
  t := PType(mapGet(stringLitTypes, len));
  if t = nil then
  begin
    t := newType(tyStringLit);
    t^.strLength := len;
    mapPut(stringLitTypes, len, t);
  end;
  stringLiteralType := t;
end;

function shortStringType(maxLen: longint): PType;
var
  t: PType;
begin
  t := PType(mapGet(shortStringTypes, maxLen));
  if t = nil then
  begin
    t := newType(tyShortString);
    t^.maxLength := maxLen;
    mapPut(shortStringTypes, maxLen, t);
  end;
  shortStringType := t;
end;

function newMethodPointerType(const params: TPtrList; result_: PType; cdecl_: boolean): PType;
var
  t: PType;
begin
  t := newRoutineType(params, result_, cdecl_);
  t^.routineOfObject := true;
  newMethodPointerType := t;
end;

function ansiStringType: PType;
begin
  ansiStringType := theAnsiString;
end;

function dynArrayType(element: PType): PType;
var
  i: longint;
  t: PType;
begin
  for i := 0 to dynArrayTypes.count - 1 do
  begin
    t := PType(listGet(dynArrayTypes, i));
    if t^.dynElement = element then
      exit(t);
  end;
  t := newType(tyDynArray);
  t^.dynElement := element;
  listAdd(dynArrayTypes, t);
  dynArrayType := t;
end;

function taskType(result_: PType): PType;
var
  i: longint;
  t: PType;
begin
  for i := 0 to taskTypes.count - 1 do
  begin
    t := PType(listGet(taskTypes, i));
    if t^.taskResult = result_ then
      exit(t);
  end;
  t := newType(tyTask);
  t^.taskResult := result_;
  listAdd(taskTypes, t);
  taskType := t;
end;

function iteratorType(element: PType): PType;
var
  i: longint;
  t: PType;
begin
  for i := 0 to iterTypes.count - 1 do
  begin
    t := PType(listGet(iterTypes, i));
    if t^.iterElement = element then
      exit(t);
  end;
  t := newType(tyIterator);
  t^.iterElement := element;
  listAdd(iterTypes, t);
  iteratorType := t;
end;

function tokenType: PType;
begin
  if theToken = nil then
    theToken := newType(tyToken);
  tokenType := theToken;
end;

function openArrayType(element: PType): PType;
var
  i: longint;
  t: PType;
begin
  for i := 0 to openArrayTypes.count - 1 do
  begin
    t := PType(listGet(openArrayTypes, i));
    if t^.openElement = element then
      exit(t);
  end;
  t := newType(tyOpenArray);
  t^.openElement := element;
  listAdd(openArrayTypes, t);
  openArrayType := t;
end;

procedure setDeclName(t: PType; name: PName);
begin
  if t^.declName = nil then
    t^.declName := name;
end;

function isBuiltinType(t: PType): boolean;
begin
  isBuiltinType := t^.kind in [tyError, tyInteger, tyReal, tyBoolean, tyChar, tyNil,
                               tyUntyped, tyWideChar];
end;

function isOrdinal(t: PType): boolean;
begin
  isOrdinal := t^.kind in [tyInteger, tyBoolean, tyChar, tyEnum, tySubrange,
                           tyWideChar];
end;

function bitLength(v: int64): longint;
var
  n: longint;
begin
  n := 0;
  while v <> 0 do
  begin
    n := n + 1;
    v := v shr 1;
  end;
  bitLength := n;
end;

function maxOf(a, b: longint): longint;
begin
  if a > b then
    maxOf := a
  else
    maxOf := b;
end;

function bitPackedWidth(t: PType): longint;
var
  n, neg, pos: longint;
begin
  case t^.kind of
    tyInteger: bitPackedWidth := t^.bits;
    tyBoolean: bitPackedWidth := 1;
    tyChar: bitPackedWidth := 8;
    tyWideChar: bitPackedWidth := 16;
    tyEnum:
      begin
        n := t^.enumValues.count;
        if n > 0 then
          n := n - 1;
        bitPackedWidth := maxOf(1, bitLength(n));
      end;
    tySubrange:
      if t^.lo >= 0 then
        bitPackedWidth := maxOf(1, bitLength(t^.hi))
      else
      begin
        neg := bitLength(-(t^.lo + 1));
        pos := 0;
        if t^.hi > 0 then
          pos := bitLength(t^.hi);
        begin
          pos := maxOf(neg, pos) + 1;
          if pos > 64 then
            pos := 64;
          bitPackedWidth := pos;
        end;
      end;
  else
    bitPackedWidth := 0;
  end;
end;

function bitPackedSigned(t: PType): boolean;
begin
  if t^.kind = tyInteger then
    bitPackedSigned := t^.isSigned
  else if t^.kind = tySubrange then
    bitPackedSigned := t^.lo < 0
  else
    bitPackedSigned := false;
end;

function hasBitElements(t: PType): boolean;
begin
  hasBitElements := (t <> nil) and (t^.kind = tyArray) and t^.isBitPackedArray and
                    isOrdinal(t^.elementType);
end;

function isBitPackedComponent(e: PNode): boolean;
var
  f: PDecl;
  rt: PType;
begin
  if e^.kind = nkIndex then
    exit((e^.idxProp = nil) and (e^.base^.typ <> nil) and hasBitElements(e^.base^.typ));
  f := nil;
  rt := nil;
  if e^.kind = nkField then
  begin
    f := e^.fieldDecl;
    rt := e^.base^.typ;
  end
  else if (e^.kind = nkNameRef) and (e^.refWith <> nil) then
  begin
    f := e^.refDecl;
    rt := e^.refWith^.rec^.typ;
  end;
  isBitPackedComponent := (f <> nil) and (f^.kind = dkField) and (rt <> nil) and
                          (rt^.kind = tyRecord) and rt^.isBitPackedRecord and
                          (f^.fieldType <> nil) and isOrdinal(f^.fieldType);
end;

function isSimple(t: PType): boolean;
begin
  isSimple := isOrdinal(t) or (t^.kind = tyReal);
end;

function isValidResultType(t: PType): boolean;
begin
  isValidResultType := isSimple(t) or
                       (t^.kind in [tyPointer, tyClass, tyClassRef, tyInterface, tyAnsiString,
                                    tyDynArray]) or isClosureType(t) or
                       (t^.kind in [tyTask, tyToken, tyIterator]);
end;

function isClosureType(t: PType): boolean;
begin
  isClosureType := (t^.kind = tyRoutine) and t^.routineReference;
end;

function isStructured(t: PType): boolean;
begin
  isStructured := t^.kind in [tyArray, tyRecord, tyShortString];
end;

function getBase(t: PType): PType;
begin
  while t^.kind = tySubrange do
    t := t^.host;
  getBase := t;
end;

function isIntegerLike(t: PType): boolean;
begin
  isIntegerLike := getBase(t)^.kind = tyInteger;
end;

function isSignedIntegerLike(t: PType): boolean;
var
  b: PType;
begin
  b := getBase(t);
  isSignedIntegerLike := (b^.kind = tyInteger) and b^.isSigned;
end;

function isNumeric(t: PType): boolean;
begin
  isNumeric := isIntegerLike(t) or (t^.kind = tyReal);
end;

function isCharLike(t: PType): boolean;
begin
  isCharLike := getBase(t)^.kind in [tyChar, tyWideChar];
end;

{ a string literal, or packed array [1..n] of char }
function isStringType(t: PType): boolean;
var
  idx: PType;
begin
  if t^.kind = tyStringLit then
    exit(true);
  if (t^.kind <> tyArray) or not t^.isPackedArray or
     (t^.elementType^.kind <> tyChar) then
    exit(false);
  idx := t^.indexType;
  isStringType := (idx^.kind = tySubrange) and (idx^.host^.kind = tyInteger) and
                  (idx^.lo = 1) and (idx^.hi >= 1);
end;

function getStringLength(t: PType): int64;
begin
  if t^.kind = tyStringLit then
    getStringLength := t^.strLength
  else
    getStringLength := t^.indexType^.hi;
end;

function isStringish(t: PType): boolean;
begin
  isStringish := (t^.kind in [tyShortString, tyAnsiString]) or isStringType(t) or
                 isCharLike(t);
end;

function isTextFile(t: PType): boolean;
begin
  isTextFile := (t^.kind = tyFile) and t^.isText;
end;

function isEmptySet(t: PType): boolean;
begin
  isEmptySet := (t^.kind = tySet) and (t^.setElement = nil);
end;

function setStorageBits(t: PType): longint;
var
  hi: int64;
begin
  if t^.setElement = nil then
    exit(32);
  hi := getOrdinalBounds(t^.setElement).hi;
  if hi <= 31 then
    setStorageBits := 32
  else if hi <= 63 then
    setStorageBits := 64
  else
    setStorageBits := 256;
end;

function isUntypedPointer(t: PType): boolean;
begin
  isUntypedPointer := (t^.kind = tyPointer) and t^.untyped;
end;

function containsRoutineType(t: PType): boolean;
var
  i: longint;
  f: PDecl;
begin
  case t^.kind of
    tyRoutine: containsRoutineType := true;
    tyArray: containsRoutineType := containsRoutineType(t^.elementType);
    tyRecord:
      begin
        for i := 0 to t^.allFields.count - 1 do
        begin
          f := PDecl(listGet(t^.allFields, i));
          if (f^.fieldType <> nil) and containsRoutineType(f^.fieldType) then
            exit(true);
        end;
        containsRoutineType := false;
      end;
  else
    containsRoutineType := false;
  end;
end;

function containsFile(t: PType): boolean;
var
  i: longint;
  f: PDecl;
begin
  case t^.kind of
    tyFile: containsFile := true;
    tyArray: containsFile := containsFile(t^.elementType);
    tyRecord:
      begin
        for i := 0 to t^.allFields.count - 1 do
        begin
          f := PDecl(listGet(t^.allFields, i));
          if (f^.fieldType <> nil) and containsFile(f^.fieldType) then
            exit(true);
        end;
        containsFile := false;
      end;
  else
    containsFile := false;
  end;
end;

function isMethodPointer(t: PType): boolean;
begin
  isMethodPointer := (t^.kind = tyRoutine) and t^.routineOfObject;
end;

function containsInterface(t: PType): boolean;
var
  i: longint;
  f: PDecl;
begin
  case t^.kind of
    tyInterface: containsInterface := t^.intfCOM;
    tyArray: containsInterface := containsInterface(t^.elementType);
    tyRecord:
      begin
        for i := 0 to t^.allFields.count - 1 do
        begin
          f := PDecl(listGet(t^.allFields, i));
          if (f^.fieldType <> nil) and containsInterface(f^.fieldType) then
            exit(true);
        end;
        containsInterface := false;
      end;
  else
    containsInterface := false;
  end;
end;

function isManaged(t: PType): boolean;
var
  i: longint;
  f: PDecl;
begin
  case t^.kind of
    tyInterface: isManaged := t^.intfCOM;
    tyAnsiString: isManaged := true;
    tyOpenArray: isManaged := isManaged(t^.openElement);
    tyDynArray: isManaged := true;
    tyTask, tyToken, tyIterator: isManaged := true;
    tyRoutine: isManaged := t^.routineReference;
    tyArray: isManaged := isManaged(t^.elementType);
    tyRecord:
      begin
        for i := 0 to t^.allFields.count - 1 do
        begin
          f := PDecl(listGet(t^.allFields, i));
          if (f^.fieldType <> nil) and isManaged(f^.fieldType) then
            exit(true);
        end;
        isManaged := false;
      end;
  else
    isManaged := false;
  end;
end;

function findField(rec: PType; name: PName): PDecl;
var
  i: longint;
  f: PDecl;
begin
  for i := 0 to rec^.allFields.count - 1 do
  begin
    f := PDecl(listGet(rec^.allFields, i));
    if f^.ident.name = name then
      exit(f);
  end;
  findField := nil;
end;

function findOwnMember(c: PType; name: PName): PDecl;
var
  i: longint;
  d: PDecl;
begin
  for i := 0 to c^.classMembers.count - 1 do
  begin
    d := PDecl(listGet(c^.classMembers, i));
    if (d^.kind = dkRoutine) and (d^.memberKey <> nil) then
    begin
      if d^.memberKey = name then { (found by its key only) }
        exit(d);
      continue;
    end;
    if d^.ident.name = name then
      exit(d);
  end;
  findOwnMember := nil;
end;

function findMember(c: PType; name: PName; var owner: PType): PDecl;
var
  d: PDecl;
begin
  while c <> nil do
  begin
    d := findOwnMember(c, name);
    if d <> nil then
    begin
      owner := c;
      exit(d);
    end;
    c := c^.classParent;
  end;
  findMember := nil;
end;

function inheritsFrom(c, a: PType): boolean;
begin
  while c <> nil do
  begin
    if c = a then
      exit(true);
    c := c^.classParent;
  end;
  inheritsFrom := false;
end;

function fieldAlignment(t: PType): longint;
begin
  if t^.isPackedRecord then
    fieldAlignment := 1
  else
    fieldAlignment := t^.recordPackRecords;
end;

function isQWord(t: PType): boolean;
begin
  isQWord := (t <> nil) and (t^.kind = tyInteger) and (t^.bits = 64) and
             not t^.isSigned;
end;

function getOrdinalBounds(t: PType): TOrdinalBounds;
var
  b: TOrdinalBounds;
begin
  case t^.kind of
    tyInteger:
      if t^.isSigned then
      begin
        if t^.bits = 64 then
        begin
          b.lo := low(int64);
          b.hi := high(int64);
        end
        else
        begin
          b.lo := -(int64(1) shl (t^.bits - 1));
          b.hi := (int64(1) shl (t^.bits - 1)) - 1;
        end;
      end
      else if t^.bits = 64 then { qword: its whole range does not fit }
      begin
        b.lo := 0;
        b.hi := high(int64);
      end
      else
      begin
        b.lo := 0;
        b.hi := (int64(1) shl t^.bits) - 1;
      end;
    tyChar:
      begin
        b.lo := 0;
        b.hi := 255;
      end;
    tyWideChar:
      begin
        b.lo := 0;
        b.hi := 65535;
      end;
    tyBoolean:
      begin
        b.lo := 0;
        b.hi := 1;
      end;
    tyEnum:
      begin
        b.lo := 0;
        b.hi := t^.enumValues.count - 1;
      end;
    tySubrange:
      begin
        b.lo := t^.lo;
        b.hi := t^.hi;
      end;
  else
    begin
      b.lo := 0;
      b.hi := -1;
    end;
  end;
  getOrdinalBounds := b;
end;

function haveSameOrdinalHost(a, b: PType): boolean;
var
  x, y: PType;
begin
  x := getBase(a);
  y := getBase(b);
  haveSameOrdinalHost := (x = y) or ((x^.kind = tyInteger) and (y^.kind = tyInteger)) or
                         (isCharLike(a) and isCharLike(b)); { char and widechar }
end;

function needsStringCompare(l, r: PType): boolean;
begin
  if not isStringish(l) or not isStringish(r) then
    exit(false);
  if isCharLike(l) and isCharLike(r) then
    exit(false); { compared as ordinals }
  if isStringType(l) and isStringType(r) and
     (getStringLength(l) = getStringLength(r)) then
    exit(false);
  needsStringCompare := true;
end;

procedure bufAddOrdinal(var b: TBuf; t: PType; v: int64);
var
  base: PType;
  c: char;
begin
  base := getBase(t);
  if (base^.kind = tyWideChar) and ((v > 126) or (v < 32)) then
  begin
    bufAdd(b, '#' + intToStr(v));
    exit;
  end;
  if base^.kind in [tyChar, tyWideChar] then
  begin
    c := chr(longint(v and 255));
    if (c >= ' ') and (c <= '~') then
    begin
      bufAddChar(b, '''');
      bufAddChar(b, c);
      bufAddChar(b, '''');
    end
    else
      bufAdd(b, 'chr(' + intToStr(v) + ')');
    exit;
  end;
  if base^.kind = tyBoolean then
  begin
    if v <> 0 then
      bufAdd(b, 'true')
    else
      bufAdd(b, 'false');
    exit;
  end;
  if (base^.kind = tyEnum) and (v >= 0) and (v < base^.enumValues.count) then
  begin
    bufAddName(b, PDecl(listGet(base^.enumValues, longint(v)))^.ident.spelling);
    exit;
  end;
  if isQWord(base) then
    bufAdd(b, uintToStr(v))
  else
    bufAddInt(b, v);
end;

procedure bufAddTypeName(var b: TBuf; t: PType);
var
  i: longint;
  p: PDecl;
begin
  if t^.declName <> nil then
  begin
    bufAddName(b, t^.declName);
    exit;
  end;
  case t^.kind of
    tyError: bufAdd(b, '<error>');
    tyInteger: bufAddName(b, t^.intName);
    tyReal: bufAdd(b, 'real');
    tyBoolean: bufAdd(b, 'boolean');
    tyChar: bufAdd(b, 'char');
    tyWideChar: bufAdd(b, 'widechar');
    tyTask:
      if t^.taskResult <> nil then
      begin
        bufAdd(b, 'async ');
        bufAddTypeName(b, t^.taskResult);
      end
      else
        bufAdd(b, 'async');
    tyToken: bufAdd(b, 'TCancelToken');
    tyIterator:
      begin
        bufAdd(b, 'iterator ');
        bufAddTypeName(b, t^.iterElement);
      end;
    tyStringLit: bufAdd(b, 'string of length ' + intToStr(t^.strLength));
    tyEnum:
      begin
        bufAddChar(b, '(');
        for i := 0 to t^.enumValues.count - 1 do
        begin
          if i > 0 then
            bufAdd(b, ', ');
          bufAddName(b, PDecl(listGet(t^.enumValues, i))^.ident.spelling);
        end;
        bufAddChar(b, ')');
      end;
    tySubrange:
      begin
        bufAddOrdinal(b, t^.host, t^.lo);
        bufAdd(b, '..');
        bufAddOrdinal(b, t^.host, t^.hi);
      end;
    tyArray:
      begin
        if t^.isBitPackedArray then
          bufAdd(b, 'bitpacked ')
        else if t^.isPackedArray then
          bufAdd(b, 'packed ');
        bufAdd(b, 'array [');
        bufAddTypeName(b, t^.indexType);
        bufAdd(b, '] of ');
        bufAddTypeName(b, t^.elementType);
      end;
    tyRecord:
      if t^.isTupleRecord then { (integer, Max: real) }
      begin
        bufAddChar(b, '(');
        for i := 0 to t^.allFields.count - 1 do
        begin
          if i > 0 then
            bufAdd(b, ', ');
          if nameStr(PDecl(listGet(t^.allFields, i))^.ident.name) <>
             'item' + intToStr(i + 1) then
          begin
            bufAddName(b, PDecl(listGet(t^.allFields, i))^.ident.spelling);
            bufAdd(b, ': ');
          end;
          if PDecl(listGet(t^.allFields, i))^.fieldType <> nil then
            bufAddTypeName(b, PDecl(listGet(t^.allFields, i))^.fieldType)
          else
            bufAddChar(b, '?');
        end;
        bufAddChar(b, ')');
      end
      else if t^.isBitPackedRecord then
        bufAdd(b, 'bitpacked record')
      else if t^.isPackedRecord then
        bufAdd(b, 'packed record')
      else
        bufAdd(b, 'record');
    tyPointer:
      if t^.untyped then
        bufAdd(b, 'pointer')
      else
      begin
        bufAddChar(b, '^');
        if t^.pointee <> nil then
          bufAddTypeName(b, t^.pointee)
        else
          bufAddChar(b, '?');
      end;
    tyNil: bufAdd(b, 'nil');
    tyUntyped: bufAdd(b, 'untyped');
    tyClass: bufAdd(b, 'class');
    tyClassRef:
      begin
        bufAdd(b, 'class of ');
        bufAddTypeName(b, t^.refClass);
      end;
    tyInterface: bufAdd(b, 'interface');
    tyAnsiString: bufAdd(b, 'ansistring');
    tyArrayOfConst: bufAdd(b, 'array of const');
    tyOpenArray:
      begin
        bufAdd(b, 'array of ');
        bufAddTypeName(b, t^.openElement);
      end;
    tyDynArray:
      begin
        bufAdd(b, 'array of ');
        bufAddTypeName(b, t^.dynElement);
      end;
    tyShortString:
      if t^.maxLength = 255 then
        bufAdd(b, 'string')
      else
        bufAdd(b, 'string[' + intToStr(t^.maxLength) + ']');
    tyRoutine:
      begin
        if t^.routineReference then
          bufAdd(b, 'reference to ');
        if t^.routineResult <> nil then
          bufAdd(b, 'function(')
        else
          bufAdd(b, 'procedure(');
        for i := 0 to t^.routineParams.count - 1 do
        begin
          p := PDecl(listGet(t^.routineParams, i));
          if i > 0 then
            bufAdd(b, '; ');
          if p^.isOut then
            bufAdd(b, 'out ')
          else if p^.varKind = vkVarParam then
            bufAdd(b, 'var ')
          else if p^.isConstRef then
            bufAdd(b, 'constref ')
          else if p^.varKind = vkConstParam then
            bufAdd(b, 'const ');
          if p^.varType <> nil then
            bufAddTypeName(b, p^.varType)
          else
            bufAddChar(b, '?');
        end;
        bufAddChar(b, ')');
        if t^.routineResult <> nil then
        begin
          bufAdd(b, ': ');
          bufAddTypeName(b, t^.routineResult);
        end;
        if t^.routineOfObject then
          bufAdd(b, ' of object');
        if t^.routineCDecl then
          bufAdd(b, '; cdecl');
      end;
    tyFile:
      if t^.isText then
        bufAdd(b, 'text')
      else if t^.fileElement = nil then
        bufAdd(b, 'file') { untyped }
      else
      begin
        bufAdd(b, 'file of ');
        bufAddTypeName(b, t^.fileElement);
      end;
    tySet:
      if t^.setElement <> nil then
      begin
        bufAdd(b, 'set of ');
        bufAddTypeName(b, t^.setElement);
      end
      else
        bufAdd(b, 'empty set');
  end;
end;

end.
