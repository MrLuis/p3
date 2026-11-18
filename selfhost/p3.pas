program p3;
{ The self-hosted Pascal compiler: command line and pipeline. It accepts
  the C++ p3's options (src/driver/main.cpp) and must behave the same.
  The dumps (--emit-tokens, --emit-parse-tree, --emit-ast) are written
  here; the IR from pcgen goes to pcdriver, which runs clang and the
  linker for -S, -c, -O and executables. }

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcparse, pcprint, pcsema, pcgen, pcproject,
     pcdriver;

type
  TAction = (actTokens, actParseTree, actAST, actLLVM, actAsm, actObj, actExe);

var
  inputFile, outputFile: string;
  action: TAction;
  mode: TLangMode;
  errorLimit: longint;
  werror, noUnusedVariable, noDeprecated: boolean;
  rangeChecksOn, overflowChecksOn, debugInfo, separateUnits: boolean;
  { -ffast-math: set the fast-math flags on floating-point instructions }
  fastMath: boolean;
  { -fkeep-unused: keep the routines and data nothing reachable refers to }
  keepUnused: boolean;
  optLevel: longint;
  { -fzero-init (1) / -fno-zero-init (-1), the last one given; 0: neither
    (local arrays, records and strings start zeroed at -O0 only) }
  zeroInitOpt: longint;
  outBuf: TBuf;

{ Do local arrays, records and strings start zeroed? }
function zeroInit: boolean;
begin
  zeroInit := (zeroInitOpt > 0) or ((zeroInitOpt = 0) and (optLevel = 0));
end;

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
  helpLine('OVERVIEW: p3 - Pascal compiler targeting LLVM');
  helpLine('');
  helpLine('USAGE: p3 [options] <input.pas>');
  helpLine('');
  helpLine('OPTIONS:');
  helpLine('');
  helpLine('Generic Options:');
  helpLine('');
  helpLine('  --help                       - Display available options (--help-hidden for more)');
  helpLine('  --help-list                  - Display list of available options (--help-list-hidden for more)');
  helpLine('  --version                    - Display the version of this program');
  helpLine('');
  helpLine('p3 options:');
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
  helpLine('  --Wno-deprecated             - Do not warn about uses of deprecated (experimental, platform, library, unimplemented) declarations');
  helpLine('  --Wno-unused-variable        - Do not warn about unused variables');
  helpLine('  -d <name>                    - Define a symbol for conditional compilation ({$IFDEF})');
  helpLine('  --fdiagnostics-color=<value> - Colorize diagnostics:');
  helpLine('    =auto                      -   If stderr is a tty');
  helpLine('    =always                    -   Always');
  helpLine('    =never                     -   Never');
  helpLine('  --ferror-limit=<N>           - Stop after N errors (0 = no limit)');
  helpLine('  --fkeep-unused               - Keep the routines and data that nothing reachable from the program refers to');
  helpLine('  --ffast-math                 - Set the fast-math flags on floating-point instructions (contraction, reassociation, no NaN/Inf assumptions)');
  helpLine('  --fno-inline-runtime         - Do not inline the runtime''s functions (from -O1 they are, when compiling for the host)');
  helpLine('  --fno-overflow-checks        - Disable runtime integer overflow checks');
  helpLine('  --fno-range-checks           - Disable runtime range checks (subranges, array indices, ...)');
  helpLine('  --fno-zero-init              - Do not zero local arrays, records and strings (the default from -O1)');
  helpLine('  --fseparate-units            - Link separately compiled units (each compiled with -c to <unit>.o next to its source)');
  helpLine('  --fzero-init                 - Local arrays, records and strings start zeroed (the default at -O0)');
  helpLine('  -g                           - Generate debug information');
  helpLine('  -l <name>                    - Link with library <name>');
  helpLine('  --mode=<value>               - Language dialect:');
  helpLine('    =iso                       -   ISO 7185 Pascal');
  helpLine('    =tp                        -   Turbo/Free Pascal extensions');
  helpLine('    =objfpc                    -   Turbo/Free Pascal extensions and classes');
  helpLine('  --no-project                 - Do not read a project file (p3.json)');
  helpLine('  -o <file>                    - Write output to <file>');
  helpLine('  --project=<file>             - Read project settings from <file> (default: the first p3.json in the input file''s directory or its parents)');
  helpLine('  --target=<triple>            - Target triple (default: host)');
end;

