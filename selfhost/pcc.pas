program pcc;
{ The self-hosted Pascal compiler: command line and pipeline. It accepts
  the C++ pcc's options (src/driver/main.cpp) and must behave the same.
  The dumps (--emit-tokens, --emit-parse-tree, --emit-ast) are written
  here; the IR from pcgen goes to pcdriver, which runs clang and the
  linker for -S, -c, -O and executables. }

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcparse, pcprint, pcsema, pcgen,
     pcdriver;

type
  TAction = (actTokens, actParseTree, actAST, actLLVM, actAsm, actObj, actExe);

var
  inputFile, outputFile: string;
  action: TAction;
  mode: TLangMode;
  errorLimit: longint;
  werror, noUnusedVariable: boolean;
  rangeChecksOn, overflowChecksOn, debugInfo, separateUnits: boolean;
  optLevel: longint;
  outBuf: TBuf;

procedure usageError(const msg: string);
begin
  diagError(noLoc, msg);
  sysExit(1);
end;

{ The command-line parser's errors (llvm::cl): "<program>: <msg>". }
function programName: string;
var
  s: string;
  i: longint;
begin
  s := paramstr(0);
  for i := length(s) downto 1 do
    if s[i] = '/' then
    begin
      s := copy(s, i + 1, 255);
      break;
    end;
  programName := s;
end;

procedure clError(const msg: string);
begin
  sysWrite(sysErr, programName + ': ' + msg + #10);
  sysExit(1);
end;

{ How llvm::cl prints an option name: -x for one letter, else --name. }
function dashed(const name: string): string;
begin
  if length(name) = 1 then
    dashed := '-' + name
  else
    dashed := '--' + name;
end;

procedure clOptionError(const name, msg: string);
begin
  clError('for the ' + dashed(name) + ' option: ' + msg);
end;

{ An unsigned number, as llvm::cl reads it for option name. }
function parseUInt(const name, s: string): longint;
var
  i: longint;
  v: int64;
  bad: boolean;
begin
  v := 0;
  bad := (s = '') or (length(s) > 10);
  for i := 1 to length(s) do
    if (s[i] < '0') or (s[i] > '9') then
      bad := true
    else if not bad then
      v := v * 10 + ord(s[i]) - ord('0');
  if bad or (v > 4294967295) then
    clOptionError(name, '''' + s + ''' value invalid for uint argument!');
  if v > 2147483647 then
    v := 2147483647;
  parseUInt := longint(v);
end;

procedure helpLine(const s: string);
begin
  sysWrite(sysOut, s + #10);
end;

procedure printHelp;
begin
  helpLine('OVERVIEW: pcc - Pascal compiler targeting LLVM');
  helpLine('');
  helpLine('USAGE: pcc [options] <input.pas>');
  helpLine('');
  helpLine('OPTIONS:');
  helpLine('');
  helpLine('Generic Options:');
  helpLine('');
  helpLine('  --help                       - Display available options (--help-hidden for more)');
  helpLine('  --help-list                  - Display list of available options (--help-list-hidden for more)');
  helpLine('  --version                    - Display the version of this program');
  helpLine('');
  helpLine('pcc options:');
  helpLine('');
  helpLine('  -I <dir>                     - Add a directory to search for units');
  helpLine('  -L <dir>                     - Add a directory to search for libraries (linking)');
  helpLine('  -O <level>                   - Optimization level (0-3)');
  helpLine('  Output kind (default: linked executable):');
  helpLine('      --emit-tokens               - Dump lexer tokens');
  helpLine('      --emit-ast                  - Dump the parsed AST');
  helpLine('      --emit-parse-tree           - Dump the parse tree (no semantic analysis; for testing)');
  helpLine('      --emit-llvm                 - Emit LLVM IR (.ll)');
  helpLine('      -S                         - Emit target assembly (.s)');
  helpLine('      -c                         - Emit an object file (.o)');
  helpLine('  --Werror                     - Treat warnings as errors');
  helpLine('  --Wno-unused-variable        - Do not warn about unused variables');
  helpLine('  -d <name>                    - Define a symbol for conditional compilation ({$IFDEF})');
  helpLine('  --fdiagnostics-color=<value> - Colorize diagnostics:');
  helpLine('    =auto                      -   If stderr is a tty');
  helpLine('    =always                    -   Always');
  helpLine('    =never                     -   Never');
  helpLine('  --ferror-limit=<N>           - Stop after N errors (0 = no limit)');
  helpLine('  --fno-overflow-checks        - Disable runtime integer overflow checks');
  helpLine('  --fno-range-checks           - Disable runtime range checks (subranges, array indices, ...)');
  helpLine('  --fseparate-units            - Link separately compiled units (each compiled with -c to <unit>.o next to its source)');
  helpLine('  -g                           - Generate debug information');
  helpLine('  -l <name>                    - Link with library <name>');
  helpLine('  --mode=<value>               - Language dialect:');
  helpLine('    =iso                       -   ISO 7185 Pascal');
  helpLine('    =tp                        -   Turbo/Free Pascal extensions');
  helpLine('    =objfpc                    -   Turbo/Free Pascal extensions and classes');
  helpLine('  -o <file>                    - Write output to <file>');
  helpLine('  --target=<triple>            - Target triple (default: host)');
end;

const
  numFlags = 15;
  flagNames: array[1..numFlags] of string[20] = (
    'help', 'help-list', 'version', 'emit-tokens', 'emit-ast', 'emit-parse-tree',
    'emit-llvm', 'S', 'c', 'Werror', 'Wno-unused-variable', 'fno-overflow-checks',
    'fno-range-checks', 'fseparate-units', 'g');
  numValueOpts = 10;
  valueOptNames: array[1..numValueOpts] of string[20] = (
    'o', 'mode', 'fdiagnostics-color', 'ferror-limit', 'target', 'I', 'O', 'L', 'l',
    'd');

function isFlag(const name: string): boolean;
var
  i: longint;
begin
  for i := 1 to numFlags do
    if flagNames[i] = name then
      exit(true);
  isFlag := false;
end;

function isValueOpt(const name: string): boolean;
var
  i: longint;
begin
  for i := 1 to numValueOpts do
    if valueOptNames[i] = name then
      exit(true);
  isValueOpt := false;
end;

{ The edit distance of a and b, replacements allowed
  (llvm::StringRef::edit_distance). }
function editDistance(const a, b: string): longint;
var
  row: array[0..255] of longint;
  i, j, prev, cur: longint;
begin
  for j := 0 to length(b) do
    row[j] := j;
  for i := 1 to length(a) do
  begin
    prev := row[0];
    row[0] := i;
    for j := 1 to length(b) do
    begin
      cur := row[j];
      if a[i] = b[j] then
        row[j] := prev
      else
      begin
        row[j] := prev + 1;
        if row[j - 1] + 1 < row[j] then
          row[j] := row[j - 1] + 1;
        if cur + 1 < row[j] then
          row[j] := cur + 1;
      end;
      prev := cur;
    end;
  end;
  editDistance := row[length(b)];
end;

{ "Unknown command line argument", with the nearest of pcc's options (as
  llvm::cl suggests it). }
procedure unknownArgument(const arg, body: string);
var
  i, d, best: longint;
  lhs, rhs, nearest, name: string;
  hasEq: boolean;
begin
  sysWrite(sysErr, programName + ': Unknown command line argument ''' + arg +
                   '''.  Try: ''' + paramstr(0) + ' --help''' + #10);
  hasEq := pos('=', body) > 0;
  lhs := body;
  rhs := '';
  if hasEq then
  begin
    lhs := copy(body, 1, pos('=', body) - 1);
    rhs := copy(body, pos('=', body) + 1, 255);
  end;
  best := -1;
  nearest := '';
  for i := 1 to numFlags + numValueOpts do
  begin
    if i <= numFlags then
    begin
      name := flagNames[i];
      d := editDistance(name, body);
    end
    else
    begin
      name := valueOptNames[i - numFlags];
      d := editDistance(name, lhs);
    end;
    if (best < 0) or (d < best) then
    begin
      best := d;
      if (i <= numFlags) or not hasEq then
        nearest := dashed(name)
      else
        nearest := dashed(name) + '=' + rhs;
    end;
  end;
  sysWrite(sysErr, programName + ': Did you mean ''' + nearest + '''?' + #10);
  sysExit(1);
end;

var
  targetTriple: string;

procedure parseArgs;
var
  i: longint;
  arg, body, name, value: string;
  hasValue, positionalOnly: boolean;

  { The value of "-name=value" or of the next argument. }
  function takeValue: string;
  begin
    if hasValue then
      exit(value);
    if i >= paramcount then
      clOptionError(name, 'requires a value!');
    i := i + 1;
    takeValue := paramstr(i);
  end;

  procedure positional;
  begin
    if inputFile <> '' then
    begin
      sysWrite(sysErr, programName + ': Too many positional arguments specified!' + #10);
      sysWrite(sysErr, 'Can specify at most 1 positional arguments: See: ' +
                       paramstr(0) + ' --help' + #10);
      sysExit(1);
    end;
    inputFile := arg;
  end;

begin
  inputFile := '';
  outputFile := '';
  targetTriple := '';
  action := actExe;
  mode := lmISO;
  errorLimit := 20;
  werror := false;
  noUnusedVariable := false;
  optLevel := 0;
  rangeChecksOn := true;
  overflowChecksOn := true;
  debugInfo := false;
  separateUnits := false;
  positionalOnly := false;
  i := 1;
  while i <= paramcount do
  begin
    arg := paramstr(i);
    if positionalOnly or (length(arg) < 2) or (arg[1] <> '-') then
    begin
      { '-' alone is standard input }
      positional;
      i := i + 1;
      continue;
    end;
    if arg = '--' then
    begin
      { only positional arguments follow }
      positionalOnly := true;
      i := i + 1;
      continue;
    end;
    { -name / --name, optionally =value }
    body := copy(arg, 2, 255);
    if body[1] = '-' then
      body := copy(body, 2, 255);
    name := body;
    value := '';
    hasValue := pos('=', body) > 0;
    if hasValue then
    begin
      name := copy(body, 1, pos('=', body) - 1);
      value := copy(body, pos('=', body) + 1, 255);
    end;
    { -Idir, -O2, -lname, -Ldir, -dNAME: prefix options }
    if not isFlag(name) and not isValueOpt(name) and (body <> '') and
       ((body[1] = 'I') or (body[1] = 'O') or (body[1] = 'l') or (body[1] = 'L') or
        (body[1] = 'd')) then
    begin
      name := body[1];
      value := copy(body, 2, 255);
      hasValue := true;
    end;
    if (name = 'help') or (name = 'help-list') then
    begin
      printHelp;
      sysExit(0);
    end
    else if name = 'version' then
    begin
      printVersion;
      sysExit(0);
    end
    else if name = 'emit-tokens' then
      action := actTokens
    else if name = 'emit-parse-tree' then
      action := actParseTree
    else if name = 'emit-ast' then
      action := actAST
    else if name = 'emit-llvm' then
      action := actLLVM
    else if name = 'S' then
      action := actAsm
    else if name = 'c' then
      action := actObj
    else if name = 'Werror' then
      werror := true
    else if name = 'Wno-unused-variable' then
      noUnusedVariable := true
    else if name = 'fno-range-checks' then
      rangeChecksOn := false
    else if name = 'fno-overflow-checks' then
      overflowChecksOn := false
    else if name = 'g' then
      debugInfo := true
    else if name = 'fseparate-units' then
      separateUnits := true
    else if name = 'o' then
      outputFile := takeValue
    else if name = 'mode' then
    begin
      value := takeValue;
      if value = 'tp' then
        mode := lmTP
      else if value = 'objfpc' then
        mode := lmObjFPC
      else if value = 'iso' then
        mode := lmISO
      else
        clOptionError(name, 'Cannot find option named ''' + value + '''!');
    end
    else if name = 'fdiagnostics-color' then
    begin
      value := takeValue;
      if (value <> 'auto') and (value <> 'always') and (value <> 'never') then
        clOptionError(name, 'Cannot find option named ''' + value + '''!');
      { colors are not supported yet }
    end
    else if name = 'ferror-limit' then
      errorLimit := parseUInt(name, takeValue)
    else if name = 'target' then
      targetTriple := takeValue
    else if name = 'I' then
      listAdd(includeDirs, intern(takeValue))
    else if name = 'd' then
      listAdd(commandLineDefines, intern(takeValue))
    else if name = 'O' then
      optLevel := parseUInt(name, takeValue)
    else if name = 'l' then
      addLinkerArg('-l' + takeValue)
    else if name = 'L' then
      addLinkerArg('-L' + takeValue)
    else
      unknownArgument(arg, body);
    i := i + 1;
  end;
end;

procedure dumpTokens(fid: longint);
var
  lx: TLexer;
  t: TToken;
  f, line, col: longint;
begin
  lexInit(lx, fid, mode);
  repeat
    lexNext(lx, t);
    smPresumed(t.loc, f, line, col);
    bufAddInt(outBuf, line);
    bufAddChar(outBuf, ':');
    bufAddInt(outBuf, col);
    bufAddChar(outBuf, ' ');
    bufAdd(outBuf, tokenNames[t.kind]);
    bufAdd(outBuf, ' ''');
    bufAddTokenText(outBuf, t);
    bufAddChar(outBuf, '''');
    case t.kind of
      tkIdentifier:
        begin
          bufAdd(outBuf, ' name=');
          bufAddName(outBuf, t.name);
        end;
      tkIntegerLiteral:
        bufAdd(outBuf, ' value=' + uintToStr(t.intValue));
      tkRealLiteral:
        bufAdd(outBuf, ' value=' + formatG(t.realValue, 6));
      tkStringLiteral:
        begin
          bufAdd(outBuf, ' value="');
          bufAddEscaped(outBuf, t.strValue^.chars, 0, t.strValue^.len);
          bufAddChar(outBuf, '"');
        end;
    else
      ;
    end;
    bufAddChar(outBuf, #10);
  until (t.kind = tkEof) or diagShouldStop;
end;

{ --- Units (Driver::loadUnits) --- }

var
  units: TPtrList;      { of PDecl, dependencies first }
  unitsDone: TIntMap;   { name id -> PDecl }
  inProgress: TIntMap;  { name id -> non-nil while being loaded }

var
  selfUnit: PName;      { the unit being compiled, if any: using it is circular }

procedure visitUnit(const u: TIdent);
var
  dirs: TPtrList;
  found, path, base, ext: string;
  i, b, x, fid: longint;
  unitDecl: PDecl;
begin
  if mapHas(unitsDone, u.name^.id) or diagShouldStop then
    exit;
  { objfpc: what pcc has of Free Pascal's SysUtils (Exception) is
    predeclared, so uses sysutils needs no file }
  if (mode = lmObjFPC) and nameEquals(u.name, 'sysutils') then
    exit;
  if (mapGet(inProgress, u.name^.id) <> nil) or (u.name = selfUnit) then
  begin
    diagErrorR(u.loc, 'circular unit reference to ''' + nameStr(u.spelling) + '''',
               identRange(u));
    exit;
  end;
  mapPut(inProgress, u.name^.id, u.name);
  { the input's directory, then -I directories }
  listInit(dirs);
  listAdd(dirs, intern(pathDirName(inputFile)));
  for i := 0 to includeDirs.count - 1 do
    listAdd(dirs, listGet(includeDirs, i));
  found := '';
  for i := 0 to dirs.count - 1 do
    for b := 1 to 2 do
      for x := 1 to 2 do
      begin
        if b = 1 then
          base := nameStr(u.name)
        else
          base := nameStr(u.spelling);
        if x = 1 then
          ext := '.pas'
        else
          ext := '.pp';
        path := pathJoin(nameStr(PName(listGet(dirs, i))), base + ext);
        if (found = '') and sysFileExists(path) then
          found := path;
      end;
  if found = '' then
  begin
    mapPut(inProgress, u.name^.id, nil);
    diagErrorR(u.loc, 'cannot find unit ''' + nameStr(u.spelling) +
               ''' (looked for ' + nameStr(u.name) +
               '.pas next to the program and in -I directories)', identRange(u));
    exit;
  end;
  if not smLoadFile(found, fid) then
  begin
    diagError(u.loc, 'cannot open unit file ''' + found + '''');
    exit;
  end;
  unitDecl := parseUnit(fid, mode);
  if unitDecl = nil then
  begin
    mapPut(inProgress, u.name^.id, nil);
    exit;
  end;
  if unitDecl^.ident.name <> u.name then
    diagError(unitDecl^.ident.loc, 'file ''' + found + ''' declares unit ''' +
              nameStr(unitDecl^.ident.spelling) + ''', expected ''' +
              nameStr(u.spelling) + '''');
  for i := 0 to unitDecl^.ifaceUses.count - 1 do
    visitUnit(PIdent(listGet(unitDecl^.ifaceUses, i))^);
  for i := 0 to unitDecl^.implUses.count - 1 do
    visitUnit(PIdent(listGet(unitDecl^.implUses, i))^);
  mapPut(inProgress, u.name^.id, nil);
  mapPut(unitsDone, u.name^.id, unitDecl);
  listAdd(units, unitDecl);
end;

{ Finds, parses and orders (dependencies first) the units in usesList. }
procedure loadUnits(const usesList: TPtrList);
var
  i: longint;
begin
  if (usesList.count > 0) and (mode = lmISO) then
  begin
    diagError(PIdent(listGet(usesList, 0))^.loc,
              'units are a Turbo/Free Pascal extension; use --mode=tp');
    exit;
  end;
  for i := 0 to usesList.count - 1 do
    visitUnit(PIdent(listGet(usesList, i))^);
end;

{ llvm::sys::path::replace_extension(path, 'o') }
function objectPath(const path: string): string;
var
  i: longint;
  r: string;
begin
  r := path;
  for i := length(r) downto 1 do
  begin
    if r[i] = '/' then
      break;
    if r[i] = '.' then
    begin
      r := copy(r, 1, i - 1);
      break;
    end;
  end;
  objectPath := r + '.o';
end;

{ The objects of separately compiled units, next to their sources; false
  (and an error) if one is missing. }
function findUnitObjects(var objects: TPtrList): boolean;
var
  i, f, line, col: longint;
  u: PDecl;
  obj: string;
begin
  findUnitObjects := false;
  for i := 0 to units.count - 1 do
  begin
    u := PDecl(listGet(units, i));
    smPresumed(u^.loc, f, line, col);
    obj := objectPath(smFileName(f));
    if not sysFileExists(obj) then
    begin
      diagError(noLoc, 'cannot find ''' + obj + ''', the object file of unit ''' +
                       nameStr(u^.ident.spelling) + '''; compile the unit with -c');
      exit;
    end;
    listAdd(objects, intern(obj));
  end;
  findUnitObjects := true;
end;

{ A unit file as the input: its own module (S11). }
function compileUnit(fid: longint): boolean;
var
  u: PDecl;
  isUnit: boolean;
  uses_: TPtrList;
  i: longint;
  layout: string;
  kind: TOutputKind;
  noObjects: TPtrList;
begin
  compileUnit := false;
  u := parseInput(fid, mode, true, isUnit);
  if (u = nil) or diagHasErrors then
    exit;
  listInit(uses_);
  for i := 0 to u^.ifaceUses.count - 1 do
    listAdd(uses_, listGet(u^.ifaceUses, i));
  for i := 0 to u^.implUses.count - 1 do
    listAdd(uses_, listGet(u^.implUses, i));
  selfUnit := u^.ident.name;
  loadUnits(uses_);
  if diagHasErrors or not semaCheckUnit(u, units, mode) then
    exit;
  if action = actExe then
  begin
    diagError(noLoc, '''' + inputFile + ''' is a unit: compile it with -c, -S or ' +
                     '--emit-llvm and link it into a program built with -fseparate-units');
    exit;
  end;
  if not moduleLayout(targetTriple, layout) then
    exit;
  generateUnitModule(u, units, mode, rangeChecksOn, overflowChecksOn, smFileName(fid),
                     targetTriple, layout, debugInfo, optLevel > 0, outBuf);
  case action of
    actLLVM: kind := okLLVM;
    actAsm: kind := okAsm;
  else
    kind := okObj;
  end;
  listInit(noObjects);
  compileUnit := emitOutput(outBuf, smFileName(fid), kind, optLevel, targetTriple,
                            outputFile, noObjects);
end;

{ The input file's name without directory and extension ('a' for standard
  input), like llvm::sys::path::stem. }
function inputStem: string;
var
  s: string;
  i: longint;
begin
  s := inputFile;
  for i := length(s) downto 1 do
    if s[i] = '/' then
    begin
      s := copy(s, i + 1, 255);
      break;
    end;
  if (s = '.') or (s = '..') then
    exit(s);
  for i := length(s) downto 2 do
    if s[i] = '.' then
    begin
      s := copy(s, 1, i - 1);
      break;
    end;
  if (s = '') or (inputFile = '-') then
    s := 'a';
  inputStem := s;
end;

{ Writes outBuf to the output file ('-': standard output). }
function writeOutput: boolean;
begin
  writeOutput := true;
  if outputFile = '-' then
    bufWrite(outBuf, sysOut)
  else if not sysWriteFile(outputFile, outBuf.data, outBuf.len) then
  begin
    diagError(noLoc, 'cannot open output file ''' + outputFile + '''');
    writeOutput := false;
  end;
end;

var
  fid: longint;
  ok, semaOK: boolean;
  prog: PDecl;
  kind: TOutputKind;
  modLayout: string;
  isUnit: boolean;
  unitObjects: TPtrList;
begin
  sysInit;
  diagInit;
  listInit(includeDirs);
  listInit(commandLineDefines);
  bufInit(outBuf);
  parseArgs;
  diagSetErrorLimit(errorLimit);
  diagSetWarningsAsErrors(werror);
  if noUnusedVariable then
    diagDisableWarning('unused-variable');
  if inputFile = '' then
    usageError('no input file');
  if optLevel > 3 then
    usageError('invalid optimization level ''-O' + intToStr(optLevel) +
               ''' (expected 0-3)');
  if not validateTarget(targetTriple) then
    sysExit(1);
  if outputFile = '' then
    case action of
      actLLVM: outputFile := inputStem + '.ll';
      actAsm: outputFile := inputStem + '.s';
      actObj: outputFile := inputStem + '.o';
      actExe:
        if inputFile = '-' then
          outputFile := 'a.out'
        else
          outputFile := inputStem;
    else
      outputFile := '-';
    end;

  if not smLoadFile(inputFile, fid) then
    usageError('cannot open input file ''' + inputFile + ''': ' +
               sysOpenError(inputFile));

  ok := false;
  listInit(units);
  mapInit(unitsDone);
  mapInit(inProgress);
  selfUnit := nil;
  initTypes;
  isUnit := false;
  if action <> actTokens then
  begin
    { a unit file? (Turbo/FPC) }
    prog := parseInput(fid, mode, false, isUnit);
    if isUnit and ((action = actParseTree) or (action = actAST)) then
    begin
      diagError(noLoc, '''' + inputFile + ''' is a unit; --emit-parse-tree and ' +
                       '--emit-ast need a program');
      sysExit(1);
    end;
    if isUnit then
    begin
      if compileUnit(fid) and not diagHasErrors then
        sysExit(0);
      sysExit(1);
    end;
  end;
  case action of
    actParseTree:
      begin
        if not diagHasErrors then
          loadUnits(prog^.progUses);
        printParseTree(outBuf, prog, units);
        ok := writeOutput;
      end;
    actTokens:
      begin
        dumpTokens(fid);
        ok := writeOutput;
      end;
  else
    begin
      { --emit-ast and the code-generating actions: parse, load units and
        check (Sema is skipped after syntax errors) }
      if not diagHasErrors then
        loadUnits(prog^.progUses);
      semaOK := false;
      if not diagHasErrors then
        semaOK := semaCheck(prog, units, mode, separateUnits);
      if action = actAST then
      begin
        { the dump is written even after errors; types appear if Sema ran }
        printAST(outBuf, prog);
        ok := writeOutput;
      end
      else if semaOK then
      begin
        if not moduleLayout(targetTriple, modLayout) then
          sysExit(1);
        listInit(unitObjects);
        if separateUnits and (action = actExe) and not findUnitObjects(unitObjects) then
          sysExit(1);
        generateModule(prog, units, mode, rangeChecksOn, overflowChecksOn,
                       smFileName(fid), targetTriple, modLayout, debugInfo, optLevel > 0,
                       separateUnits, outBuf);
        case action of
          actLLVM: kind := okLLVM;
          actAsm: kind := okAsm;
          actObj: kind := okObj;
        else
          kind := okExe;
        end;
        ok := emitOutput(outBuf, smFileName(fid), kind, optLevel, targetTriple, outputFile,
                         unitObjects);
      end;
    end;
  end;
  if ok and not diagHasErrors then
    sysExit(0);
  sysExit(1);
end.
