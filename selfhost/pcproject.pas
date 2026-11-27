{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
unit pcproject;
{ The project file, p3.json, of the self-hosted compiler; a port of
  src/driver/Project.cpp, with the same diagnostics. A project file holds
  settings for compiling a project's programs and units (mode, unit and
  library paths, defines, libraries, optimization, debug info, target,
  warnings, checks) and its main program and output. It is looked for in
  the input file's directory (the current directory without an input file)
  and its parents; --project=<file> names one, --no-project ignores it.
  p3.pas applies the settings under the command line's. Paths in it are
  relative to its directory. }

interface

uses pcsys, pcutil, pcdiag, pclex;

type
  TProjectSettings = record
    path: string;          { the file read ('': none) }
    base: string;          { its directory: '' or ending in '/' }
    hasMain, hasOutput, hasTarget, hasCPU, hasMode, hasOptLevel,
    hasErrorLimit, hasGeneratedDir, hasGeneratorTimeout: boolean;
    main, output, target, cpu: string; { main, output: relative to base }
    generatedDir: string;   { relative to base }
    generatorTimeout: longint;
    mode: TLangMode;
    optLevel, errorLimit: longint;
    { flags, false when absent }
    debug, separateUnits, wholeProgram, warningsAsErrors: boolean;
    { switches, true when absent }
    warnUnusedVariables, warnDeprecated, rangeChecks, overflowChecks: boolean;
    unitPaths, defines, libraryPaths, libraries, generators: TPtrList; { of PName }
  end;