const
  numFlags = 22;
  flagNames: array[1..numFlags] of string[20] = (
    'help', 'help-list', 'version', 'emit-tokens', 'emit-ast', 'emit-parse-tree',
    'emit-llvm', 'S', 'c', 'Werror', 'Wno-unused-variable', 'Wno-deprecated',
    'fno-overflow-checks',
    'fno-range-checks', 'fseparate-units', 'g', 'no-project', 'fzero-init',
    'fno-zero-init', 'fno-inline-runtime', 'fkeep-unused', 'ffast-math');
  numValueOpts = 11;
  valueOptNames: array[1..numValueOpts] of string[20] = (
    'o', 'mode', 'fdiagnostics-color', 'ferror-limit', 'target', 'I', 'O', 'L', 'l',
    'd', 'project');

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

{ "Unknown command line argument", with the nearest of p3's options (as
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
  { the project file (p3.json): --project, --no-project; options with a
    value given on the command line, which override its settings }
  projectFile: string;
  noProject: boolean;
  givenOutput, givenMode, givenOptLevel, givenTarget, givenErrorLimit: boolean;

procedure parseArgs; forward;

{ The project file's settings (p3.json, see pcproject.pas), under the
  command line's; its lists after the command line's. }
procedure applyProject;
var
  s: TProjectSettings;
  buildsMain: boolean;
  i, slash: longint;
begin
  if noProject then
    exit;
  if not loadProject(projectFile, inputFile, s) then
    sysExit(1);
  if s.path = '' then
    exit;
  buildsMain := inputFile = '';
  if buildsMain and s.hasMain then
    inputFile := projectPath(s, s.main);
  if buildsMain and s.hasOutput and not givenOutput and (action = actExe) then
  begin
    outputFile := projectPath(s, s.output);
    slash := length(outputFile);
    while (slash > 0) and (outputFile[slash] <> '/') do
      slash := slash - 1;
    if slash > 1 then
      sysMakeDirs(copy(outputFile, 1, slash - 1));
  end;
  if s.hasMode and not givenMode then
    mode := s.mode;
  if s.hasOptLevel and not givenOptLevel then
    optLevel := s.optLevel;
  if s.hasTarget and not givenTarget then
    targetTriple := s.target;
  if s.hasErrorLimit and not givenErrorLimit then
    errorLimit := s.errorLimit;
  debugInfo := debugInfo or s.debug;
  separateUnits := separateUnits or s.separateUnits;
  werror := werror or s.warningsAsErrors;
  noUnusedVariable := noUnusedVariable or not s.warnUnusedVariables;
  noDeprecated := noDeprecated or not s.warnDeprecated;
  rangeChecksOn := rangeChecksOn and s.rangeChecks;
  overflowChecksOn := overflowChecksOn and s.overflowChecks;
  for i := 0 to s.unitPaths.count - 1 do
    listAdd(includeDirs, intern(projectPath(s, nameStr(PName(listGet(s.unitPaths, i))))));
  for i := 0 to s.libraryPaths.count - 1 do
    addLinkerArg('-L' + projectPath(s, nameStr(PName(listGet(s.libraryPaths, i)))));
  for i := 0 to s.defines.count - 1 do
    listAdd(commandLineDefines, listGet(s.defines, i));
  for i := 0 to s.libraries.count - 1 do
    addLinkerArg('-l' + nameStr(PName(listGet(s.libraries, i))));
  { (the language mode and diagnostic settings, again) }
  diagSetErrorLimit(errorLimit);
  diagSetWarningsAsErrors(werror);
  if noUnusedVariable then
    diagDisableWarning('unused-variable');
  if noDeprecated then
    diagDisableWarning('deprecated');
  if buildsMain and not s.hasMain then
  begin
    diagError(noLoc, 'no input file, and the project file ''' + s.path +
                     ''' has no "main" program');
    sysExit(1);
  end;
end;

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
  noDeprecated := false;
  optLevel := 0;
  zeroInitOpt := 0;
  rangeChecksOn := true;
  overflowChecksOn := true;
  fastMath := false;
  debugInfo := false;
  keepUnused := false;
  separateUnits := false;
  projectFile := '';
  noProject := false;
  givenOutput := false;
  givenMode := false;
  givenOptLevel := false;
  givenTarget := false;
  givenErrorLimit := false;
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
    else if name = 'Wno-deprecated' then
      noDeprecated := true
    else if name = 'fno-range-checks' then
      rangeChecksOn := false
    else if name = 'fno-overflow-checks' then
      overflowChecksOn := false
    else if name = 'ffast-math' then
      fastMath := true
    else if name = 'fno-inline-runtime' then
      disableRuntimeInlining
    else if name = 'fkeep-unused' then
      keepUnused := true
    else if name = 'fzero-init' then
      zeroInitOpt := 1
    else if name = 'fno-zero-init' then
      zeroInitOpt := -1
    else if name = 'g' then
      debugInfo := true
    else if name = 'fseparate-units' then
      separateUnits := true
    else if name = 'no-project' then
      noProject := true
    else if name = 'project' then
      projectFile := takeValue
    else if name = 'o' then
    begin
      outputFile := takeValue;
      givenOutput := true;
    end
    else if name = 'mode' then
    begin
      givenMode := true;
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
    begin
      errorLimit := parseUInt(name, takeValue);
      givenErrorLimit := true;
    end
    else if name = 'target' then
    begin
      targetTriple := takeValue;
      givenTarget := true;
    end
    else if name = 'I' then
      listAdd(includeDirs, intern(takeValue))
    else if name = 'd' then
      listAdd(commandLineDefines, intern(takeValue))
    else if name = 'O' then
    begin
      optLevel := parseUInt(name, takeValue);
      givenOptLevel := true;
    end
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
      tkStringLiteral, tkTemplateHead, tkTemplateMiddle, tkTemplateTail:
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
  { uses that failed to load, by the location of the name: not tried again
    when the units are loaded once more, after an early load for
    declared(NAME) }
  failedUses: TIntMap;

var
  selfUnit: PName;      { the unit being compiled, if any: using it is circular }

{ The symbols Free Pascal predefines for the targets p3's library supports
  (Linux, macOS and Windows; x86-64 and AArch64). }
procedure addTargetDefines(const triple: string);
var
  x86, arm: boolean;

  procedure def(const name: string);
  begin
    listAdd(commandLineDefines, intern(name));
  end;

begin
  x86 := (pos('x86_64', triple) = 1) or (pos('amd64', triple) = 1);
  arm := (pos('aarch64', triple) = 1) or (pos('arm64', triple) = 1);
  if pos('linux', triple) > 0 then
  begin
    def('LINUX');
    def('UNIX');
  end
  else if (pos('darwin', triple) > 0) or (pos('macos', triple) > 0) then
  begin
    def('DARWIN');
    def('BSD');
    def('UNIX');
  end
  else if (pos('windows', triple) > 0) or (pos('mingw', triple) > 0) or
          (pos('win32', triple) > 0) then
  begin
    def('WINDOWS');
    def('MSWINDOWS');
    if x86 or arm then
      def('WIN64');
  end;
  if x86 then
  begin
    def('CPUX86_64');
    def('CPUAMD64');
    def('CPU64');
  end
  else if arm then
  begin
    def('CPUAARCH64');
    def('CPU64');
  end;
end;

procedure visitUnit(const u: TIdent);
var
  dirs: TPtrList;
  found, path, base, ext: string;
  i, b, x, fid: longint;
  unitDecl: PDecl;
begin
  if mapHas(unitsDone, u.name^.id) or (mapGet(failedUses, u.loc) <> nil) or
     diagShouldStop then
    exit;
  if (mapGet(inProgress, u.name^.id) <> nil) or (u.name = selfUnit) then
  begin
    diagErrorR(u.loc, 'circular unit reference to ''' + nameStr(u.spelling) + '''',
               identRange(u));
    exit;
  end;
  mapPut(inProgress, u.name^.id, u.name);
  { the input's directory, -I directories, then the unit library }
  listInit(dirs);
  listAdd(dirs, intern(pathDirName(inputFile)));
  for i := 0 to includeDirs.count - 1 do
    listAdd(dirs, listGet(includeDirs, i));
  listAdd(dirs, intern(findUnitLibrary));
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
    mapPut(failedUses, u.loc, u.name);
    { objfpc: Exception and its kin are predeclared, so uses sysutils works
      without the library }
    if (mode = lmObjFPC) and nameEquals(u.name, 'sysutils') then
      exit;
    diagErrorR(u.loc, 'cannot find unit ''' + nameStr(u.spelling) +
               ''' (looked for ' + nameStr(u.name) +
               '.pas next to the program, in -I directories ' +
               'and in p3''s unit library)', identRange(u));
    exit;
  end;
  if not smLoadFile(found, fid) then
  begin
    mapPut(failedUses, u.loc, u.name);
    diagError(u.loc, 'cannot open unit file ''' + found + '''');
    exit;
  end;
  unitDecl := parseUnit(fid, mode);
  if unitDecl = nil then
  begin
    mapPut(inProgress, u.name^.id, nil);
    mapPut(failedUses, u.loc, u.name);
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

{ declared(NAME) after a uses clause: the units' names and those of their
  interfaces (an enumeration's values too). }
procedure declaredUnitNames(const usesList: TPtrList; selfName: PName;
                            var names: TPtrList);
var
  i, j, k: longint;
  u: PIdent;
  unitDecl, d: PDecl;
begin
  if selfName <> nil then
    mapPut(inProgress, selfName^.id, selfName);
  loadUnits(usesList);
  for i := 0 to usesList.count - 1 do
  begin
    u := PIdent(listGet(usesList, i));
    listAdd(names, u^.name);
    unitDecl := PDecl(mapGet(unitsDone, u^.name^.id));
    if unitDecl = nil then
      continue;
    for j := 0 to unitDecl^.iface^.decls.count - 1 do
    begin
      d := PDecl(listGet(unitDecl^.iface^.decls, j));
      listAdd(names, d^.ident.name);
      if (d^.kind = dkType) and (d^.typeSpec <> nil) and (d^.typeSpec^.kind = tsEnum) then
        for k := 0 to d^.typeSpec^.enumNames.count - 1 do
          listAdd(names, PIdent(listGet(d^.typeSpec^.enumNames, k))^.name);
    end;
  end;
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
{ The libraries named by the external routines of block b (external 'z'),
  linked after the -l ones: -lz, or -l:name for a file name
  ('libz.so.1'). }
procedure collectExternalLibraries(b: PBlock; var seen: TPtrList);
var
  lib: PName;
  i, k: longint;
  d: PDecl;
  l: PName;
  s: string;
  dup: boolean;
begin
  if b = nil then
    exit;
  for i := 0 to b^.decls.count - 1 do
  begin
    d := PDecl(listGet(b^.decls, i));
    lib := nil;
    if (d^.kind = dkRoutine) and d^.isExternal then
      lib := d^.libraryName
    else if (d^.kind = dkVar) and d^.varExternal then
      lib := d^.varLibName;
    { StdCLib (classic Mac OS's C library, named by Free Pascal's sources):
      the C library, linked anyway }
    if (lib = nil) or (lib^.len = 0) or (lowerStr(nameStr(lib)) = 'stdclib') then
      continue;
    s := nameStr(lib);
    if (pos('.', s) > 0) or (pos('/', s) > 0) then
      s := ':' + s;
    l := intern('-l' + s);
    dup := false;
    for k := 0 to seen.count - 1 do
      if PName(listGet(seen, k)) = l then
        dup := true;
    if dup then
      continue;
    listAdd(seen, l);
    addLinkerArg(nameStr(l));
  end;
end;

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
{ Compiles the unit u parsed from file fid. }
function compileUnit(fid: longint; u: PDecl): boolean;
var
  uses_: TPtrList;
  i: longint;
  layout: string;
  kind: TOutputKind;
  noObjects: TPtrList;
begin
  compileUnit := false;
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
                     targetTriple, layout, debugInfo, optLevel > 0, zeroInit, keepUnused,
                     outBuf);
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
  unitObjects, extLibs: TPtrList;
  i: longint;
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
  if noDeprecated then
    diagDisableWarning('deprecated');
  applyProject;
  cgSetFastMath(fastMath);
  if inputFile = '' then
    usageError('no input file');
  if optLevel > 3 then
    usageError('invalid optimization level ''-O' + intToStr(optLevel) +
               ''' (expected 0-3)');
  if not validateTarget(targetTriple) then
    sysExit(1);
  addTargetDefines(targetTriple);
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
  mapInit(failedUses);
  selfUnit := nil;
  initTypes;
  isUnit := false;
  if action <> actTokens then
  begin
    lexSetPredeclared(standardNames(mode));
    usedUnitNames := @declaredUnitNames;
    { a program or (Turbo/FPC) a unit, parsed once (a unit's body only
      when it is to be compiled) }
    prog := parseInput(fid, mode, (action <> actParseTree) and (action <> actAST),
                       isUnit);
    if isUnit and ((action = actParseTree) or (action = actAST)) then
    begin
      diagError(noLoc, '''' + inputFile + ''' is a unit; --emit-parse-tree and ' +
                       '--emit-ast need a program');
      sysExit(1);
    end;
    if isUnit then
    begin
      if compileUnit(fid, prog) and not diagHasErrors then
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
        listInit(extLibs);
        for i := 0 to units.count - 1 do
        begin
          collectExternalLibraries(PDecl(listGet(units, i))^.iface, extLibs);
          collectExternalLibraries(PDecl(listGet(units, i))^.impl, extLibs);
        end;
        collectExternalLibraries(prog^.progBlock, extLibs);
        generateModule(prog, units, mode, rangeChecksOn, overflowChecksOn,
                       smFileName(fid), targetTriple, modLayout, debugInfo, optLevel > 0,
                       separateUnits, zeroInit, keepUnused, outBuf);
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
