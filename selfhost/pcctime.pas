{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
unit pcctime;
{ Constants computed by compiletime functions (modernizing-pascal.md item
  19): the Pascal counterpart of src/driver/Compiletime.cpp, which explains
  the scheme. In short: for each  const X = F(args)  at the level of a
  program or unit, F compiletime, a small program is written to
  <generated dir>/.ct (the file's uses, the declarations the call needs on
  their own lines and columns, a printer of the result as a Pascal
  constant), built with this compiler and run; its output replaces the
  declaration in the source (spliceFor), and the compilation starts again
  on the new text. }

interface

uses pcsys, pcutil, pcdiag, pcast, pclex, pcsrcgen;

{ Computes the compiletime constants of root (a program or unit) and of
  units (of PDecl, loaded from sources); restart is true if sources got
  new text. False after errors, which are reported. defines, generators:
  of PName; includeDirs: of PName. }
function evaluateCompiletime(root: PDecl; const units: TPtrList;
                             mode: TLangMode; const inputFile,
                             generatedDir: string;
                             const includeDirs, defines,
                             generators: TPtrList; timeoutSecs: longint;
                             var restart: boolean): boolean;

implementation

type
  PTopDecl = ^TTopDecl;
  { one top-level declaration of a file, as the evaluation program may copy
    it: its text is [b, e) in file fid (offsets) }
  TTopDecl = record
    d: PDecl;
    section: string[8];  { 'const', 'type', 'var' or '' (a routine) }
    fid: longint;
    b, e: int64;
    repl: TBuf;          { a computed constant's text, or a var's }
    hasRepl: boolean;
    pending: boolean;    { a compiletime constant not computed yet }
    headingOnly: boolean; { a routine heading: copied as forward }
    { a routine that is not compiletime, or a variable: copied (a heading
      marked external) for Sema to say they cannot be used }
    unusable: boolean;
    included: boolean;   { (while one constant is evaluated) }
  end;

  PFileInfo = ^TFileInfo;
  TFileInfo = record
    root: PDecl;
    fid: longint;
    path: string;
    decls: TPtrList; { of PTopDecl }
    uses_: TPtrList; { of PName (spelling) }
    byName: TIntMap; { name id -> PPtrList of PTopDecl }
  end;

  PPtrList = ^TPtrList;

var
  files: TPtrList;        { of PFileInfo }
  compiletimeFns: TIntMap; { name id -> PPtrList of PDecl }
  anyCompiletime: boolean;
  optMode: TLangMode;
  optInput, optGenDir: string;
  optIncludes, optDefines, optGenerators: TPtrList;
  optTimeout: longint;

{ --- Text --- }

function isIdentStart(c: char): boolean;
begin
  isIdentStart := (c in ['a'..'z', 'A'..'Z', '_']);
end;

function isIdentChar(c: char): boolean;
begin
  isIdentChar := (c in ['a'..'z', 'A'..'Z', '_', '0'..'9']);
end;

{ Skips a comment or string at i of p (len bytes): true, i past it. }
function skipCommentOrString(p: PCharArray; len: int64; var i: int64): boolean;
var
  c: char;
begin
  skipCommentOrString := true;
  c := p^[i];
  if c = '{' then
  begin
    i := i + 1;
    while (i < len) and (p^[i] <> '}') do
      i := i + 1;
    if i < len then
      i := i + 1;
    exit;
  end;
  if (c = '(') and (i + 1 < len) and (p^[i + 1] = '*') then
  begin
    i := i + 2;
    while (i + 1 < len) and not ((p^[i] = '*') and (p^[i + 1] = ')')) do
      i := i + 1;
    if i + 1 < len then
      i := i + 2
    else
      i := len;
    exit;
  end;
  if (c = '/') and (i + 1 < len) and (p^[i + 1] = '/') then
  begin
    while (i < len) and (p^[i] <> #10) do
      i := i + 1;
    exit;
  end;
  if c = '''' then
  begin
    i := i + 1;
    while i < len do
    begin
      if p^[i] = '''' then
      begin
        if (i + 1 < len) and (p^[i + 1] = '''') then
        begin
          i := i + 2;
          continue;
        end;
        i := i + 1;
        break;
      end;
      i := i + 1;
    end;
    exit;
  end;
  skipCommentOrString := false;
end;

{ The identifiers of p[b, e) (lower case, outside comments and strings)
  not in seen: added to seen and to work (of PName). }
procedure scanIdentifiers(p: PCharArray; b, e: int64; var seen: TIntMap;
                          var work: TPtrList);
var
  i, s: int64;
  c: char;
  w: string;
  n: PName;
begin
  i := b;
  while i < e do
  begin
    if skipCommentOrString(p, e, i) then
      continue;
    c := p^[i];
    if c in ['0'..'9'] then
    begin
      while (i < e) and (isIdentChar(p^[i]) or (p^[i] = '.')) do
        i := i + 1;
      continue;
    end;
    if c in ['$', '#', '%'] then
    begin
      i := i + 1;
      while (i < e) and (isIdentChar(p^[i]) or (p^[i] = '$')) do
        i := i + 1;
      continue;
    end;
    if isIdentStart(c) then
    begin
      s := i;
      w := '';
      while (i < e) and isIdentChar(p^[i]) do
      begin
        if length(w) < 255 then
          w := w + p^[i];
        i := i + 1;
      end;
      n := intern(lowerStr(w));
      if not mapHas(seen, n^.id) then
      begin
        mapPut(seen, n^.id, n);
        listAdd(work, n);
      end;
      continue;
    end;
    i := i + 1;
  end;
end;

{ The end of a routine heading at b: just after its first ';' outside
  parentheses, comments and strings. }
function headingEnd(p: PCharArray; b, len: int64): int64;
var
  i: int64;
  depth: longint;
  c: char;
begin
  depth := 0;
  i := b;
  while i < len do
  begin
    if skipCommentOrString(p, len, i) then
      continue;
    c := p^[i];
    i := i + 1;
    if c in ['(', '['] then
      depth := depth + 1
    else if (c in [')', ']']) and (depth > 0) then
      depth := depth - 1
    else if (c = ';') and (depth = 0) then
      exit(i);
  end;
  headingEnd := len;
end;

{ The compiler directives of p[0, e) other than includes and conditional
  compilation, in order. }
procedure copiedDirectives(p: PCharArray; e: int64; var o: TBuf);
var
  i, j, n: int64;
  name: string;
  skip, multi: boolean;
begin
  i := 0;
  while i < e do
  begin
    if (p^[i] = '{') and (i + 1 < e) and (p^[i + 1] = '$') then
    begin
      j := i + 2;
      while (j < e) and (p^[j] <> '}') do
        j := j + 1;
      if j >= e then
        exit;
      n := i + 2;
      name := '';
      while (n < j) and isIdentChar(p^[n]) do
      begin
        if length(name) < 32 then
          name := name + p^[n];
        n := n + 1;
      end;
      name := lowerStr(name);
      skip := (name = 'i') or (name = 'include') or (name = 'ifdef') or
              (name = 'ifndef') or (name = 'if') or (name = 'ifopt') or
              (name = 'else') or (name = 'elseif') or (name = 'endif') or
              (name = 'ifend') or (name = 'mode');
      multi := false;
      for n := i + 2 to j - 1 do
        if p^[n] = #10 then
          multi := true;
      if not skip and not multi then
        bufAddChars(o, p, i, j + 1 - i);
      i := j + 1;
      continue;
    end;
    if skipCommentOrString(p, e, i) then
      continue;
    i := i + 1;
  end;
end;

function countNewlines(p: PCharArray; b, e: int64): longint;
var
  i: int64;
  n: longint;
begin
  n := 0;
  for i := b to e - 1 do
    if p^[i] = #10 then
      n := n + 1;
  countNewlines := n;
end;

{ The text of p[b, e) as a string (short texts: type names, indexes). }
function textStr(p: PCharArray; b, e: int64): string;
var
  s: string;
  i: int64;
begin
  s := '';
  i := b;
  while (i < e) and (length(s) < 255) do
  begin
    s := s + p^[i];
    i := i + 1;
  end;
  textStr := s;
end;

function locOffset(loc: TLoc; var fid: longint): int64;
begin
  fid := smFileOf(loc);
  if fid = 0 then
    exit(-1);
  locOffset := loc - smFileStart(fid);
end;

{ The text of a type spec or expression, as written. }
function specText(ts: PTypeSpec): string;
var
  fid: longint;
  b: int64;
begin
  b := locOffset(ts^.b, fid);
  if b < 0 then
    exit('');
  specText := textStr(smFileData(fid), b, ts^.e - smFileStart(fid));
end;

function nodeText(n: PNode): string;
var
  fid: longint;
  b: int64;
begin
  b := locOffset(n^.b, fid);
  if b < 0 then
    exit('');
  nodeText := textStr(smFileData(fid), b, n^.e - smFileStart(fid));
end;

procedure addNameEntry(var m: TIntMap; key: PName; value: pointer);
var
  l: PPtrList;
begin
  l := PPtrList(mapGet(m, key^.id));
  if l = nil then
  begin
    l := PPtrList(arenaAlloc(sizeof(TPtrList)));
    listInit(l^);
    mapPut(m, key^.id, l);
  end;
  listAdd(l^, value);
end;

{ --- The files --- }

procedure addFile(root: PDecl);
var
  f: PFileInfo;
  blocks: array[1..2] of PBlock;
  nb, k, i, j: longint;
  d: PDecl;
  t: PTopDecl;
  fid2: longint;
  b, e: int64;
  ts: PTypeSpec;
  bodyNode: PNode;
begin
  f := PFileInfo(arenaAlloc(sizeof(TFileInfo)));
  f^.root := root;
  f^.fid := smFileOf(root^.loc);
  f^.path := smFileName(f^.fid);
  listInit(f^.decls);
  listInit(f^.uses_);
  mapInit(f^.byName);
  nb := 0;
  if root^.kind = dkProgram then
  begin
    nb := 1;
    blocks[1] := root^.progBlock;
    for i := 0 to root^.progUses.count - 1 do
      listAdd(f^.uses_, PIdent(listGet(root^.progUses, i))^.spelling);
  end
  else if root^.kind = dkUnit then
  begin
    nb := 2;
    blocks[1] := root^.iface;
    blocks[2] := root^.impl;
    for i := 0 to root^.ifaceUses.count - 1 do
      listAdd(f^.uses_, PIdent(listGet(root^.ifaceUses, i))^.spelling);
    for i := 0 to root^.implUses.count - 1 do
      listAdd(f^.uses_, PIdent(listGet(root^.implUses, i))^.spelling);
  end;
  for k := 1 to nb do
  begin
    if blocks[k] = nil then
      continue;
    for j := 0 to blocks[k]^.decls.count - 1 do
    begin
      d := PDecl(listGet(blocks[k]^.decls, j));
      t := PTopDecl(arenaAlloc(sizeof(TTopDecl)));
      t^.d := d;
      t^.section := '';
      bufInit(t^.repl);
      t^.hasRepl := false;
      t^.pending := false;
      t^.headingOnly := false;
      t^.unusable := false;
      t^.included := false;
      b := -1;
      e := -1;
      case d^.kind of
        dkConst:
          begin
            if d^.constInit = nil then
              continue;
            t^.section := 'const';
            b := locOffset(d^.ident.loc, fid2);
            e := d^.constInit^.e - smFileStart(fid2);
          end;
        dkType:
          begin
            ts := d^.typeSpec;
            if (ts = nil) or d^.tgGeneric then
              continue;
            t^.section := 'type';
            b := locOffset(d^.ident.loc, fid2);
            e := ts^.e - smFileStart(fid2);
            if (ts^.kind = tsEnum) and not ts^.enumScoped then
              for i := 0 to ts^.enumNames.count - 1 do
                addNameEntry(f^.byName, PIdent(listGet(ts^.enumNames, i))^.name, t);
          end;
        dkVar:
          begin
            if d^.varSpec = nil then
              continue;
            b := locOffset(d^.ident.loc, fid2);
            if (d^.init <> nil) and not d^.isInitVar then
            begin
              t^.section := 'const';
              e := d^.init^.e - smFileStart(fid2);
            end
            else
            begin { (a, b: T: each alone) }
              t^.section := 'var';
              e := b + length(nameStr(d^.ident.spelling));
              bufAddName(t^.repl, d^.ident.spelling);
              bufAdd(t^.repl, ': ');
              bufAdd(t^.repl, specText(d^.varSpec));
              t^.hasRepl := true;
              t^.unusable := true;
            end;
          end;
        dkRoutine:
          begin
            if identValid(d^.methodClassName) or d^.isGeneric then
              continue;
            if d^.isCompiletime then
            begin
              addNameEntry(compiletimeFns, d^.ident.name, d);
              anyCompiletime := true;
            end;
            b := locOffset(d^.loc, fid2);
            t^.unusable := not d^.isCompiletime;
            bodyNode := nil;
            if d^.body <> nil then
              bodyNode := d^.body^.body;
            if (bodyNode <> nil) and d^.isCompiletime then
              e := bodyNode^.e - smFileStart(fid2)
            else
            begin
              e := headingEnd(smFileData(fid2), b, smFileLen(fid2));
              t^.headingOnly := d^.body = nil;
            end;
          end;
      else
        continue;
      end;
      if (b < 0) or (e < b) then
        continue;
      t^.fid := fid2;
      t^.b := b;
      t^.e := e;
      addNameEntry(f^.byName, d^.ident.name, t);
      listAdd(f^.decls, t);
    end;
  end;
  listAdd(files, f);
end;

{ fn, if declaration t of file f is  const X = F(args)  for compiletime F
  (not hidden by a routine of f that is not compiletime). }
function compiletimeConstFn(f: PFileInfo; t: PTopDecl): PDecl;
var
  d: PDecl;
  name: PName;
  own, fns: PPtrList;
  hidden: boolean;
  i: longint;
  r: PDecl;
begin
  compiletimeConstFn := nil;
  d := t^.d;
  if d^.kind <> dkConst then
    exit;
  if d^.constInit^.kind = nkCall then
    name := d^.constInit^.callee.name
  else if d^.constInit^.kind = nkNameRef then
    name := d^.constInit^.ref.name
  else
    exit;
  fns := PPtrList(mapGet(compiletimeFns, name^.id));
  if fns = nil then
    exit;
  own := PPtrList(mapGet(f^.byName, name^.id));
  if own <> nil then
  begin
    hidden := true;
    for i := 0 to own^.count - 1 do
    begin
      r := PTopDecl(listGet(own^, i))^.d;
      if (r^.kind = dkRoutine) and r^.isCompiletime then
        hidden := false;
    end;
    if hidden then
      exit;
  end;
  compiletimeConstFn := PDecl(listGet(fns^, 0));
end;

function homeOf(d: PDecl): PFileInfo;
var
  i, j: longint;
  f: PFileInfo;
begin
  homeOf := nil;
  for i := 0 to files.count - 1 do
  begin
    f := PFileInfo(listGet(files, i));
    for j := 0 to f^.decls.count - 1 do
      if PTopDecl(listGet(f^.decls, j))^.d = d then
        exit(f);
  end;
end;

{ A type declaration named key: in prefer first, then in every file. }
function findType(key: PName; prefer: PFileInfo): PDecl;
  function look(f: PFileInfo): PDecl;
  var
    l: PPtrList;
    i: longint;
    d: PDecl;
  begin
    look := nil;
    l := PPtrList(mapGet(f^.byName, key^.id));
    if l = nil then
      exit;
    for i := 0 to l^.count - 1 do
    begin
      d := PTopDecl(listGet(l^, i))^.d;
      if (d^.kind = dkType) and (d^.ident.name = key) then
        exit(d);
    end;
  end;
var
  i: longint;
  d: PDecl;
begin
  if prefer <> nil then
  begin
    d := look(prefer);
    if d <> nil then
      exit(d);
  end;
  for i := 0 to files.count - 1 do
  begin
    d := look(PFileInfo(listGet(files, i)));
    if d <> nil then
      exit(d);
  end;
  findType := nil;
end;

{ The predeclared types a printer knows: 'i'nteger, 'r'eal, 's'tring,
  'c'har, 'b'oolean; ' ' if not one. }
function predeclaredKind(const n: string): char;
begin
  if (n = 'integer') or (n = 'longint') or (n = 'int64') or
     (n = 'shortint') or (n = 'smallint') or (n = 'byte') or
     (n = 'word') or (n = 'cardinal') or (n = 'longword') or
     (n = 'dword') or (n = 'qword') or (n = 'uint64') or
     (n = 'int8') or (n = 'int16') or (n = 'int32') or
     (n = 'uint8') or (n = 'uint16') or (n = 'uint32') or
     (n = 'nativeint') or (n = 'nativeuint') or (n = 'sizeint') or
     (n = 'sizeuint') or (n = 'ptrint') or (n = 'ptruint') then
    predeclaredKind := 'i'
  else if (n = 'real') or (n = 'double') or (n = 'single') or
          (n = 'extended') then
    predeclaredKind := 'r'
  else if (n = 'string') or (n = 'ansistring') or (n = 'shortstring') or
          (n = 'rawbytestring') or (n = 'utf8string') then
    predeclaredKind := 's'
  else if (n = 'char') or (n = 'ansichar') or (n = 'widechar') then
    predeclaredKind := 'c'
  else if (n = 'boolean') or (n = 'bytebool') or (n = 'wordbool') or
          (n = 'longbool') then
    predeclaredKind := 'b'
  else
    predeclaredKind := ' ';
end;

function isStructured(ts: PTypeSpec; home: PFileInfo): boolean;
var
  guard: longint;
  td: PDecl;
begin
  isStructured := false;
  guard := 0;
  while (ts <> nil) and (guard < 64) do
  begin
    guard := guard + 1;
    if ts^.kind in [tsArray, tsRecord] then
      exit(true);
    if ts^.kind <> tsNamed then
      exit;
    td := findType(ts^.tname.name, home);
    if td = nil then
      exit;
    ts := td^.typeSpec;
  end;
end;

{ Pascal statements (to code) that write the value of e (of type ts) as a
  constant; locals gets the declarations they need. }
function printer(ts: PTypeSpec; const e: string; home: PFileInfo;
                 var code, locals: TBuf; var counter: longint;
                 var err: string; const enumName: string): boolean;
var
  td: PDecl;
  i, j: longint;
  prefix, lo, k, t, iv, elem, closing: string;
  fd: PDecl;
  first: boolean;
  nr: PNode;
  l: PPtrList;
  f: PFileInfo;
begin
  printer := false;
  case ts^.kind of
    tsNamed:
      begin
        if ts^.tquals.count = 0 then
        begin
          td := findType(ts^.tname.name, home);
          if td <> nil then
            exit(printer(td^.typeSpec, e, homeOf(td), code, locals, counter,
                         err, nameStr(td^.ident.spelling)));
        end;
        case predeclaredKind(nameStr(ts^.tname.name)) of
          'i': bufAdd(code, 'write(' + e + ');');
          'r': bufAdd(code, 'p3ct_real(' + e + ');');
          's': bufAdd(code, 'p3ct_str(' + e + ');');
          'c': bufAdd(code, 'write(''#'', ord(' + e + '));');
          'b': bufAdd(code, 'if ' + e + ' then write(''True'') else write(''False'');');
        else
          begin
            err := 'type ''' + nameStr(ts^.tname.spelling) +
                   ''' is not declared in the program or its units'' ' +
                   'sources, nor an ordinal, real or string type';
            exit;
          end;
        end;
        printer := true;
      end;
    tsEnum:
      begin
        if ts^.enumScoped and (enumName = '') then
        begin
          err := 'an anonymous scoped enumeration cannot be written as a ' +
                 'constant';
          exit;
        end;
        prefix := '';
        if ts^.enumScoped then
          prefix := enumName + '.';
        bufAdd(code, 'case ord(' + e + ') of ');
        for i := 0 to ts^.enumNames.count - 1 do
        begin
          bufAddInt(code, i);
          bufAdd(code, ': write(''' + prefix);
          bufAddName(code, PIdent(listGet(ts^.enumNames, i))^.spelling);
          bufAdd(code, '''); ');
        end;
        bufAdd(code, 'end;');
        printer := true;
      end;
    tsSubrange:
      begin
        lo := nodeText(ts^.lo);
        if (lo <> '') and ((lo[1] = '''') or (lo[1] = '#')) then
        begin
          bufAdd(code, 'write(''#'', ord(' + e + '));');
          exit(true);
        end;
        nr := ts^.lo;
        if nr^.kind = nkNameRef then
          for i := 0 to files.count - 1 do
          begin
            f := PFileInfo(listGet(files, i));
            l := PPtrList(mapGet(f^.byName, nr^.ref.name^.id));
            if l = nil then
              continue;
            for j := 0 to l^.count - 1 do
            begin
              td := PTopDecl(listGet(l^, j))^.d;
              if (td^.kind = dkType) and (td^.typeSpec^.kind = tsEnum) then
                exit(printer(td^.typeSpec, e, f, code, locals, counter, err,
                             nameStr(td^.ident.spelling)));
            end;
          end;
        bufAdd(code, 'write(' + e + ');');
        printer := true;
      end;
    tsString:
      begin
        bufAdd(code, 'p3ct_str(' + e + ');');
        printer := true;
      end;
    tsArray:
      begin
        elem := e;
        closing := '';
        for i := 0 to ts^.indexes.count - 1 do
        begin
          k := intToStr(counter);
          counter := counter + 1;
          t := 'p3ct_t' + k;
          iv := 'p3ct_i' + k;
          bufAdd(locals, 'type ' + t + ' = ' +
                 specText(PTypeSpec(listGet(ts^.indexes, i))) + '; var ' +
                 iv + ': ' + t + '; ');
          bufAdd(code, 'write(''(''); for ' + iv + ' := low(' + t +
                 ') to high(' + t + ') do begin if ' + iv + ' <> low(' + t +
                 ') then write('', ''); ');
          elem := elem + '[' + iv + ']';
          closing := 'end; write('')'');' + closing; { (innermost first) }
        end;
        if not printer(ts^.element, elem, home, code, locals, counter, err, '') then
          exit;
        bufAdd(code, closing);
        printer := true;
      end;
    tsRecord:
      begin
        if (ts^.fields <> nil) and (ts^.fields^.variant <> nil) then
        begin
          err := 'a record with variant parts cannot be written as a constant';
          exit;
        end;
        if (ts^.fields = nil) or (ts^.fields^.fixed.count = 0) then
        begin
          err := 'a record without fields cannot be written as a constant';
          exit;
        end;
        bufAdd(code, 'write(''('');');
        first := true;
        for i := 0 to ts^.fields^.fixed.count - 1 do
        begin
          fd := PDecl(listGet(ts^.fields^.fixed, i));
          bufAdd(code, 'write(''');
          if not first then
            bufAdd(code, '; ');
          bufAddName(code, fd^.ident.spelling);
          bufAdd(code, ': '');');
          first := false;
          if not printer(fd^.fieldSpec, e + '.' + nameStr(fd^.ident.spelling),
                         home, code, locals, counter, err, '') then
            exit;
        end;
        bufAdd(code, 'write('')'');');
        printer := true;
      end;
    tsSet: { (p3 has no set constants yet) }
      err := 'a set cannot be written as a constant';
  else
    err := 'a value of type ''' + specText(ts) +
           ''' cannot be written as a constant (pointers, objects, dynamic ' +
           'arrays, files and procedural types cannot)';
  end;
end;

procedure addPrinterHelpers(var o: TBuf);
begin
  bufAdd(o, 'procedure p3ct_str(const s: ansistring);' + #10);
  bufAdd(o, 'var i: longint; q: boolean;' + #10);
  bufAdd(o, 'begin' + #10);
  bufAdd(o, '  if length(s) = 0 then begin write(''''''''''''); exit end;' + #10);
  bufAdd(o, '  q := false;' + #10);
  bufAdd(o, '  for i := 1 to length(s) do' + #10);
  bufAdd(o, '    if (ord(s[i]) >= 32) and (ord(s[i]) < 127) then begin' + #10);
  bufAdd(o, '      if not q then begin write(''''''''); q := true end;' + #10);
  bufAdd(o, '      if s[i] = '''''''' then write('''''''''''') else write(s[i]);' + #10);
  bufAdd(o, '    end else begin' + #10);
  bufAdd(o, '      if q then begin write(''''''''); q := false end;' + #10);
  bufAdd(o, '      write(''#'', ord(s[i]));' + #10);
  bufAdd(o, '    end;' + #10);
  bufAdd(o, '  if q then write('''''''');' + #10);
  bufAdd(o, 'end;' + #10);
  bufAdd(o, 'procedure p3ct_real(x: double);' + #10);
  bufAdd(o, 'begin' + #10);
  bufAdd(o, '  if (x <> x) or (x - x <> 0) then write(''@p3ct-nonfinite@'')' + #10);
  bufAdd(o, '  else write(x:30);' + #10);
  bufAdd(o, 'end;' + #10);
end;

{ --- Evaluation --- }

function modeName(m: TLangMode): string;
begin
  case m of
    lmISO: modeName := 'iso';
    lmTP: modeName := 'tp';
  else
    modeName := 'objfpc';
  end;
end;

{ b with every from replaced by to }
procedure replaceAll(var b: TBuf; const from, too: string);
var
  r: TBuf;
  i, j: int64;
  match: boolean;
begin
  if from = '' then
    exit;
  bufInit(r);
  i := 0;
  while i < b.len do
  begin
    match := i + length(from) <= b.len;
    j := 0;
    while match and (j < length(from)) do
    begin
      match := b.data^[i + j] = from[j + 1];
      j := j + 1;
    end;
    if match then
    begin
      bufAdd(r, too);
      i := i + length(from);
    end
    else
    begin
      bufAddChar(r, b.data^[i]);
      i := i + 1;
    end;
  end;
  bufFree(b);
  b := r;
end;

function evaluate(f: PFileInfo; x: PTopDecl; fn: PDecl; var value: TBuf;
                  var resultTS: PTypeSpec): boolean;
var
  c: PDecl;
  msg: TBuf;
  seen: TIntMap;
  work: TPtrList;
  n: PName;
  l: PPtrList;
  i, j, line, col, fid, counter, rc: longint;
  t: PTopDecl;
  r: PDecl;
  ct, failed: boolean;
  fns: PPtrList;
  src, dataP: PCharArray;
  callB, callE, outLen, m, eol: int64;
  o, locals, code, helpers, errText, outText: TBuf;
  args: TPtrList;
  timedOut: boolean;
  curLine, curCol: longint;
  kw, err, genDir, ctDir, base, srcPath, exe, outPath, errPath, stampPath,
    self, fileDir, inputDir, stamp, a: string;
  key: int64;
  exeData: PCharArray;
  exeLen: int64;
  uses_: TPtrList;

  procedure fail(const what: string);
  begin
    bufInit(msg);
    bufAdd(msg, 'cannot compute constant ''');
    bufAddName(msg, c^.ident.spelling);
    bufAdd(msg, ''': ' + what);
    diagReportBuf(sevError, c^.ident.loc, msg, identRange(c^.ident), noRange);
    bufFree(msg);
  end;

  procedure failBuf(const what: TBuf);
  begin
    bufInit(msg);
    bufAdd(msg, 'cannot compute constant ''');
    bufAddName(msg, c^.ident.spelling);
    bufAdd(msg, ''': ');
    bufAddChars(msg, what.data, 0, what.len);
    diagReportBuf(sevError, c^.ident.loc, msg, identRange(c^.ident), noRange);
    bufFree(msg);
  end;

  procedure append(p: PCharArray; b, e: int64);
  var
    q: int64;
  begin
    for q := b to e - 1 do
      if p^[q] = #10 then
      begin
        curLine := curLine + 1;
        curCol := 1;
      end
      else
        curCol := curCol + 1;
    bufAddChars(o, p, b, e - b);
  end;

  procedure appendStr(const s: string);
  var
    q: longint;
  begin
    for q := 1 to length(s) do
      if s[q] = #10 then
      begin
        curLine := curLine + 1;
        curCol := 1;
      end
      else
        curCol := curCol + 1;
    bufAdd(o, s);
  end;

  procedure appendBuf(const b: TBuf);
  begin
    append(b.data, 0, b.len);
  end;

begin
  evaluate := false;
  c := x^.d;
  resultTS := fn^.resultSpec;
  if x^.fid <> f^.fid then
  begin
    fail('a compiletime constant must be in the file itself, not in an ' +
         'included one');
    exit;
  end;
  if resultTS = nil then
  begin
    fail('''' + nameStr(fn^.ident.spelling) + ''' has no result type');
    exit;
  end;

  { --- the declarations it needs --- }
  src := smFileData(f^.fid);
  callB := c^.constInit^.b - smFileStart(f^.fid);
  callE := x^.e;
  mapInit(seen);
  listInit(work);
  scanIdentifiers(src, callB, callE, seen, work);
  for i := 0 to f^.decls.count - 1 do
    PTopDecl(listGet(f^.decls, i))^.included := false;
  while work.count > 0 do
  begin
    n := PName(listPop(work));
    l := PPtrList(mapGet(f^.byName, n^.id));
    if l = nil then
      continue;
    for i := 0 to l^.count - 1 do
    begin
      t := PTopDecl(listGet(l^, i));
      if t = x then
      begin
        fail('its value depends on itself');
        exit;
      end;
      r := t^.d;
      if (r^.kind = dkRoutine) and not r^.isCompiletime then
      begin
        { (the definition of a compiletime heading is marked on either) }
        ct := false;
        fns := PPtrList(mapGet(compiletimeFns, n^.id));
        if fns <> nil then
          for j := 0 to fns^.count - 1 do
            if PDecl(listGet(fns^, j))^.ident.name = r^.ident.name then
              ct := true;
        if ct then
        begin
          t^.unusable := false;
          if (r^.body <> nil) and (r^.body^.body <> nil) then
            t^.e := r^.body^.body^.e - smFileStart(t^.fid);
        end;
      end;
      if t^.pending then
      begin
        fail('it uses compiletime constant ''' + nameStr(r^.ident.spelling) +
             ''', declared after it');
        exit;
      end;
      if t^.included or t^.unusable then
      begin
        t^.included := true;
        continue;
      end;
      t^.included := true;
      if t^.hasRepl then
        scanIdentifiers(t^.repl.data, 0, t^.repl.len, seen, work)
      else
        scanIdentifiers(smFileData(t^.fid), t^.b, t^.e, seen, work);
    end;
  end;

  { --- the program, with each declaration on its own line and column --- }
  bufInit(o);
  bufAdd(o, 'program p3ct;');
  listInit(uses_);
  for i := 0 to f^.uses_.count - 1 do
  begin
    ct := false;
    for j := 0 to uses_.count - 1 do
      if lowerStr(nameStr(PName(listGet(uses_, j)))) =
         lowerStr(nameStr(PName(listGet(f^.uses_, i)))) then
        ct := true;
    if not ct then
      listAdd(uses_, listGet(f^.uses_, i));
  end;
  if uses_.count > 0 then
  begin
    bufAdd(o, ' uses ');
    for i := 0 to uses_.count - 1 do
    begin
      if i > 0 then
        bufAdd(o, ', ');
      bufAddName(o, PName(listGet(uses_, i)));
    end;
    bufAdd(o, ';');
  end;
  copiedDirectives(src, x^.b, o);
  curLine := 1;
  curCol := o.len + 1;
  for i := 0 to f^.decls.count - 1 do
  begin
    t := PTopDecl(listGet(f^.decls, i));
    if not t^.included then
      continue;
    kw := '';
    if t^.section <> '' then
      kw := ' ' + t^.section;
    smPresumed(smFileStart(t^.fid) + t^.b, fid, line, col);
    if (t^.fid = f^.fid) and (line > curLine) then
    begin
      appendStr(kw);
      while curLine < line do
        appendStr(#10);
      for j := 1 to col - 1 do
        appendStr(' ');
    end
    else
      appendStr(#10 + kw + ' ');
    if t^.hasRepl then
      appendBuf(t^.repl)
    else
      append(smFileData(t^.fid), t^.b, t^.e);
    if t^.unusable and (t^.d^.kind = dkRoutine) then
      appendStr(' external;')
    else if t^.headingOnly then
      appendStr(' compiletime; forward;')
    else
      appendStr(';');
  end;
  bufInit(locals);
  bufInit(code);
  counter := 0;
  err := '';
  if not printer(resultTS, 'p3ct_v', homeOf(fn), code, locals, counter, err,
                 '') then
  begin
    fail('the result of ''' + nameStr(fn^.ident.spelling) + ''': ' + err);
    exit;
  end;
  appendStr(#10);
  bufInit(helpers);
  addPrinterHelpers(helpers);
  appendBuf(helpers);
  bufFree(helpers);
  appendStr('procedure p3ct_out(const p3ct_v: ' + specText(resultTS) + ');' + #10);
  appendBuf(locals);
  appendStr(#10 + 'begin' + #10 + '  ');
  appendBuf(code);
  appendStr(#10 + 'end;' + #10);
  appendStr('begin' + #10 + '  write(''@p3ct@'');' + #10 + '  p3ct_out(');
  append(src, callB, callE);
  appendStr(');' + #10 + '  writeln;' + #10 + 'end.' + #10);

  { --- build and run it, unless its output is known --- }
  if optGenDir = '' then
    genDir := pathJoin(pathDirName(optInput), 'gen')
  else
    genDir := optGenDir;
  genDir := absPath(genDir);
  ctDir := pathJoin(genDir, '.ct');
  sysMakeDirs(ctDir);
  base := generatedFileBase(f^.path) + '.' + nameStr(c^.ident.spelling);
  srcPath := pathJoin(ctDir, base + '.pas');
  exe := pathJoin(ctDir, base);
  outPath := exe + '.out';
  errPath := exe + '.err';
  stampPath := exe + '.stamp';
  self := sysExecutablePath;
  fileDir := absPath(pathDirName(f^.path));
  inputDir := absPath(pathDirName(optInput));

  listInit(args);
  argAdd(args, self);
  { (without warnings about the variables copied only for diagnostics) }
  argAdd(args, '--no-project');
  argAdd(args, '-O0');
  argAdd(args, '--Wno-unused-variable');
  argAdd(args, '--mode=' + modeName(optMode));
  argAdd(args, '--generated-dir=' + genDir);
  argAdd(args, '-I' + fileDir);
  if inputDir <> fileDir then
    argAdd(args, '-I' + inputDir);
  for i := 0 to optIncludes.count - 1 do
    argAdd(args, '-I' + absPath(nameStr(PName(listGet(optIncludes, i)))));
  for i := 0 to optDefines.count - 1 do
    argAdd(args, '-d' + nameStr(PName(listGet(optDefines, i))));
  for i := 0 to optGenerators.count - 1 do
  begin
    a := nameStr(PName(listGet(optGenerators, i)));
    if not isLibraryGenerator(a) then
      a := absPath(a);
    argAdd(args, '--generator=' + a);
  end;
  argAdd(args, '--generator-timeout=' + intToStr(optTimeout));
  argAdd(args, '-o');
  argAdd(args, exe);
  argAdd(args, srcPath);

  { the key: the program, the command, the sources of the compilation's
    files and the compiler itself }
  key := fnvBytes(o.data, o.len, fnvSeed);
  for i := 0 to args.count - 1 do
    key := fnvString(nameStr(PName(listGet(args, i))), key);
  for i := 0 to files.count - 1 do
  begin
    j := PFileInfo(listGet(files, i))^.fid;
    key := fnvBytes(smFileData(j), smFileLen(j), key);
  end;
  if readWhole(self, exeData, exeLen) then
    key := fnvBytes(exeData, exeLen, key);
  stamp := hex16(key);
  bufInit(outText);
  if not (stampIs(stampPath, stamp) and readWhole(outPath, dataP, outLen)) then
  begin
    if not writeBuf(srcPath, o) then
    begin
      listFree(args);
      fail('cannot write ''' + srcPath + '''');
      exit;
    end;
    timedOut := false;
    rc := execArgs(args, '', errPath, errPath, 0, timedOut);
    listFree(args);
    if rc <> 0 then
    begin
      { its diagnostics, with the source file's name (the lines are the
        same) }
      bufInit(errText);
      if readWhole(errPath, dataP, outLen) then
        bufAddChars(errText, dataP, 0, outLen);
      replaceAll(errText, srcPath + ':', f^.path + ':');
      bufWrite(errText, sysErr);
      bufFree(errText);
      bufInit(errText);
      bufAdd(errText, 'the program that computes it (');
      bufAdd(errText, srcPath);
      bufAdd(errText, ') did not compile');
      failBuf(errText);
      bufFree(errText);
      exit;
    end;
    listInit(args);
    argAdd(args, exe);
    timedOut := false;
    rc := execArgs(args, '/dev/null', outPath, errPath, optTimeout, timedOut);
    listFree(args);
    if timedOut then
    begin
      fail('''' + nameStr(fn^.ident.spelling) + ''' ran longer than ' +
           intToStr(optTimeout) + ' seconds');
      exit;
    end;
    if rc <> 0 then
    begin
      bufInit(errText);
      if readWhole(errPath, dataP, outLen) then
        bufAddChars(errText, dataP, 0, outLen);
      replaceAll(errText, srcPath, f^.path);
      while (errText.len > 0) and (errText.data^[errText.len - 1] in [#10, #13]) do
        errText.len := errText.len - 1;
      bufInit(msg);
      bufAdd(msg, '''');
      bufAddName(msg, fn^.ident.spelling);
      bufAdd(msg, ''' failed (exit status ' + intToStr(rc) + ')');
      if errText.len > 0 then
      begin
        bufAdd(msg, ': ');
        bufAddChars(msg, errText.data, 0, errText.len);
      end;
      bufFree(errText);
      errText := msg;
      failBuf(errText);
      exit;
    end;
    if not readWhole(outPath, dataP, outLen) then
    begin
      fail('cannot read ''' + outPath + '''');
      exit;
    end;
    writeStamp(stampPath, stamp);
  end
  else
    listFree(args);
  { the value: after the last @p3ct@, to the end of its line }
  m := -1;
  for i := 0 to outLen - 6 do
    if (dataP^[i] = '@') and (dataP^[i + 1] = 'p') and (dataP^[i + 2] = '3') and
       (dataP^[i + 3] = 'c') and (dataP^[i + 4] = 't') and (dataP^[i + 5] = '@') then
      m := i;
  if m < 0 then
  begin
    fail('its program wrote no value');
    exit;
  end;
  eol := m + 6;
  while (eol < outLen) and (dataP^[eol] <> #10) do
    eol := eol + 1;
  while (eol > m + 6) and (dataP^[eol - 1] in [#13, ' ']) do
    eol := eol - 1;
  bufInit(value);
  bufAddChars(value, dataP, m + 6, eol - m - 6);
  failed := false;
  for i := 0 to value.len - 16 do
    if (value.data^[i] = '@') and (value.data^[i + 1] = 'p') and
       (value.data^[i + 5] = '-') then
      failed := true;
  if failed then
  begin
    fail('its value is not a finite number');
    exit;
  end;
  bufFree(o);
  evaluate := true;
end;

function evaluateFile(f: PFileInfo; var changed: boolean): boolean;
var
  i: longint;
  t: PTopDecl;
  fn: PDecl;
  todo, todoFns: TPtrList;
  value, newText, text: TBuf;
  rts: PTypeSpec;
  ok: boolean;
  src: PCharArray;
  srcLen, at: int64;
  edits: TPtrList; { of PTopDecl, in order }
  nl, q: longint;
begin
  evaluateFile := true;
  listInit(todo);
  listInit(todoFns);
  for i := 0 to f^.decls.count - 1 do
  begin
    t := PTopDecl(listGet(f^.decls, i));
    fn := compiletimeConstFn(f, t);
    if fn <> nil then
    begin
      t^.pending := true;
      listAdd(todo, t);
      listAdd(todoFns, fn);
    end;
  end;
  if todo.count = 0 then
    exit;
  ok := true;
  listInit(edits);
  for i := 0 to todo.count - 1 do
  begin
    t := PTopDecl(listGet(todo, i));
    fn := PDecl(listGet(todoFns, i));
    t^.pending := false;
    if not evaluate(f, t, fn, value, rts) then
    begin
      ok := false;
      continue;
    end;
    if not ok then { (only the errors of the first constant that failed) }
      continue;
    bufInit(newText);
    bufAddName(newText, t^.d^.ident.spelling);
    if isStructured(rts, homeOf(fn)) then
    begin
      bufAdd(newText, ': ');
      bufAdd(newText, specText(rts));
      bufAdd(newText, ' = ');
    end
    else
      bufAdd(newText, ' = ');
    bufAddChars(newText, value.data, 0, value.len);
    { (the lines it took stay: the following ones do not move) }
    nl := countNewlines(smFileData(f^.fid), t^.b, t^.e);
    for q := 1 to nl do
      bufAddChar(newText, #10);
    t^.repl := newText;
    t^.hasRepl := true;
    listAdd(edits, t);
  end;
  if not ok then { (the files after it would report the same errors again) }
    exit(false);
  if edits.count = 0 then
    exit;
  src := smFileData(f^.fid);
  srcLen := smFileLen(f^.fid);
  bufInit(text);
  at := 0;
  for i := 0 to edits.count - 1 do
  begin
    t := PTopDecl(listGet(edits, i));
    bufAddChars(text, src, at, t^.b - at);
    bufAddChars(text, t^.repl.data, 0, t^.repl.len);
    at := t^.e;
  end;
  bufAddChars(text, src, at, srcLen - at);
  bufAddChar(text, #0);
  addSplice(f^.path, text.data, text.len - 1);
  changed := true;
end;

function evaluateCompiletime(root: PDecl; const units: TPtrList;
                             mode: TLangMode; const inputFile,
                             generatedDir: string;
                             const includeDirs, defines,
                             generators: TPtrList; timeoutSecs: longint;
                             var restart: boolean): boolean;
var
  i: longint;
  u: PDecl;
  name: string;
  changed: boolean;
begin
  restart := false;
  evaluateCompiletime := true;
  listInit(files);
  mapInit(compiletimeFns);
  anyCompiletime := false;
  optMode := mode;
  optInput := inputFile;
  optGenDir := generatedDir;
  optIncludes := includeDirs;
  optDefines := defines;
  optGenerators := generators;
  optTimeout := timeoutSecs;
  for i := 0 to units.count - 1 do
  begin
    u := PDecl(listGet(units, i));
    name := smFileName(smFileOf(u^.loc));
    if (length(name) > 4) and (copy(name, length(name) - 3, 4) = '.p3i') then
      continue;
    addFile(u);
  end;
  addFile(root);
  if not anyCompiletime then
    exit;
  changed := false;
  for i := 0 to files.count - 1 do
    if not evaluateFile(PFileInfo(listGet(files, i)), changed) then
      exit(false);
  restart := changed;
end;

end.
