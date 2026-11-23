unit pcsrcgen;
{ Source generators (modernizing-pascal.md item 19): the Pascal counterpart
  of src/driver/Generators.cpp and include/p3/Driver/Generators.h; see there
  for the format of the model (and unit p3gen, rtl/p3gen.pas, for what a
  generator is).

  A generator is a program that starts  generator Name for Attr;  The
  compiler builds it, runs it on the model of each file's types marked
  [Attr], and includes what it writes in the file that declares the types
  (an include directive before the implementation keyword and one before
  the end of its declarations; both before the main block of a program).
  The files that were read for it come back through spliceFor. }

interface

uses pcsys, pcutil, pcdiag, pcast, pclex, pcparse;

{ Appends s with \, tab and newline escaped. }
procedure addEscaped(var b: TBuf; const s: string);
{ The model of the record and class types declared in root (a program's
  block, or a unit's interface) that are marked [attr] (lower case): the
  number of types; model gets nothing if it is 0. }
function buildModel(root: PDecl; const attr: string; var model: TBuf): longint;
{ Builds and runs the generators (PName paths) over the input and the units
  it uses (found next to it and in includeDirs, of PName). generatedDir:
  '' for gen next to the input. False after errors, which are reported. }
function runGenerators(const inputFile: string; const generators: TPtrList;
                       const generatedDir: string; const includeDirs: TPtrList;
                       mode: TLangMode; timeoutSecs: longint): boolean;
{ The text of the file path with the generated code included, if there
  is: its bytes (followed by #0) and their number. }
function spliceFor(const path: string; var data: PCharArray;
                   var len: int64): boolean;
{ --emit-model: parses the file quietly and builds its model. False if it
  has syntax errors. }
function modelOfFile(const path: string; mode: TLangMode; const attr: string;
                     var model: TBuf): boolean;

implementation

const
  tab = #9;

{ --- Fields --- }

procedure addEscaped(var b: TBuf; const s: string);
var
  i: longint;
begin
  for i := 1 to length(s) do
    case s[i] of
      '\': bufAdd(b, '\\');
      #9: bufAdd(b, '\t');
      #10: bufAdd(b, '\n');
      #13: bufAdd(b, '\r');
    else
      bufAddChar(b, s[i]);
    end;
end;

{ Appends the p[start..start+count-1] with the escapes of addEscaped undone. }
procedure addUnescaped(var b: TBuf; p: PCharArray; start, count: int64);
var
  i: int64;
  c: char;
begin
  i := start;
  while i < start + count do
  begin
    c := p^[i];
    if (c = '\') and (i + 1 < start + count) then
    begin
      i := i + 1;
      case p^[i] of
        't': bufAddChar(b, #9);
        'n': bufAddChar(b, #10);
        'r': bufAddChar(b, #13);
      else
        bufAddChar(b, p^[i]);
      end;
    end
    else
      bufAddChar(b, c);
    i := i + 1;
  end;
end;

procedure addField(var b: TBuf; const s: string);
begin
  bufAddChar(b, tab);
  addEscaped(b, s);
end;

procedure addNumField(var b: TBuf; n: longint);
begin
  bufAddChar(b, tab);
  bufAddInt(b, n);
end;

{ The text of the source between two locations of a file, with each run of
  white space as one blank (at most 255 characters). }
function textOf(b, e: TLoc): string;
var
  fid: longint;
  data: PCharArray;
  i, first, last: int64;
  r: string;
  blank: boolean;
  c: char;
begin
  textOf := '';
  if (b = noLoc) or (e = noLoc) or (e < b) then
    exit;
  fid := smFileOf(b);
  if fid = 0 then
    exit;
  data := smFileData(fid);
  first := b - smFileStart(fid);
  last := e - smFileStart(fid);
  if last > smFileLen(fid) then
    last := smFileLen(fid);
  r := '';
  blank := false;
  i := first;
  while (i < last) and (length(r) < 250) do
  begin
    c := data^[i];
    if (c = ' ') or (c = #9) or (c = #10) or (c = #13) then
      blank := length(r) > 0
    else
    begin
      if blank then
        r := r + ' ';
      blank := false;
      r := r + c;
    end;
    i := i + 1;
  end;
  textOf := r;
end;

procedure addPos(var b: TBuf; loc: TLoc);
var
  fid, line, col: longint;
begin
  smPresumed(loc, fid, line, col);
  addNumField(b, line);
  addNumField(b, col);
end;

{ --- The model --- }

procedure addArg(var b: TBuf; e: PNode);
var
  t, l: string;
begin
  bufAdd(b, 'arg');
  if e^.kind = nkIntegerLit then
  begin
    addField(b, 'int');
    addField(b, intToStr(e^.intValue));
  end
  else if (e^.kind = nkUnary) and (e^.uop = uoMinus) and
          (e^.operand^.kind = nkIntegerLit) then
  begin
    addField(b, 'int');
    addField(b, '-' + intToStr(e^.operand^.intValue));
  end
  else if e^.kind = nkRealLit then
  begin
    addField(b, 'real');
    addField(b, textOf(e^.b, e^.e));
  end
  else if e^.kind = nkStringLit then
  begin
    addField(b, 'string');
    addField(b, nameStr(e^.strValue));
  end
  else
  begin
    t := textOf(e^.b, e^.e);
    l := lowerStr(t);
    if (l = 'true') or (l = 'false') then
    begin
      addField(b, 'bool');
      addField(b, l);
    end
    else
    begin
      addField(b, 'ident');
      addField(b, t);
    end;
  end;
  bufAddChar(b, #10);
end;

procedure addAttrs(var b: TBuf; d: PDecl);
var
  i, j: longint;
  a: PAttribute;
begin
  for i := 0 to d^.attrs.count - 1 do
  begin
    a := PAttribute(listGet(d^.attrs, i));
    bufAdd(b, 'attr');
    addField(b, nameStr(a^.name.spelling));
    addPos(b, a^.name.loc);
    bufAddChar(b, #10);
    for j := 0 to a^.args.count - 1 do
      addArg(b, PNode(listGet(a^.args, j)));
  end;
end;

function visibilityName(v: TVisibility): string;
begin
  case v of
    vsPublished: visibilityName := 'published';
    vsProtected: visibilityName := 'protected';
    vsPrivate: visibilityName := 'private';
    vsStrictProtected: visibilityName := 'strictprotected';
    vsStrictPrivate: visibilityName := 'strictprivate';
  else
    visibilityName := 'public';
  end;
end;

function specText(ts: PTypeSpec): string;
begin
  if ts = nil then
    specText := ''
  else
    specText := textOf(ts^.b, ts^.e);
end;

procedure addMember(var b: TBuf; m: PDecl);
var
  i: longint;
  p: PDecl;
  kind, mode, stat: string;
begin
  if m^.kind = dkField then
  begin
    bufAdd(b, 'field');
    addField(b, nameStr(m^.ident.spelling));
    addField(b, specText(m^.fieldSpec));
    addField(b, visibilityName(m^.vis));
    addPos(b, m^.loc);
    bufAddChar(b, #10);
    addAttrs(b, m);
  end
  else if m^.kind = dkRoutine then
  begin
    if m^.methodKind = mtConstructor then
      kind := 'constructor'
    else if m^.methodKind = mtDestructor then
      kind := 'destructor'
    else if m^.isFunction then
      kind := 'function'
    else
      kind := 'procedure';
    if m^.isClassMethod or m^.isStatic then
      stat := '1'
    else
      stat := '0';
    bufAdd(b, 'method');
    addField(b, nameStr(m^.ident.spelling));
    addField(b, kind);
    if m^.isFunction then
      addField(b, specText(m^.resultSpec))
    else
      addField(b, '');
    addField(b, visibilityName(m^.vis));
    addField(b, stat);
    addPos(b, m^.loc);
    bufAddChar(b, #10);
    addAttrs(b, m);
    for i := 0 to m^.params.count - 1 do
    begin
      p := PDecl(listGet(m^.params, i));
      if p^.isOut then
        mode := 'out'
      else if p^.varKind = vkVarParam then
        mode := 'var'
      else if p^.varKind = vkConstParam then
        mode := 'const'
      else
        mode := 'value';
      bufAdd(b, 'param');
      addField(b, nameStr(p^.ident.spelling));
      addField(b, specText(p^.varSpec));
      addField(b, mode);
      bufAddChar(b, #10);
    end;
  end;
end;

function hasAttribute(d: PDecl; attr: PName): boolean;
var
  i: longint;
begin
  hasAttribute := false;
  for i := 0 to d^.attrs.count - 1 do
    if PAttribute(listGet(d^.attrs, i))^.name.name = attr then
      hasAttribute := true;
end;

function buildModel(root: PDecl; const attr: string; var model: TBuf): longint;
var
  decls: TPtrList;
  attrName: PName;
  i, j, n: longint;
  td, m: PDecl;
  ts: PTypeSpec;
  kind, parent: string;
  all: TPtrList;
  b: TBuf;
begin
  n := 0;
  attrName := intern(attr);
  listInit(decls);
  if root^.kind = dkUnit then
    decls := root^.iface^.decls
  else if root^.kind = dkProgram then
    decls := root^.progBlock^.decls;
  bufInit(b);
  for i := 0 to decls.count - 1 do
  begin
    td := PDecl(listGet(decls, i));
    if td^.kind <> dkType then
      continue;
    ts := td^.typeSpec;
    if (ts = nil) or td^.tgGeneric or not hasAttribute(td, attrName) then
      continue;
    listInit(all);
    parent := '';
    if ts^.kind = tsRecord then
    begin
      kind := 'record';
      if ts^.fields <> nil then
        for j := 0 to ts^.fields^.fixed.count - 1 do
          listAdd(all, listGet(ts^.fields^.fixed, j));
      for j := 0 to ts^.recMembers.count - 1 do
        listAdd(all, listGet(ts^.recMembers, j));
    end
    else if ts^.kind = tsClass then
    begin
      if ts^.isForwardClass then
        continue;
      kind := 'class';
      if identValid(ts^.parentName) then
        parent := nameStr(ts^.parentName.spelling);
      for j := 0 to ts^.memberList.count - 1 do
        listAdd(all, listGet(ts^.memberList, j));
    end
    else
      continue;
    if n = 0 then
      bufAdd(b, 'p3model'#9'1'#10);
    bufAdd(b, 'type');
    addField(b, nameStr(td^.ident.spelling));
    addField(b, kind);
    addField(b, parent);
    addPos(b, td^.loc);
    bufAddChar(b, #10);
    addAttrs(b, td);
    for j := 0 to all.count - 1 do
    begin
      m := PDecl(listGet(all, j));
      addMember(b, m);
    end;
    bufAdd(b, 'endtype'#10);
    n := n + 1;
  end;
  if n > 0 then
    bufAddChars(model, b.data, 0, b.len);
  bufFree(b);
  buildModel := n;
end;

{ --- Files and commands --- }

{ The absolute path of path (a file need not exist), without . and .. }
function absPath(const path: string): string;
var
  full, part, r: string;
  i: longint;
  parts: TPtrList;
  dummy: pointer;
begin
  if (path <> '') and (path[1] = '/') then
    full := path
  else
    full := pathJoin(sysGetCwd, path);
  listInit(parts);
  part := '';
  for i := 1 to length(full) + 1 do
    if (i > length(full)) or (full[i] = '/') then
    begin
      if part = '..' then
      begin
        if parts.count > 0 then
          dummy := listPop(parts);
      end
      else if (part <> '') and (part <> '.') then
        listAdd(parts, intern(part));
      part := '';
    end
    else
      part := part + full[i];
  r := '';
  for i := 0 to parts.count - 1 do
    r := r + '/' + nameStr(PName(listGet(parts, i)));
  if r = '' then
    r := '/';
  absPath := r;
end;

{ path in single quotes for the shell }
procedure addQuoted(var b: TBuf; const s: string);
var
  i: longint;
begin
  bufAddChar(b, '''');
  for i := 1 to length(s) do
    if s[i] = '''' then
      bufAdd(b, '''\''''')
    else
      bufAddChar(b, s[i]);
  bufAddChar(b, '''');
end;

function runCommand(var cmd: TBuf): longint;
begin
  runCommand := sysRunChars(cmd.data, cmd.len);
end;

function readWhole(const path: string; var data: PCharArray; var len: int64): boolean;
begin
  readWhole := sysReadFile(path, data, len);
end;

function writeBuf(const path: string; const b: TBuf): boolean;
begin
  writeBuf := sysWriteFile(path, b.data, b.len);
end;

{ --- The files read --- }

type
  PSource = ^TSource;
  TSource = record
    path: string;
    fid: longint;
    root: PDecl;
  end;

  PSplice = ^TSplice;
  TSplice = record
    path: PName; { absolute }
    data: PCharArray;
    len: int64;
  end;

var
  splices: TPtrList; { of PSplice }

function spliceFor(const path: string; var data: PCharArray;
                   var len: int64): boolean;
var
  i: longint;
  a: PName;
begin
  spliceFor := false;
  if splices.count = 0 then
    exit;
  a := intern(absPath(path));
  for i := 0 to splices.count - 1 do
    if PSplice(listGet(splices, i))^.path = a then
    begin
      data := PSplice(listGet(splices, i))^.data;
      len := PSplice(listGet(splices, i))^.len;
      spliceFor := true;
      exit;
    end;
end;

{ A file parsed without reporting: nil after syntax errors. }
function parseQuietly(const path: string; mode: TLangMode; var fid: longint;
                      var isUnit: boolean): PDecl;
var
  root: PDecl;
begin
  parseQuietly := nil;
  if not smLoadFile(path, fid) then
    exit;
  diagQuietBegin;
  root := parseInput(fid, mode, true, isUnit);
  if diagQuietEnd > 0 then
    root := nil;
  parseQuietly := root;
end;

function modelOfFile(const path: string; mode: TLangMode; const attr: string;
                     var model: TBuf): boolean;
var
  fid: longint;
  isUnit: boolean;
  root: PDecl;
begin
  root := parseQuietly(path, mode, fid, isUnit);
  if root = nil then
    exit(false);
  buildModel(root, attr, model);
  modelOfFile := true;
end;

{ The location of line:col (1-based) in file fid. }
function locAt(fid: longint; line, col: longint): TLoc;
var
  data: PCharArray;
  len, off: int64;
  l: longint;
begin
  data := smFileData(fid);
  len := smFileLen(fid);
  off := 0;
  l := 1;
  while (l < line) and (off < len) do
  begin
    while (off < len) and (data^[off] <> #10) do
      off := off + 1;
    if off >= len then
      break;
    off := off + 1;
    l := l + 1;
  end;
  if col > 0 then
    off := off + col - 1;
  if off > len then
    off := len;
  locAt := smFileStart(fid) + off;
end;

{ --- What the generators wrote --- }

type
  TOffsets = array[0..9] of int64;
  PTypeOut = ^TTypeOut;
  TTypeOut = record
    name, kind: string;
    key: PName; { lower case name }
    members, impl: TBuf;
  end;

function toInt(const s: string): longint;
var
  i: longint;
  r: longint;
begin
  r := 0;
  for i := 1 to length(s) do
    if (s[i] >= '0') and (s[i] <= '9') then
      r := r * 10 + (ord(s[i]) - 48);
  toInt := r;
end;

{ One line of the generator's output, p[start..start+count-1], is
  tag TAB field TAB ...: the number of fields, and field k's bytes. }
procedure splitLine(p: PCharArray; start, count: int64; var starts, counts: TOffsets;
                    var n: longint);
var
  i, from: int64;
begin
  n := 0;
  from := start;
  i := start;
  while i <= start + count do
  begin
    if (i = start + count) or (p^[i] = #9) then
    begin
      if n <= 9 then
      begin
        starts[n] := from;
        counts[n] := i - from;
      end;
      n := n + 1;
      from := i + 1;
    end;
    i := i + 1;
  end;
end;

function fieldStr(p: PCharArray; start, count: int64): string;
var
  b: TBuf;
  s: string;
  i: int64;
begin
  bufInit(b);
  addUnescaped(b, p, start, count);
  s := '';
  i := 0;
  while (i < b.len) and (length(s) < 255) do
  begin
    s := s + b.data^[i];
    i := i + 1;
  end;
  bufFree(b);
  fieldStr := s;
end;

{ --- The generators --- }

type
  PGenInfo = ^TGenInfo;
  TGenInfo = record
    path, name, exe: string;
    attr: string;
  end;

function stem(const path: string): string;
var
  i, dot, slash: longint;
  s: string;
begin
  slash := 0;
  for i := 1 to length(path) do
    if path[i] = '/' then
      slash := i;
  s := copy(path, slash + 1, 255);
  dot := 0;
  for i := 1 to length(s) do
    if s[i] = '.' then
      dot := i;
  if dot > 1 then
    s := copy(s, 1, dot - 1);
  stem := s;
end;

function runGenerators(const inputFile: string; const generators: TPtrList;
                       const generatedDir: string; const includeDirs: TPtrList;
                       mode: TLangMode; timeoutSecs: longint): boolean;
var
  genDir, binDir, inputDir, self, p: string;
  gens: TPtrList; { of PGenInfo }
  dirs: TPtrList; { of PName }
  work, seen: TPtrList; { of PName }
  files: TPtrList; { of PSource }
  i, j, k, g, u: longint;
  gi: PGenInfo;
  cmd: TBuf;
  fid: longint;
  isUnit, ok, found: boolean;
  root: PDecl;
  src: PSource;
  uses1: TPtrList;
  id: PIdent;
  base, ext, cand, foundPath: string;
  bb, ee: longint;
  numTypes: longint;
  model, outText, intf, impl, spliced: TBuf;
  modelPath, outPath, intfPath, implPath: string;
  rc: longint;
  outData: PCharArray;
  outLen, pos, eol: int64;
  starts, counts: TOffsets;
  nf: longint;
  tag: string;
  types: TPtrList; { of PTypeOut }
  cur, t: PTypeOut;
  key: PName;
  sp: PSplice;
  srcData: PCharArray;
  srcLen, a, b2: int64;
  incI, incM: string;

  function newType(const nm, kd: string): PTypeOut;
  var
    r: PTypeOut;
  begin
    r := PTypeOut(arenaAlloc(sizeof(TTypeOut)));
    r^.name := nm;
    r^.kind := kd;
    r^.key := intern(lowerStr(nm));
    bufInit(r^.members);
    bufInit(r^.impl);
    newType := r;
  end;

  function findType(k: PName): PTypeOut;
  var
    q: longint;
  begin
    findType := nil;
    for q := 0 to types.count - 1 do
      if PTypeOut(listGet(types, q))^.key = k then
        findType := PTypeOut(listGet(types, q));
  end;

  procedure fail(const msg: string);
  begin
    diagError(noLoc, msg);
    ok := false;
  end;

begin
  ok := true;
  runGenerators := false;
  inputDir := pathDirName(inputFile);
  if generatedDir = '' then
    genDir := pathJoin(inputDir, 'gen')
  else
    genDir := generatedDir;
  genDir := absPath(genDir);
  binDir := pathJoin(genDir, '.bin');
  sysMakeDirs(binDir);
  self := sysExecutablePath;

  { the directories units are looked for in (not the library) }
  listInit(dirs);
  listAdd(dirs, intern(inputDir));
  for i := 0 to includeDirs.count - 1 do
    listAdd(dirs, listGet(includeDirs, i));

  { --- the generators: header, then build --- }
  listInit(gens);
  for i := 0 to generators.count - 1 do
  begin
    p := nameStr(PName(listGet(generators, i)));
    gi := PGenInfo(arenaAlloc(sizeof(TGenInfo)));
    gi^.path := p;
    gi^.name := '';
    gi^.attr := '';
    root := nil;
    if not sysFileExists(p) then
    begin
      diagError(noLoc, 'cannot open generator ''' + p + ''': ' + sysOpenError(p));
      exit;
    end;
    root := parseQuietly(p, mode, fid, isUnit);
    if (root <> nil) and not isUnit then
    begin
      if not identValid(root^.generatorFor) then
      begin
        diagError(noLoc, '''' + p + ''' is not a source generator: it must ' +
                  'start with ''generator Name for Attribute;''');
        exit;
      end;
      gi^.name := nameStr(root^.ident.spelling);
      gi^.attr := nameStr(root^.generatorFor.name);
      gi^.exe := pathJoin(binDir, gi^.name);
    end
    else
      gi^.exe := pathJoin(binDir, stem(p));
    listAdd(gens, gi);
    bufInit(cmd);
    addQuoted(cmd, self);
    bufAdd(cmd, ' --no-project --mode=objfpc');
    for j := 0 to includeDirs.count - 1 do
    begin
      bufAdd(cmd, ' ');
      addQuoted(cmd, '-I' + nameStr(PName(listGet(includeDirs, j))));
    end;
    bufAdd(cmd, ' ');
    if pathDirName(p) = '' then
      addQuoted(cmd, '-I.')
    else
      addQuoted(cmd, '-I' + pathDirName(p));
    bufAdd(cmd, ' -o ');
    addQuoted(cmd, gi^.exe);
    bufAdd(cmd, ' ');
    addQuoted(cmd, p);
    rc := runCommand(cmd);
    bufFree(cmd);
    if rc <> 0 then
    begin
      diagError(noLoc, 'generator ''' + p + ''' failed to build');
      exit;
    end;
  end;

  { --- the project's sources: the input and the units it uses --- }
  listInit(files);
  listInit(work);
  listInit(seen);
  listAdd(work, intern(inputFile));
  listAdd(seen, intern(absPath(inputFile)));
  while work.count > 0 do
  begin
    p := nameStr(PName(listPop(work)));
    if not sysFileExists(p) then
      continue;
    root := parseQuietly(p, mode, fid, isUnit);
    if root = nil then
      continue;
    listInit(uses1);
    if root^.kind = dkProgram then
    begin
      for j := 0 to root^.progUses.count - 1 do
        listAdd(uses1, listGet(root^.progUses, j));
    end
    else if root^.kind = dkUnit then
    begin
      for j := 0 to root^.ifaceUses.count - 1 do
        listAdd(uses1, listGet(root^.ifaceUses, j));
      for j := 0 to root^.implUses.count - 1 do
        listAdd(uses1, listGet(root^.implUses, j));
    end;
    for j := 0 to uses1.count - 1 do
    begin
      id := PIdent(listGet(uses1, j));
      for k := 0 to dirs.count - 1 do
      begin
        foundPath := '';
        for bb := 1 to 2 do
          for ee := 1 to 2 do
          begin
            if bb = 1 then
              base := nameStr(id^.name)
            else
              base := nameStr(id^.spelling);
            if ee = 1 then
              ext := '.pas'
            else
              ext := '.pp';
            cand := pathJoin(nameStr(PName(listGet(dirs, k))), base + ext);
            if (foundPath = '') and sysFileExists(cand) then
              foundPath := cand;
          end;
        if foundPath <> '' then
        begin
          found := false;
          for u := 0 to seen.count - 1 do
            if PName(listGet(seen, u)) = intern(absPath(foundPath)) then
              found := true;
          if not found then
          begin
            listAdd(seen, intern(absPath(foundPath)));
            listAdd(work, intern(foundPath));
          end;
          break;
        end;
      end;
    end;
    src := PSource(arenaAlloc(sizeof(TSource)));
    src^.path := p;
    src^.fid := fid;
    src^.root := root;
    listAdd(files, src);
  end;

  { --- run each generator over each file --- }
  for i := 0 to files.count - 1 do
  begin
    src := PSource(listGet(files, i));
    listInit(types);
    cur := nil;
    for g := 0 to gens.count - 1 do
    begin
      gi := PGenInfo(listGet(gens, g));
      if gi^.attr = '' then
        continue;
      bufInit(model);
      numTypes := buildModel(src^.root, gi^.attr, model);
      if numTypes = 0 then
      begin
        bufFree(model);
        continue;
      end;
      modelPath := pathJoin(genDir, stem(src^.path) + '.' + gi^.name + '.model');
      outPath := pathJoin(genDir, stem(src^.path) + '.' + gi^.name + '.out');
      if not writeBuf(modelPath, model) then
      begin
        diagError(noLoc, 'cannot write ''' + modelPath + '''');
        exit;
      end;
      bufFree(model);
      bufInit(cmd);
      bufAdd(cmd, 'ulimit -t ');
      bufAddInt(cmd, timeoutSecs);
      bufAdd(cmd, '; ulimit -v 2097152 2>/dev/null; ');
      addQuoted(cmd, gi^.exe);
      bufAdd(cmd, ' < ');
      addQuoted(cmd, modelPath);
      bufAdd(cmd, ' > ');
      addQuoted(cmd, outPath);
      rc := runCommand(cmd);
      bufFree(cmd);
      if rc = 152 then { SIGXCPU: the time limit }
      begin
        fail('generator ''' + gi^.name + ''' ran longer than ' +
             intToStr(timeoutSecs) + ' seconds');
        continue;
      end;
      if rc <> 0 then
      begin
        fail('generator ''' + gi^.name + ''' failed (exit status ' +
             intToStr(rc) + ')');
        continue;
      end;
      if not readWhole(outPath, outData, outLen) then
      begin
        fail('cannot read ''' + outPath + '''');
        continue;
      end;
      pos := 0;
      while pos < outLen do
      begin
        eol := pos;
        while (eol < outLen) and (outData^[eol] <> #10) do
          eol := eol + 1;
        if eol > pos then
        begin
          splitLine(outData, pos, eol - pos, starts, counts, nf);
          tag := fieldStr(outData, starts[0], counts[0]);
          if (tag = 'type') and (nf >= 3) then
          begin
            key := intern(lowerStr(fieldStr(outData, starts[1], counts[1])));
            t := findType(key);
            if t = nil then
            begin
              t := newType(fieldStr(outData, starts[1], counts[1]),
                           fieldStr(outData, starts[2], counts[2]));
              listAdd(types, t);
            end;
            cur := t;
          end
          else if (tag = 'member') and (cur <> nil) and (nf >= 2) then
          begin
            bufAdd(cur^.members, '    ');
            addUnescaped(cur^.members, outData, starts[1], counts[1]);
            bufAddChar(cur^.members, #10);
          end
          else if (tag = 'line') and (cur <> nil) and (nf >= 2) then
          begin
            addUnescaped(cur^.impl, outData, starts[1], counts[1]);
            bufAddChar(cur^.impl, #10);
          end
          else if ((tag = 'error') or (tag = 'warning')) and (nf >= 4) then
          begin
            if tag = 'error' then
            begin
              diagError(locAt(src^.fid, toInt(fieldStr(outData, starts[1], counts[1])),
                              toInt(fieldStr(outData, starts[2], counts[2]))),
                        fieldStr(outData, starts[3], counts[3]));
              ok := false;
            end
            else
              diagWarning(locAt(src^.fid, toInt(fieldStr(outData, starts[1], counts[1])),
                                toInt(fieldStr(outData, starts[2], counts[2]))),
                          fieldStr(outData, starts[3], counts[3]));
          end;
        end;
        pos := eol + 1;
      end;
    end;
    if types.count = 0 then
      continue;

    { the generated code: type halves, then the method bodies }
    base := stem(src^.path);
    bufInit(intf);
    bufInit(impl);
    bufAdd(intf, '{ Generated by p3 for ' + base +
           ': do not edit (it is rewritten at each build). }'#10'type'#10);
    bufAdd(impl, '{ Generated by p3 for ' + base +
           ': do not edit (it is rewritten at each build). }'#10);
    for j := 0 to types.count - 1 do
    begin
      t := PTypeOut(listGet(types, j));
      bufAdd(intf, '  ' + t^.name + ' = partial ' + t^.kind + #10);
      bufAddChars(intf, t^.members.data, 0, t^.members.len);
      bufAdd(intf, '  end;'#10);
      bufAddChars(impl, t^.impl.data, 0, t^.impl.len);
    end;
    intfPath := pathJoin(genDir, base + '.intf.inc');
    implPath := pathJoin(genDir, base + '.impl.inc');
    if not writeBuf(intfPath, intf) or not writeBuf(implPath, impl) then
    begin
      diagError(noLoc, 'cannot write ''' + intfPath + '''');
      exit;
    end;
    bufFree(intf);
    bufFree(impl);

    { the source with the two includes, on the lines of the points they go
      before (so no line of it moves) }
    incI := '{$I ' + intfPath + '}';
    incM := '{$I ' + implPath + '}';
    srcData := smFileData(src^.fid);
    srcLen := smFileLen(src^.fid);
    bufInit(spliced);
    if src^.root^.kind = dkUnit then
    begin
      a := src^.root^.implLoc - smFileStart(src^.fid);
      b2 := src^.root^.declsEndLoc - smFileStart(src^.fid);
      bufAddChars(spliced, srcData, 0, a);
      bufAdd(spliced, incI);
      bufAddChars(spliced, srcData, a, b2 - a);
      bufAdd(spliced, incM);
      bufAddChars(spliced, srcData, b2, srcLen - b2);
    end
    else
    begin
      a := src^.root^.progBlock^.body^.b - smFileStart(src^.fid);
      bufAddChars(spliced, srcData, 0, a);
      bufAdd(spliced, incI);
      bufAdd(spliced, incM);
      bufAddChars(spliced, srcData, a, srcLen - a);
    end;
    bufAddChar(spliced, #0);
    sp := PSplice(arenaAlloc(sizeof(TSplice)));
    sp^.path := intern(absPath(src^.path));
    sp^.data := spliced.data;
    sp^.len := spliced.len - 1;
    listAdd(splices, sp);
  end;
  runGenerators := ok;
end;

end.
