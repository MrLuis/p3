unit pcscope;
{ Name scopes for Sema; a port of include/pcc/Sema/Scope.h. A scope maps
  lower-case names to declarations; a `with` scope exposes a record's
  fields; units add the interface scopes of the units they use. In
  objfpc, `with obj do` and every method body open a scope exposing the
  members of a class and its ancestors. }

interface

uses pcsys, pcutil, pcdiag, pcast, pctypes;

type
  PScope = ^TScope;
  TScope = record
    parent: PScope;
    symbols: TIntMap;      { name id -> PDecl }
    names: TPtrList;       { of PName, the names declared here }
    withRecord: PType;     { a with scope: the record type, else nil }
    withClass: PType;      { a class scope: the class, else nil }
    withCtx: PWithContext;
    imports: TPtrList;     { of PScope: used units' interfaces (later win) }
    { the outermost scope of a routine: a name found beyond it belongs
      to an enclosing routine (or to the program) }
    routineBoundary: boolean;
  end;

function newScope(parent: PScope): PScope;
function newWithScope(parent: PScope; rec: PType; w: PWithContext): PScope;
function newClassScope(parent: PScope; c: PType; w: PWithContext): PScope;
procedure scopeAddImport(s, imported: PScope);
{ the declaration of name in s itself (not imports), or nil }
function scopeLookupOwn(s: PScope; name: PName): PDecl;
{ declares name; returns the existing declaration (and does not insert)
  if name is already declared in s, else nil }
function scopeInsert(s: PScope; name: PName; d: PDecl): PDecl;
function scopeLookupLocal(s: PScope; name: PName): PDecl;
{ the innermost declaration of name visible from s; w receives the with
  context if it is a field found through a with. A variable found in an
  enclosing routine is marked as captured (whatever the caller then does
  with it: every use of such a variable starts here). }
function scopeLookup(s: PScope; name: PName; var w: PWithContext): PDecl;
function scopeFind(s: PScope; name: PName): PDecl;
{ appends (PName) the names declared directly in s, including imports }
procedure scopeNames(s: PScope; var names: TPtrList);

implementation

function newScope(parent: PScope): PScope;
var
  s: PScope;
begin
  s := PScope(arenaAlloc(sizeof(TScope)));
  s^.parent := parent;
  mapInit(s^.symbols);
  listInit(s^.names);
  s^.withRecord := nil;
  s^.withClass := nil;
  s^.withCtx := nil;
  listInit(s^.imports);
  s^.routineBoundary := false;
  newScope := s;
end;

function newWithScope(parent: PScope; rec: PType; w: PWithContext): PScope;
var
  s: PScope;
begin
  s := newScope(parent);
  s^.withRecord := rec;
  s^.withCtx := w;
  newWithScope := s;
end;

function newClassScope(parent: PScope; c: PType; w: PWithContext): PScope;
var
  s: PScope;
begin
  s := newScope(parent);
  s^.withClass := c;
  s^.withCtx := w;
  newClassScope := s;
end;

procedure scopeAddImport(s, imported: PScope);
begin
  listAdd(s^.imports, imported);
end;

function scopeLookupOwn(s: PScope; name: PName): PDecl;
begin
  scopeLookupOwn := PDecl(mapGet(s^.symbols, name^.id));
end;

function scopeInsert(s: PScope; name: PName; d: PDecl): PDecl;
var
  prev: PDecl;
begin
  prev := PDecl(mapGet(s^.symbols, name^.id));
  if prev <> nil then
    exit(prev);
  mapPut(s^.symbols, name^.id, d);
  listAdd(s^.names, name);
  scopeInsert := nil;
end;

function scopeLookupLocal(s: PScope; name: PName): PDecl;
var
  d: PDecl;
  i: longint;
  owner: PType;
begin
  if s^.withRecord <> nil then
    exit(findField(s^.withRecord, name));
  if (s^.withClass <> nil) and (s^.withCtx = nil) then
  begin
    { a record's static method: only its class methods and operators
      (static) are usable without a record }
    d := findMember(s^.withClass, name, owner);
    if (d <> nil) and (d^.kind = dkRoutine) and d^.isStatic then
      exit(d);
    exit(nil);
  end;
  if s^.withClass <> nil then
    exit(findMember(s^.withClass, name, owner));
  d := scopeLookupOwn(s, name);
  if d <> nil then
    exit(d);
  for i := s^.imports.count - 1 downto 0 do
  begin
    d := scopeLookupOwn(PScope(listGet(s^.imports, i)), name);
    if d <> nil then
      exit(d);
  end;
  scopeLookupLocal := nil;
end;

{ d was found in an enclosing routine's scope s: a variable of that
  routine is captured, and so is the Self of the method whose members s
  exposes. }
procedure noteCaptured(s: PScope; d: PDecl);
var
  v: PDecl;
begin
  if (d^.kind = dkVar) and (d^.owner <> nil) then
  begin
    v := d;
    while v <> nil do { an overlay: what it overlays }
    begin
      v^.captured := true;
      v := v^.absRoot;
    end;
  end;
  if (s^.withCtx <> nil) and (s^.withCtx^.rec^.kind = nkNameRef) then
  begin
    v := s^.withCtx^.rec^.refDecl;
    if (v <> nil) and (v^.kind = dkVar) and (v^.owner <> nil) then
      v^.captured := true;
  end;
end;

function scopeLookup(s: PScope; name: PName; var w: PWithContext): PDecl;
var
  d: PDecl;
  outside: boolean;
begin
  outside := false;
  while s <> nil do
  begin
    d := scopeLookupLocal(s, name);
    if d <> nil then
    begin
      w := s^.withCtx;
      if outside then
        noteCaptured(s, d);
      exit(d);
    end;
    outside := outside or s^.routineBoundary;
    s := s^.parent;
  end;
  scopeLookup := nil;
end;

function scopeFind(s: PScope; name: PName): PDecl;
var
  w: PWithContext;
begin
  scopeFind := scopeLookup(s, name, w);
end;

procedure scopeNames(s: PScope; var names: TPtrList);
var
  i, j: longint;
  imp: PScope;
  c: PType;
begin
  if s^.withRecord <> nil then
  begin
    for i := 0 to s^.withRecord^.allFields.count - 1 do
      listAdd(names, PDecl(listGet(s^.withRecord^.allFields, i))^.ident.name);
    exit;
  end;
  if s^.withClass <> nil then
  begin
    c := s^.withClass;
    while c <> nil do
    begin
      for i := 0 to c^.classMembers.count - 1 do
        if (s^.withCtx <> nil) or
           ((PDecl(listGet(c^.classMembers, i))^.kind = dkRoutine) and
            PDecl(listGet(c^.classMembers, i))^.isStatic) then
          listAdd(names, PDecl(listGet(c^.classMembers, i))^.ident.name);
      c := c^.classParent;
    end;
    exit;
  end;
  for i := 0 to s^.names.count - 1 do
    listAdd(names, listGet(s^.names, i));
  for i := 0 to s^.imports.count - 1 do
  begin
    imp := PScope(listGet(s^.imports, i));
    for j := 0 to imp^.names.count - 1 do
      listAdd(names, listGet(imp^.names, j));
  end;
end;

end.