{ Finds (projectFile = '': from inputFile's directory up) and reads the
  project file into s; s.path = '' when there is none. False after
  reporting an error. }
function loadProject(const projectFile, inputFile: string;
                     var s: TProjectSettings): boolean;

{ A path written in the project file, relative to its directory. }
function projectPath(const s: TProjectSettings; const p: string): string;

implementation

{ --- Paths (lexical, '/'-separated, as Project.cpp does them) --- }

{ path made absolute without "." and ".." components }
function absolutePath(const p: string): string;
var
  full, c, r: string;
  parts: TPtrList;
  i, j: longint;
  dummy: pointer;
begin
  full := p;
  if (full = '') or (full[1] <> '/') then
    full := sysGetCwd + '/' + p;
  listInit(parts);
  i := 1;
  while i <= length(full) + 1 do
  begin
    j := i;
    while (j <= length(full)) and (full[j] <> '/') do
      j := j + 1;
    c := copy(full, i, j - i);
    if c = '..' then
    begin
      if parts.count > 0 then
        dummy := listPop(parts);
    end
    else if (c <> '') and (c <> '.') then
      listAdd(parts, intern(c));
    i := j + 1;
  end;
  r := '';
  for i := 0 to parts.count - 1 do
    r := r + '/' + nameStr(PName(listGet(parts, i)));
  listFree(parts);
  if r = '' then
    r := '/';
  absolutePath := r;
end;

{ the directory of an absolute path ('/' for '/x'), or '' for '/' }
function parentDir(const p: string): string;
var
  s: longint;
begin
  if p = '/' then
    exit('');
  s := length(p);
  while (s > 0) and (p[s] <> '/') do
    s := s - 1;
  if s = 1 then
    parentDir := '/'
  else
    parentDir := copy(p, 1, s - 1);
end;

function joinPath(const dir, name: string): string;
begin
  if dir[length(dir)] = '/' then
    joinPath := dir + name
  else
    joinPath := dir + '/' + name;
end;

function projectPath(const s: TProjectSettings; const p: string): string;
begin
  if (p <> '') and (p[1] = '/') then
    projectPath := p
  else
    projectPath := s.base + p;
end;

{ --- JSON --- }

type
  TJKind = (jObject, jArray, jString, jNumber, jBool, jNull);
  PJValue = ^TJValue;
  TJValue = record
    kind: TJKind;
    b, e: int64;     { offsets in the file }
    text: string;    { string: the value; number: as written (its start) }
    numLen: int64;   { number: its length }
    bool: boolean;
    members: TPtrList; { object: of PJMember }
    elems: TPtrList;   { array: of PJValue }
  end;
  PJMember = ^TJMember;
  TJMember = record
    name: string;
    nb, ne: int64;
    value: PJValue;
  end;

var
  data: PCharArray;
  len, pos: int64;
  start: TLoc;
  depth: longint;

procedure jsonError(off: int64; const msg: string);
begin
  diagError(start + off, msg);
end;

function peek: char;
begin
  if pos < len then
    peek := data^[pos]
  else
    peek := #0;
end;

procedure skipSpace;
begin
  while (pos < len) and (data^[pos] in [' ', #9, #10, #13]) do
    pos := pos + 1;
end;

function hexValue(c: char): longint;
begin
  if c in ['0'..'9'] then
    hexValue := ord(c) - ord('0')
  else if c in ['a'..'f'] then
    hexValue := ord(c) - ord('a') + 10
  else if c in ['A'..'F'] then
    hexValue := ord(c) - ord('A') + 10
  else
    hexValue := -1;
end;

{ a string at pos (its opening quote): its value in res }
function readString(var res: string): boolean;
var
  b, escB, n: int64;
  c, e: char;
  v, i, h: longint;

  procedure add(ch: char);
  begin
    n := n + 1;
    if length(res) < 255 then
      res := res + ch;
  end;

begin
  readString := false;
  b := pos;
  pos := pos + 1;
  res := '';
  n := 0;
  while true do
  begin
    if (pos >= len) or (data^[pos] = #10) then
    begin
      jsonError(b, 'unterminated string');
      exit;
    end;
    c := data^[pos];
    if c = '"' then
    begin
      pos := pos + 1;
      break;
    end;
    if ord(c) < 32 then
    begin
      jsonError(pos, 'control character in a string; write it as an escape such as \t');
      exit;
    end;
    if c <> '\' then
    begin
      add(c);
      pos := pos + 1;
      continue;
    end;
    escB := pos;
    pos := pos + 1;
    e := peek;
    pos := pos + 1;
    case e of
      '"': add('"');
      '\': add('\');
      '/': add('/');
      'b': add(#8);
      'f': add(#12);
      'n': add(#10);
      'r': add(#13);
      't': add(#9);
      'u':
        begin
          v := 0;
          for i := 1 to 4 do
          begin
            h := hexValue(peek);
            if h < 0 then
            begin
              jsonError(escB, 'invalid escape sequence in a string');
              exit;
            end;
            v := v * 16 + h;
            pos := pos + 1;
          end;
          if v < $80 then
            add(chr(v))
          else if v < $800 then
          begin
            add(chr($C0 or (v shr 6)));
            add(chr($80 or (v and $3F)));
          end
          else
          begin
            add(chr($E0 or (v shr 12)));
            add(chr($80 or ((v shr 6) and $3F)));
            add(chr($80 or (v and $3F)));
          end;
        end;
    else
      begin
        jsonError(escB, 'invalid escape sequence in a string');
        exit;
      end;
    end;
  end;
  if n > 255 then
  begin
    jsonError(b, 'string longer than 255 characters');
    exit;
  end;
  readString := true;
end;

function isDigit(c: char): boolean;
begin
  isDigit := c in ['0'..'9'];
end;

function readNumber(v: PJValue): boolean;
var
  b, i: int64;
  ok: boolean;
begin
  b := pos;
  if peek = '-' then
    pos := pos + 1;
  ok := isDigit(peek);
  if peek = '0' then
    pos := pos + 1
  else
    while isDigit(peek) do
      pos := pos + 1;
  if ok and (peek = '.') then
  begin
    pos := pos + 1;
    ok := isDigit(peek);
    while isDigit(peek) do
      pos := pos + 1;
  end;
  if ok and (peek in ['e', 'E']) then
  begin
    pos := pos + 1;
    if peek in ['+', '-'] then
      pos := pos + 1;
    ok := isDigit(peek);
    while isDigit(peek) do
      pos := pos + 1;
  end;
  if not ok then
  begin
    jsonError(b, 'invalid number');
    exit(false);
  end;
  v^.kind := jNumber;
  v^.numLen := pos - b;
  v^.text := '';
  i := b;
  while (i < pos) and (length(v^.text) < 255) do
  begin
    v^.text := v^.text + data^[i];
    i := i + 1;
  end;
  readNumber := true;
end;

function readWord(v: PJValue): boolean;
var
  b: int64;
  w: string;
begin
  b := pos;
  w := '';
  while peek in ['a'..'z'] do
  begin
    if length(w) < 10 then
      w := w + peek;
    pos := pos + 1;
  end;
  readWord := true;
  if (pos - b <= 5) and ((w = 'true') or (w = 'false')) then
  begin
    v^.kind := jBool;
    v^.bool := w = 'true';
  end
  else if (pos - b = 4) and (w = 'null') then
    v^.kind := jNull
  else
  begin
    jsonError(b, 'expected a value (an object, array, string, number, true, ' +
                 'false or null)');
    readWord := false;
  end;
end;

function readValue: PJValue; forward;

function readMembers(v: PJValue): boolean;
var
  m: PJMember;
begin
  readMembers := false;
  skipSpace;
  if peek = '}' then
  begin
    pos := pos + 1;
    exit(true);
  end;
  while true do
  begin
    skipSpace;
    if peek <> '"' then
    begin
      jsonError(pos, 'expected a string (the name of a setting)');
      exit;
    end;
    m := PJMember(arenaAlloc(sizeof(TJMember)));
    m^.nb := pos;
    if not readString(m^.name) then
      exit;
    m^.ne := pos;
    skipSpace;
    if peek <> ':' then
    begin
      jsonError(pos, 'expected '':'' after the name');
      exit;
    end;
    pos := pos + 1;
    m^.value := readValue;
    if m^.value = nil then
      exit;
    listAdd(v^.members, m);
    skipSpace;
    if peek = ',' then
    begin
      pos := pos + 1;
      continue;
    end;
    if peek = '}' then
    begin
      pos := pos + 1;
      exit(true);
    end;
    jsonError(pos, 'expected '','' or ''}''');
    exit;
  end;
end;

function readElements(v: PJValue): boolean;
var
  e: PJValue;
begin
  readElements := false;
  skipSpace;
  if peek = ']' then
  begin
    pos := pos + 1;
    exit(true);
  end;
  while true do
  begin
    e := readValue;
    if e = nil then
      exit;
    listAdd(v^.elems, e);
    skipSpace;
    if peek = ',' then
    begin
      pos := pos + 1;
      continue;
    end;
    if peek = ']' then
    begin
      pos := pos + 1;
      exit(true);
    end;
    jsonError(pos, 'expected '','' or '']''');
    exit;
  end;
end;

function readValue: PJValue;
var
  v: PJValue;
  ok: boolean;
  c: char;
begin
  skipSpace;
  v := PJValue(arenaAlloc(sizeof(TJValue)));
  v^.kind := jNull;
  listInit(v^.members);
  listInit(v^.elems);
  v^.b := pos;
  depth := depth + 1;
  if depth > 64 then
  begin
    jsonError(pos, 'values nested too deeply');
    exit(nil);
  end;
  c := peek;
  if c = '{' then
  begin
    pos := pos + 1;
    v^.kind := jObject;
    ok := readMembers(v);
  end
  else if c = '[' then
  begin
    pos := pos + 1;
    v^.kind := jArray;
    ok := readElements(v);
  end
  else if c = '"' then
  begin
    v^.kind := jString;
    ok := readString(v^.text);
  end
  else if (c = '-') or isDigit(c) then
    ok := readNumber(v)
  else if c in ['a'..'z'] then
    ok := readWord(v)
  else
  begin
    jsonError(pos, 'expected a value (an object, array, string, number, true, ' +
                   'false or null)');
    ok := false;
  end;
  depth := depth - 1;
  v^.e := pos;
  if ok then
    readValue := v
  else
    readValue := nil;
end;

{ the whole file: one object }
function readFile: PJValue;
var
  v: PJValue;
begin
  readFile := nil;
  skipSpace;
  if peek <> '{' then
  begin
    jsonError(pos, 'a project file holds an object: { "main": ... }');
    exit;
  end;
  v := readValue;
  if v = nil then
    exit;
  skipSpace;
  if pos < len then
  begin
    jsonError(pos, 'unexpected text after the project''s settings');
    exit;
  end;
  readFile := v;
end;

{ --- Settings --- }

function rangeOf(v: PJValue): TRange;
begin
  rangeOf := makeRange(start + v^.b, start + v^.e);
end;

procedure bad(v: PJValue; const name, what: string);
begin
  diagErrorR(start + v^.b, '''' + name + ''' must be ' + what, rangeOf(v));
end;

function getString(v: PJValue; const name: string; var res: string): boolean;
begin
  getString := v^.kind = jString;
  if v^.kind = jString then
    res := v^.text
  else
    bad(v, name, 'a string');
end;

function getBool(v: PJValue; const name: string; var res: boolean): boolean;
begin
  getBool := v^.kind = jBool;
  if v^.kind = jBool then
    res := v^.bool
  else
    bad(v, name, 'true or false');
end;

{ an integer from 0 to max (at most 9 digits) }
function getInt(v: PJValue; const name: string; max: longint; const what: string;
                var res: longint): boolean;
var
  ok: boolean;
  n: longint;
  i: longint;
begin
  ok := (v^.kind = jNumber) and (v^.numLen > 0) and (v^.numLen <= 9);
  n := 0;
  if ok then
    for i := 1 to length(v^.text) do
      if not isDigit(v^.text[i]) then
        ok := false
      else if ok then
        n := n * 10 + ord(v^.text[i]) - ord('0');
  if ok and (n <= max) then
  begin
    res := n;
    exit(true);
  end;
  bad(v, name, what);
  getInt := false;
end;

procedure getStrings(v: PJValue; const name: string; var res: TPtrList);
var
  ok: boolean;
  i: longint;
begin
  ok := v^.kind = jArray;
  i := 0;
  while ok and (i < v^.elems.count) do
  begin
    ok := PJValue(listGet(v^.elems, i))^.kind = jString;
    i := i + 1;
  end;
  if not ok then
  begin
    bad(v, name, 'an array of strings');
    exit;
  end;
  for i := 0 to v^.elems.count - 1 do
    listAdd(res, intern(PJValue(listGet(v^.elems, i))^.text));
end;

procedure checkDuplicates(obj: PJValue; const prefix: string);
var
  i, j: longint;
  mi: PJMember;
begin
  for i := 0 to obj^.members.count - 1 do
  begin
    mi := PJMember(listGet(obj^.members, i));
    for j := 0 to i - 1 do
      if PJMember(listGet(obj^.members, j))^.name = mi^.name then
      begin
        diagError(start + mi^.nb, 'duplicate setting ''' + prefix + mi^.name + '''');
        break;
      end;
  end;
end;

procedure unknown(m: PJMember; const prefix: string);
begin
  diagWarningR(start + m^.nb, 'unknown setting ''' + prefix + m^.name + '''; ignored',
               makeRange(start + m^.nb, start + m^.ne));
end;

procedure checkSettings(root: PJValue; var s: TProjectSettings);
var
  i, k: longint;
  m, sub: PJMember;
  v, sv: PJValue;
  n, prefix, sn, str: string;
begin
  checkDuplicates(root, '');
  for i := 0 to root^.members.count - 1 do
  begin
    m := PJMember(listGet(root^.members, i));
    v := m^.value;
    n := m^.name;
    if (n = 'name') or (n = 'version') or (n = 'description') then
      getString(v, n, str) { for people and tools: not used by p3 }
    else if n = 'main' then
      s.hasMain := getString(v, n, s.main)
    else if n = 'output' then
      s.hasOutput := getString(v, n, s.output)
    else if n = 'target' then
      s.hasTarget := getString(v, n, s.target)
    else if n = 'cpu' then
      s.hasCPU := getString(v, n, s.cpu)
    else if n = 'mode' then
    begin
      s.hasMode := true;
      if (v^.kind = jString) and (v^.text = 'iso') then
        s.mode := lmISO
      else if (v^.kind = jString) and (v^.text = 'tp') then
        s.mode := lmTP
      else if (v^.kind = jString) and (v^.text = 'objfpc') then
        s.mode := lmObjFPC
      else
      begin
        s.hasMode := false;
        bad(v, n, '"iso", "tp" or "objfpc"');
      end;
    end
    else if n = 'optimize' then
      s.hasOptLevel := getInt(v, n, 3, 'an integer from 0 to 3', s.optLevel)
    else if n = 'errorLimit' then
      s.hasErrorLimit := getInt(v, n, 999999999, 'an integer (0: no limit)',
                                s.errorLimit)
    else if n = 'debug' then
      getBool(v, n, s.debug)
    else if n = 'separateUnits' then
      getBool(v, n, s.separateUnits)
    else if n = 'wholeProgram' then
      getBool(v, n, s.wholeProgram)
    else if n = 'unitPaths' then
      getStrings(v, n, s.unitPaths)
    else if n = 'defines' then
      getStrings(v, n, s.defines)
    else if n = 'libraryPaths' then
      getStrings(v, n, s.libraryPaths)
    else if n = 'libraries' then
      getStrings(v, n, s.libraries)
    else if n = 'generators' then
      getStrings(v, n, s.generators)
    else if n = 'generatedDir' then
      s.hasGeneratedDir := getString(v, n, s.generatedDir)
    else if n = 'generatorTimeout' then
    begin
      s.hasGeneratorTimeout := getInt(v, n, 86400, 'an integer from 1 to 86400',
                                      s.generatorTimeout);
      if s.hasGeneratorTimeout and (s.generatorTimeout = 0) then
      begin
        bad(v, n, 'an integer from 1 to 86400');
        s.hasGeneratorTimeout := false;
      end;
    end
    else if (n = 'warnings') or (n = 'checks') then
    begin
      if v^.kind <> jObject then
      begin
        bad(v, n, 'an object');
        continue;
      end;
      prefix := n + '.';
      checkDuplicates(v, prefix);
      for k := 0 to v^.members.count - 1 do
      begin
        sub := PJMember(listGet(v^.members, k));
        sv := sub^.value;
        sn := prefix + sub^.name;
        if (n = 'warnings') and (sub^.name = 'asErrors') then
          getBool(sv, sn, s.warningsAsErrors)
        else if (n = 'warnings') and (sub^.name = 'unusedVariable') then
          getBool(sv, sn, s.warnUnusedVariables)
        else if (n = 'warnings') and (sub^.name = 'deprecated') then
          getBool(sv, sn, s.warnDeprecated)
        else if (n = 'checks') and (sub^.name = 'range') then
          getBool(sv, sn, s.rangeChecks)
        else if (n = 'checks') and (sub^.name = 'overflow') then
          getBool(sv, sn, s.overflowChecks)
        else
          unknown(sub, prefix);
      end;
    end
    else
      unknown(m, '');
  end;
end;

function loadProject(const projectFile, inputFile: string;
                     var s: TProjectSettings): boolean;
var
  dir, cwd: string;
  sl, fid, errorsBefore: longint;
  root: PJValue;
begin
  loadProject := false;
  s.path := '';
  s.base := '';
  s.hasMain := false;
  s.hasOutput := false;
  s.hasTarget := false;
  s.hasCPU := false;
  s.hasMode := false;
  s.hasOptLevel := false;
  s.hasErrorLimit := false;
  s.hasGeneratedDir := false;
  s.hasGeneratorTimeout := false;
  s.debug := false;
  s.separateUnits := false;
  s.wholeProgram := false;
  s.warningsAsErrors := false;
  s.warnUnusedVariables := true;
  s.warnDeprecated := true;
  s.rangeChecks := true;
  s.overflowChecks := true;
  listInit(s.unitPaths);
  listInit(s.defines);
  listInit(s.libraryPaths);
  listInit(s.libraries);
  listInit(s.generators);
  if projectFile <> '' then
  begin
    s.path := projectFile;
    sl := length(projectFile);
    while (sl > 0) and (projectFile[sl] <> '/') do
      sl := sl - 1;
    s.base := copy(projectFile, 1, sl);
  end
  else
  begin
    if (inputFile = '') or (inputFile = '-') then
      dir := absolutePath('.')
    else
      dir := parentDir(absolutePath(inputFile));
    cwd := absolutePath('.');
    while dir <> '' do
    begin
      if sysIsRegularFile(joinPath(dir, 'p3.json')) then
      begin
        if dir = cwd then
          s.base := ''
        else
          s.base := joinPath(dir, '');
        s.path := s.base + 'p3.json';
        break;
      end;
      dir := parentDir(dir);
    end;
    if s.path = '' then
      exit(true); { no project }
  end;

  if not smLoadFile(s.path, fid) then
  begin
    diagError(noLoc, 'cannot open project file ''' + s.path + ''': ' +
                     sysOpenError(s.path));
    exit;
  end;
  data := smFileData(fid);
  len := smFileLen(fid);
  start := smFileStart(fid);
  pos := 0;
  depth := 0;
  errorsBefore := diagNumErrors;
  root := readFile;
  if root = nil then
    exit;
  checkSettings(root, s);
  loadProject := diagNumErrors = errorsBefore;
end;

end.
