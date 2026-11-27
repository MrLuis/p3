{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
program p3;
{ The self-hosted Pascal compiler: command line and pipeline. It accepts
  the C++ p3's options (src/driver/main.cpp) and must behave the same.
  The dumps (--emit-tokens, --emit-parse-tree, --emit-ast) are written
  here; the IR from pcgen goes to pcdriver, which runs clang and the
  linker for -S, -c, -O and executables. }

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcparse, pcprint, pcsema, pcgen, pcproject,
     pcdriver, pcsrcgen, pcctime;

type
  TAction = (actTokens, actParseTree, actAST, actModel, actLLVM, actAsm, actObj, actExe);

var
  inputFile, outputFile: string;
  action: TAction;
  mode: TLangMode;
  safeLevel: TSafeLevel;
  errorLimit: longint;
  werror, noUnusedVariable, noDeprecated: boolean;
  { --Wunsafe / --Wno-unsafe (0: default, on for --safe=checked) }
  warnUnsafeOpt: longint;
  rangeChecksOn, overflowChecksOn, debugInfo, separateUnits: boolean;
  { -ffast-math: set the fast-math flags on floating-point instructions }
  fastMath: boolean;
  { -mcpu / -mattr: the target CPU and features; "native" = the host CPU }
  targetCPU: string;
  targetFeatures: PName;
  givenCPU: boolean;
  { -Os (1) / -Oz (2): optimize for size }
  sizeLevel: longint;
  { -fkeep-unused: keep the routines and data nothing reachable refers to }
  keepUnused: boolean;
  optLevel: longint;
  { -fzero-init (1) / -fno-zero-init (-1), the last one given; 0: neither
    (local arrays, records and strings start zeroed at -O0 only) }
  zeroInitOpt: longint;
  outBuf: TBuf;
  { --generator, --generated-dir, --generator-timeout, --emit-model }
  generators: TPtrList; { of PName }
  generatedDir, emitModelAttr: string;
  generatorTimeout: longint;
  givenGeneratorTimeout: boolean;

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
  helpLine('  --Wno-unsafe                 - Do not warn about unsafe constructs outside ''unsafe'' regions');
  helpLine('  --Wno-unused-variable        - Do not warn about unused variables');
  helpLine('  --Wunsafe                    - Warn about unsafe constructs outside ''unsafe'' regions');
  helpLine('  -d <name>                    - Define a symbol for conditional compilation ({$IFDEF})');
  helpLine('  --emit-model=<attr>          - Print the generator model of the types marked [attr] (for testing)');
  helpLine('  --fdiagnostics-color=<value> - Colorize diagnostics:');
  helpLine('    =auto                      -   If stderr is a tty');
  helpLine('    =always                    -   Always');
  helpLine('    =never                     -   Never');
  helpLine('  --ferror-limit=<N>           - Stop after N errors (0 = no limit)');
  helpLine('  --fkeep-unused               - Keep the routines and data that nothing reachable from the program refers to');
  helpLine('  --ffast-math                 - Set the fast-math flags on floating-point instructions (contraction, reassociation, no NaN/Inf assumptions)');
  helpLine('  -mcpu <cpu>                  - Target CPU (default: generic; ''native'': the host CPU)');
  helpLine('  -Os                          - Optimize for size (like -O2, SLP vectorization on)');
  helpLine('  -Oz                          - Optimize aggressively for size (like -Os)');
  helpLine('  -mattr <features>            - Target features, e.g. +avx2,-sse4.2');
  helpLine('  --fno-inline-runtime         - Do not inline the runtime''s functions (from -O1 they are, when compiling for the host)');
  helpLine('  --fno-internalize-runtime    - Keep the inlined runtime''s functions in the library (from -O1 the program gets its own copies, which the optimizer may specialize)');
  helpLine('  --fno-overflow-checks        - Disable runtime integer overflow checks');
  helpLine('  --fno-range-checks           - Disable runtime range checks (subranges, array indices, ...)');
  helpLine('  --fno-zero-init              - Do not zero local arrays, records and strings (the default from -O1)');
  helpLine('  --fseparate-units            - Link separately compiled units (each compiled with -c to <unit>.o next to its source)');
  helpLine('  --fwhole-program             - With -fseparate-units, from -O1: optimize the units'' code as part of the program instead of linking their objects');
  helpLine('  --fzero-init                 - Local arrays, records and strings start zeroed (the default at -O0)');
  helpLine('  -g                           - Generate debug information');
  helpLine('  --generated-dir=<dir>        - Where the generators'' code is written (default: gen next to the input)');
  helpLine('  --generator=<file>           - Build and run a source generator (a program that starts `generator Name for Attr;`) first');
  helpLine('  --generator-timeout=<seconds> - Seconds a generator may run (default 30)');
  helpLine('  -l <name>                    - Link with library <name>');
  helpLine('  --mode=<value>               - Language dialect:');
  helpLine('    =iso                       -   ISO 7185 Pascal');
  helpLine('    =tp                        -   Turbo/Free Pascal extensions');
  helpLine('    =objfpc                    -   Turbo/Free Pascal extensions and classes');
  helpLine('  --no-project                 - Do not read a project file (p3.json)');
  helpLine('  -o <file>                    - Write output to <file>');
  helpLine('  --project=<file>             - Read project settings from <file> (default: the first p3.json in the input file''s directory or its parents)');
  helpLine('  --safe=<value>               - Memory safety level:');
  helpLine('    =standard                  -   Standard language rules (default)');
  helpLine('    =checked                   -   Runtime safety checks');
  helpLine('    =verified                  -   Checked plus compile-time pointer checks (forces {$T+})');
  helpLine('  --target=<triple>            - Target triple (default: host)');
end;

const
  numFlags = 28;
  flagNames: array[1..numFlags] of string = (
    'help', 'help-list', 'version', 'emit-tokens', 'emit-ast', 'emit-parse-tree',
    'emit-llvm', 'S', 'c', 'Werror', 'Wno-unused-variable', 'Wno-deprecated',
    'Wunsafe', 'Wno-unsafe', 'fno-overflow-checks',
    'fno-range-checks', 'fseparate-units', 'g', 'no-project', 'fzero-init',
    'fno-zero-init', 'fno-inline-runtime', 'fkeep-unused', 'ffast-math', 'Os', 'Oz',
    'fno-internalize-runtime', 'fwhole-program');
  numValueOpts = 18;
  valueOptNames: array[1..numValueOpts] of string = (
    'o', 'mode', 'safe', 'fdiagnostics-color', 'ferror-limit', 'target', 'I', 'O',
    'L', 'l', 'd', 'project', 'mcpu', 'mattr', 'generator', 'generated-dir',
    'generator-timeout', 'emit-model');

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
  givenOutput, givenMode, givenSafe, givenOptLevel, givenTarget, givenErrorLimit: boolean;

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
  if s.hasSafe and not givenSafe then
    safeLevel := s.safe;
  if s.hasOptLevel and not givenOptLevel then
    optLevel := s.optLevel;
  if s.hasTarget and not givenTarget then
    targetTriple := s.target;
  if s.hasCPU and not givenCPU then
    targetCPU := s.cpu;
  if s.hasErrorLimit and not givenErrorLimit then
    errorLimit := s.errorLimit;
  debugInfo := debugInfo or s.debug;
  separateUnits := separateUnits or s.separateUnits;
  if s.wholeProgram then
    enableWholeProgram;
  werror := werror or s.warningsAsErrors;
  noUnusedVariable := noUnusedVariable or not s.warnUnusedVariables;
  noDeprecated := noDeprecated or not s.warnDeprecated;
  if (warnUnsafeOpt = 0) and s.hasWarnUnsafe then
  begin
    if s.warnUnsafe then
      warnUnsafeOpt := 1
    else
      warnUnsafeOpt := -1;
  end;
  rangeChecksOn := rangeChecksOn and s.rangeChecks;
  overflowChecksOn := overflowChecksOn and s.overflowChecks;
  for i := 0 to s.unitPaths.count - 1 do
    listAdd(includeDirs, intern(projectPath(s, nameStr(PName(listGet(s.unitPaths, i))))));
  for i := 0 to s.libraryPaths.count - 1 do
    addLinkerArg('-L' + projectPath(s, nameStr(PName(listGet(s.libraryPaths, i)))));
  for i := 0 to s.generators.count - 1 do
    if isLibraryGenerator(nameStr(PName(listGet(s.generators, i)))) then
      listAdd(generators, listGet(s.generators, i)) { (a library one: its name) }
    else
      listAdd(generators, intern(projectPath(s, nameStr(PName(listGet(s.generators, i))))));
  if s.hasGeneratedDir and (generatedDir = '') then
    generatedDir := projectPath(s, s.generatedDir);
  if s.hasGeneratorTimeout and not givenGeneratorTimeout then
    generatorTimeout := s.generatorTimeout;
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
  generatedDir := '';
  emitModelAttr := '';
  generatorTimeout := 30;
  givenGeneratorTimeout := false;
  targetTriple := '';
  action := actExe;
  mode := lmISO;
  safeLevel := slStandard;
  errorLimit := 20;
  werror := false;
  noUnusedVariable := false;
  noDeprecated := false;
  optLevel := 0;
  zeroInitOpt := 0;
  rangeChecksOn := true;
  overflowChecksOn := true;
  fastMath := false;
  targetCPU := 'generic';
  targetFeatures := intern('');
  givenCPU := false;
  sizeLevel := 0;
  debugInfo := false;
  keepUnused := false;
  separateUnits := false;
  projectFile := '';
  noProject := false;
  givenOutput := false;
  givenMode := false;
  givenSafe := false;
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
    else if name = 'Wunsafe' then
      warnUnsafeOpt := 1
    else if name = 'Wno-unsafe' then
      warnUnsafeOpt := -1
    else if name = 'fno-range-checks' then
      rangeChecksOn := false
    else if name = 'fno-overflow-checks' then
      overflowChecksOn := false
    else if name = 'ffast-math' then
      fastMath := true
    else if name = 'fno-inline-runtime' then
      disableRuntimeInlining
    else if name = 'fno-internalize-runtime' then
      disableRuntimeInternalizing
    else if name = 'fwhole-program' then
      enableWholeProgram
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
    else if name = 'safe' then
    begin
      givenSafe := true;
      value := takeValue;
      if value = 'standard' then
        safeLevel := slStandard
      else if value = 'checked' then
        safeLevel := slChecked
      else if value = 'verified' then
        safeLevel := slVerified
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
    else if name = 'mcpu' then
    begin
      targetCPU := takeValue;
      givenCPU := true;
    end
    else if name = 'mattr' then
      targetFeatures := intern(takeValue)
    else if name = 'generator' then
      listAdd(generators, intern(takeValue))
    else if name = 'generated-dir' then
      generatedDir := takeValue
    else if name = 'generator-timeout' then
    begin
      generatorTimeout := parseUInt(name, takeValue);
      givenGeneratorTimeout := true;
    end
    else if name = 'emit-model' then
    begin
      action := actModel;
      emitModelAttr := lowerStr(takeValue);
    end
    else if name = 'I' then
      listAdd(includeDirs, intern(takeValue))
    else if name = 'd' then
      listAdd(commandLineDefines, intern(takeValue))
    else if name = 'O' then
    begin
      optLevel := parseUInt(name, takeValue);
      sizeLevel := 0;
      givenOptLevel := true;
    end
    else if name = 'Os' then
    begin
      optLevel := 2;
      sizeLevel := 1;
      givenOptLevel := true;
    end
    else if name = 'Oz' then
    begin
      optLevel := 2;
      sizeLevel := 2;
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

{ optimization3 item 2: on AArch64 Linux, atomics use the library's
  __aarch64_* helpers, which run the LSE instructions where the CPU has
  them and an LL/SC loop elsewhere (clang's default there). Without the
  feature the generic CPU always gets the loop, in the program's code and in
  the inlined runtime (reference counts). A -mattr that says anything about
  LSE is left alone. Darwin is not changed: not measured. }
{ Does n hold s (lower case), letters in either case? }
function nameHasText(n: PName; const s: string): boolean;
var
  i: int64;
  k: longint;
begin
  nameHasText := false;
  for i := 0 to n^.len - length(s) do
  begin
    k := 0;
    {$R-}
    while (k < length(s)) and (lowerChar(n^.chars^[i + k]) = s[k + 1]) do
      k := k + 1;
    {$R+}
    if k = length(s) then
      exit(true);
  end;
end;

procedure addDefaultTargetFeatures(const triple: string; var features: PName);
var
  b: TBuf;
begin
  if (pos('aarch64', triple) <> 1) or (pos('linux', triple) = 0) then
    exit;
  if nameHasText(features, 'lse') or nameHasText(features, 'outline-atomics') then
    exit;
  bufInit(b);
  bufAddName(b, features);
  if features^.len > 0 then
    bufAddChar(b, ',');
  bufAdd(b, '+outline-atomics');
  features := internChars(b.data, 0, b.len);
  bufFree(b);
end;

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

function replaceExt(const path, ext: string): string;
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
  replaceExt := r + ext;
end;

{ FNV-1a of some bytes, as the interface files' stamp (item 22). }
function ifHash(p: PCharArray; len: int64): int64;
var
  i: int64;
  h: int64;
begin
  h := -3750763034362895579; { 0xcbf29ce484222325 }
  {$Q-}{$R-}
  for i := 0 to len - 1 do
  begin
    h := h xor int64(ord(p^[i]));
    h := h * 1099511628211;
  end;
  {$Q+}{$R+}
  ifHash := h;
end;

function ifHashHex(h: int64): string;
const
  digits = '0123456789abcdef';
var
  i: longint;
  s: string;
begin
  s := '';
  for i := 15 downto 0 do
    s := s + digits[((h shr (i * 4)) and 15) + 1];
  ifHashHex := s;
end;

function ifFindUnitSource(const name: string; const dirs: TPtrList;
                          var found: string): boolean;
var
  i, b, x: longint;
  base, ext, path: string;
begin
  found := '';
  for i := 0 to dirs.count - 1 do
    for b := 1 to 2 do
      for x := 1 to 2 do
      begin
        if b = 1 then
          base := name
        else
          base := name;
        if x = 1 then
          ext := '.pas'
        else
          ext := '.pp';
        path := pathJoin(nameStr(PName(listGet(dirs, i))), base + ext);
        if (found = '') and sysFileExists(path) then
          found := path;
      end;
  ifFindUnitSource := found <> '';
end;

function ifLower(c: char): char;
begin
  if (c >= 'A') and (c <= 'Z') then
    ifLower := chr(ord(c) + 32)
  else
    ifLower := c;
end;

function ifIdChar(c: char): boolean;
begin
  ifIdChar := ((c >= 'a') and (c <= 'z')) or ((c >= 'A') and (c <= 'Z')) or
              ((c >= '0') and (c <= '9')) or (c = '_');
end;

{ Does the source contain the word generic? (Conservative: such a unit's
  implementation has bodies a precompiled interface would lose.) }
{ Does u declare any generic types, routines, or methods whose
  implementation tokens a precompiled .p3i interface would lose? }
function ifDeclsHaveGenerics(const decls: TPtrList): boolean;
var
  i: longint;
  d: PDecl;
  ts: PTypeSpec;
begin
  ifDeclsHaveGenerics := true;
  for i := 0 to decls.count - 1 do
  begin
    d := PDecl(listGet(decls, i));
    if d^.kind = dkType then
    begin
      if d^.tgGeneric then
        exit;
      ts := d^.typeSpec;
      if ts <> nil then
      begin
        if (ts^.kind = tsRecord) and ifDeclsHaveGenerics(ts^.recMembers) then
          exit;
        if (ts^.kind = tsClass) and ifDeclsHaveGenerics(ts^.memberList) then
          exit;
        if (ts^.kind = tsHelper) and ifDeclsHaveGenerics(ts^.helperMembers) then
          exit;
      end;
    end
    else if (d^.kind = dkRoutine) and d^.isGeneric then
      exit;
  end;
  ifDeclsHaveGenerics := false;
end;

function ifUnitHasGenerics(u: PDecl): boolean;
begin
  ifUnitHasGenerics :=
    ((u^.iface <> nil) and ifDeclsHaveGenerics(u^.iface^.decls)) or
    ((u^.impl <> nil) and ifDeclsHaveGenerics(u^.impl^.decls));
end;

{ The offset of the implementation keyword, skipping comments and strings,
  or len. }
function ifImplementationOffset(p: PCharArray; len: int64): int64;
var
  i, j: int64;
  c: char;
  word: boolean;
begin
  i := 0;
  while i < len do
  begin
    c := p^[i];
    if c = '''' then
    begin
      i := i + 1;
      while i < len do
      begin
        if p^[i] = '''' then
        begin
          if (i + 1 < len) and (p^[i + 1] = '''') then
            i := i + 2
          else
          begin
            i := i + 1;
            break;
          end;
        end
        else
          i := i + 1;
      end;
      continue;
    end;
    if c = '{' then
    begin
      i := i + 1;
      while (i < len) and (p^[i] <> '}') do
        i := i + 1;
      i := i + 1;
      continue;
    end;
    if (c = '(') and (i + 1 < len) and (p^[i + 1] = '*') then
    begin
      i := i + 2;
      while (i + 1 < len) and not ((p^[i] = '*') and (p^[i + 1] = ')')) do
        i := i + 1;
      i := i + 2;
      continue;
    end;
    if (c = '/') and (i + 1 < len) and (p^[i + 1] = '/') then
    begin
      while (i < len) and (p^[i] <> #10) do
        i := i + 1;
      continue;
    end;
    if ifIdChar(c) and not ((c >= '0') and (c <= '9')) then
    begin
      j := i;
      while (j < len) and ifIdChar(p^[j]) do
        j := j + 1;
      word := j - i = 14;
      if word then
      begin
        word := (ifLower(p^[i]) = 'i') and (ifLower(p^[i + 1]) = 'm') and
                (ifLower(p^[i + 2]) = 'p') and (ifLower(p^[i + 3]) = 'l') and
                (ifLower(p^[i + 4]) = 'e') and (ifLower(p^[i + 5]) = 'm') and
                (ifLower(p^[i + 6]) = 'e') and (ifLower(p^[i + 7]) = 'n') and
                (ifLower(p^[i + 8]) = 't') and (ifLower(p^[i + 9]) = 'a') and
                (ifLower(p^[i + 10]) = 't') and (ifLower(p^[i + 11]) = 'i') and
                (ifLower(p^[i + 12]) = 'o') and (ifLower(p^[i + 13]) = 'n');
      end;
      if word then
        exit(i);
      i := j;
      continue;
    end;
    i := i + 1;
  end;
  ifImplementationOffset := len;
end;

{ Checks the .p3i's source and dependency hashes (item 22). }
function ifInterfaceValid(const p3iPath, sourcePath: string;
                          const dirs: TPtrList): boolean;
var
  data: PCharArray;
  len, p, start, i: int64;
  text: TBuf;
  line, rest, name, hash, dep: string;
  depData: PCharArray;
  depLen: int64;
  sawSource, ok, found: boolean;
begin
  ifInterfaceValid := false;
  if not sysReadFile(p3iPath, data, len) then
    exit;
  if not sysReadFile(sourcePath, depData, depLen) then
    exit;
  hash := ifHashHex(ifHash(depData, depLen));
  sawSource := false;
  ok := true;
  found := false;
  bufInit(text);
  bufAddChars(text, data, 0, len);
  p := 0;
  while (p < text.len) and ok do
  begin
    start := p;
    while (p < text.len) and (text.data^[p] <> #10) do
      p := p + 1;
    line := '';
    for i := start to p - 1 do
      line := line + text.data^[i];
    if p < text.len then
      p := p + 1;
    if not found then
    begin
      if line = '// p3 interface v1' then
        found := true;
      continue;
    end;
    if copy(line, 1, 11) = '// source ' then
    begin
      if copy(line, 12, length(line)) <> hash then
        ok := false
      else
        sawSource := true;
      continue;
    end;
    if copy(line, 1, 7) = '// use ' then
    begin
      rest := copy(line, 8, length(line));
      i := pos(' ', rest);
      if i = 0 then
        ok := false
      else
      begin
        name := copy(rest, 1, i - 1);
        hash := copy(rest, i + 1, length(rest));
        if not ifFindUnitSource(name, dirs, dep) then
          ok := false
        else if not sysReadFile(dep, depData, depLen) then
          ok := false
        else if ifHashHex(ifHash(depData, depLen)) <> hash then
          ok := false;
      end;
      continue;
    end;
    if copy(line, 1, 11) = '// generics' then
    begin
      ok := false;
      continue;
    end;
    if copy(line, 1, 2) <> '//' then
      break;
  end;
  bufFree(text);
  ifInterfaceValid := found and ok and sawSource;
end;

{ Writes the unit u's precompiled interface to path (item 22). }
procedure ifWriteInterface(u: PDecl; const deps: TPtrList;
                           srcData: PCharArray; srcLen: int64;
                           const dirs: TPtrList; const path: string);
var
  off, i, j: int64;
  text: TBuf;
  useName: PName;
  dep: PDecl;
  depPath: string;
  depData: PCharArray;
  depLen: int64;
  depName: PName;
begin
  off := ifImplementationOffset(srcData, srcLen);
  if off >= srcLen then
    exit;
  bufInit(text);
  bufAddChars(text, srcData, 0, off);
  bufAdd(text, #10 + '// p3 interface v1' + #10);
  if ifUnitHasGenerics(u) then
    bufAdd(text, '// generics' + #10);
  bufAdd(text, '// source ' + ifHashHex(ifHash(srcData, srcLen)) + #10);
  for i := 0 to u^.ifaceUses.count - 1 do
  begin
    useName := PIdent(listGet(u^.ifaceUses, i))^.name;
    dep := nil;
    for j := 0 to deps.count - 1 do
    begin
      depName := PDecl(listGet(deps, j))^.ident.name;
      if depName = useName then
      begin
        dep := PDecl(listGet(deps, j));
        break;
      end;
    end;
    if (dep <> nil) and ifFindUnitSource(nameStr(useName), dirs, depPath) and
       sysReadFile(depPath, depData, depLen) then
      bufAdd(text, '// use ' + nameStr(useName) + ' ' +
                    ifHashHex(ifHash(depData, depLen)) + #10);
  end;
  bufAdd(text, 'implementation' + #10 + 'end.' + #10);
  if not sysWriteFile(path, text.data, text.len) then
    diagWarning(noLoc, 'cannot write interface file ''' + path + '''');
  bufFree(text);
end;

{ Loads a source file: with the code the source generators wrote for it
  included, if they wrote any. }
function loadSource(const path: string; var fid: longint): boolean;
var
  data: PCharArray;
  len: int64;
begin
  if spliceFor(path, data, len) then
  begin
    fid := smAddBuffer(path, data, len);
    loadSource := true;
  end
  else
    loadSource := smLoadFile(path, fid);
end;

procedure visitUnit(const u: TIdent; interfacesOnly: boolean);
var
  dirs: TPtrList;
  found, path, base, ext, e: string;
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
  if interfacesOnly then
  begin
    e := replaceExt(found, '.p3i');
    if sysFileExists(e) and ifInterfaceValid(e, found, dirs) then
      found := e;
  end;
  if not loadSource(found, fid) then
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
    visitUnit(PIdent(listGet(unitDecl^.ifaceUses, i))^, interfacesOnly);
  if not interfacesOnly then
    for i := 0 to unitDecl^.implUses.count - 1 do
      visitUnit(PIdent(listGet(unitDecl^.implUses, i))^, interfacesOnly);
  mapPut(inProgress, u.name^.id, nil);
  mapPut(unitsDone, u.name^.id, unitDecl);
  listAdd(units, unitDecl);
end;

{ Finds, parses and orders (dependencies first) the units in usesList. }
procedure loadUnits(const usesList: TPtrList; interfacesOnly: boolean);
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
    visitUnit(PIdent(listGet(usesList, i))^, interfacesOnly);
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
  loadUnits(usesList, separateUnits);
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

{ path with its extension replaced by .ll (the IR next to a unit's object) }
function unitIRPath(const path: string): string;
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
  unitIRPath := r + '.ll';
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

function findUnitObjects(var objects: TPtrList; required: boolean): boolean;
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
    { the library's units are compiled for the host only }
    if not isHostTriple(targetTriple) and (pathDirName(obj) = findUnitLibrary) then
    begin
      if required then
      begin
        diagError(noLoc, 'unit ''' + nameStr(u^.ident.spelling) +
                         ''' of p3''s library is compiled for the host only; build for ''' +
                         targetTriple + ''' without -fseparate-units');
        exit;
      end;
      continue;
    end;
    if not sysFileExists(obj) then
    begin
      if required then
      begin
        diagError(noLoc, 'cannot find ''' + obj + ''', the object file of unit ''' +
                         nameStr(u^.ident.spelling) + '''; compile the unit with -c');
        exit;
      end;
      continue;
    end;
    listAdd(objects, intern(obj));
  end;
  findUnitObjects := true;
end;

{ A unit file as the input: its own module (S11). }
{ Compiles the unit u parsed from file fid. }
{ Forgets the units loaded (to load them again). }
procedure resetUnits;
begin
  listInit(units);
  mapInit(unitsDone);
  mapInit(inProgress);
  mapInit(failedUses);
end;

{ Compiletime constants (pcctime): computed after parsing; the sources get
  their values, and root (with fid) is parsed again from the new text.
  False after errors. }
function computeCompiletime(var root: PDecl; var fid: longint;
                            parseBody: boolean): boolean;
var
  restart, isU: boolean;
begin
  computeCompiletime := false;
  if not evaluateCompiletime(root, units, mode, inputFile, generatedDir,
                             includeDirs, commandLineDefines, generators,
                             generatorTimeout, restart) then
    exit;
  computeCompiletime := true;
  if not restart then
    exit;
  resetUnits;
  diagSetSkipRepeatedWarnings(true);
  if not loadSource(inputFile, fid) then
  begin
    diagError(noLoc, 'cannot open input file ''' + inputFile + '''');
    exit(false);
  end;
  root := parseInput(fid, mode, parseBody, isU);
  computeCompiletime := root <> nil;
end;

function compileUnit(fid: longint; u: PDecl): boolean;
var
  uses_: TPtrList;
  i: longint;
  layout: string;
  kind: TOutputKind;
  noObjects, dirs: TPtrList;
  p3i, srcP3i: string;
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
  loadUnits(uses_, true);
  if not diagHasErrors then
  begin
    if not computeCompiletime(u, fid, true) then
      exit;
    listInit(uses_);
    for i := 0 to u^.ifaceUses.count - 1 do
      listAdd(uses_, listGet(u^.ifaceUses, i));
    for i := 0 to u^.implUses.count - 1 do
      listAdd(uses_, listGet(u^.implUses, i));
    if units.count = 0 then
      loadUnits(uses_, true);
  end;
  if diagHasErrors or not semaCheckUnit(u, units, mode, safeLevel) then
    exit;
  if (action = actObj) and (outputFile <> '-') then
  begin
    listInit(dirs);
    listAdd(dirs, intern(pathDirName(inputFile)));
    for i := 0 to includeDirs.count - 1 do
      listAdd(dirs, listGet(includeDirs, i));
    listAdd(dirs, intern(findUnitLibrary));
    p3i := replaceExt(outputFile, '.p3i');
    ifWriteInterface(u, units, smFileData(fid), smFileLen(fid), dirs, p3i);
    srcP3i := replaceExt(smFileName(fid), '.p3i');
    if srcP3i <> p3i then
      ifWriteInterface(u, units, smFileData(fid), smFileLen(fid), dirs, srcP3i);
    listFree(dirs);
  end;
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
  { an IR file present is always the object's (Driver.cpp's compileUnit):
    at -O0 an earlier one is removed }
  if (kind = okObj) and (optLevel > 0) and (outputFile <> '-') then
  begin
    if not writeModuleText(outBuf, unitIRPath(outputFile)) then
      exit;
  end
  else if (kind = okObj) and (outputFile <> '-') then
    sysRemoveFile(unitIRPath(outputFile));
  compileUnit := emitOutput(outBuf, smFileName(fid), kind, optLevel, sizeLevel,
                            targetTriple, targetCPU, targetFeatures, outputFile,
                            noObjects);
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
  if not ((warnUnsafeOpt = 1) or
          ((warnUnsafeOpt = 0) and (safeLevel = slChecked))) then
    diagDisableWarning('unsafe');
  if targetCPU = 'native' then
  begin
    targetCPU := hostCPUName;
    if targetFeatures^.len = 0 then
      targetFeatures := hostCPUFeatures;
  end;
  cgSetSizeLevel(sizeLevel);
  cgSetFastMath(fastMath);
  if inputFile = '' then
    usageError('no input file');
  if optLevel > 3 then
    usageError('invalid optimization level ''-O' + intToStr(optLevel) +
               ''' (expected 0-3)');
  if not validateTarget(targetTriple) then
    sysExit(1);
  addDefaultTargetFeatures(targetTriple, targetFeatures);
  cgSetTarget(targetCPU, targetFeatures);
  addTargetDefines(targetTriple);
  if outputFile = '' then
    case action of
      actLLVM: outputFile := inputStem + '.ll';
      actAsm: outputFile := inputStem + '.s';
      actObj: outputFile := inputStem + '.o';
      actExe:
        if isWindowsTriple(targetTriple) then
          outputFile := inputStem + '.exe'
        else if inputFile = '-' then
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
    if action = actModel then
    begin
      if not modelOfFile(inputFile, mode, emitModelAttr, outBuf) then
      begin
        diagError(noLoc, 'cannot read ''' + inputFile +
                  ''': it has syntax errors');
        sysExit(1);
      end;
      if writeOutput then
        sysExit(0);
      sysExit(1);
    end;
    if generators.count > 0 then
    begin
      if not runGenerators(inputFile, generators, generatedDir, includeDirs,
                           mode, generatorTimeout) then
        sysExit(1);
      if not loadSource(inputFile, fid) then
        usageError('cannot open input file ''' + inputFile + ''': ' +
                   sysOpenError(inputFile));
    end;
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
          loadUnits(prog^.progUses, separateUnits);
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
        loadUnits(prog^.progUses, separateUnits);
      { (not for a dump: --emit-ast leaves them as calls) }
      semaCompiletimeDeferred := action = actAST;
      if not diagHasErrors and (action <> actAST) then
      begin
        if not computeCompiletime(prog, fid, action <> actParseTree) then
          sysExit(1);
        if (units.count = 0) and not diagHasErrors then
          loadUnits(prog^.progUses, separateUnits);
      end;
      semaOK := false;
      if not diagHasErrors then
        semaOK := semaCheck(prog, units, mode, safeLevel, separateUnits);
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
        if separateUnits and not findUnitObjects(unitObjects, action = actExe) then
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
        ok := emitOutput(outBuf, smFileName(fid), kind, optLevel, sizeLevel,
                         targetTriple, targetCPU, targetFeatures, outputFile,
                         unitObjects);
      end;
    end;
  end;
  if ok and not diagHasErrors then
    sysExit(0);
  sysExit(1);
end.
