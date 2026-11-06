program pcc;
{ The self-hosted Pascal compiler: command line and pipeline. It accepts
  the C++ pcc's options (src/driver/main.cpp) and must behave the same.
  Implemented so far: --emit-tokens (S4), --emit-parse-tree (S5),
  --emit-ast and semantic checks (S6). }

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcparse, pcprint, pcsema;

type
  TAction = (actTokens, actParseTree, actAST, actLLVM, actAsm, actObj, actExe);

var
  inputFile, outputFile: string;
  action: TAction;
  mode: TLangMode;
  errorLimit: longint;
  werror, noUnusedVariable: boolean;
  optLevel: longint;
  outBuf: TBuf;

procedure usageError(const msg: string);
begin
  diagError(noLoc, msg);
  sysExit(1);
end;

function startsWith(const s, prefix: string): boolean;
begin
  startsWith := copy(s, 1, length(prefix)) = prefix;
end;

{ A non-negative decimal number, or -1. }
function parseCount(const s: string): longint;
var
  i: longint;
  v: int64;
begin
  if (s = '') or (length(s) > 9) then
    exit(-1);
  v := 0;
  for i := 1 to length(s) do
  begin
    if (s[i] < '0') or (s[i] > '9') then
      exit(-1);
    v := v * 10 + ord(s[i]) - ord('0');
  end;
  parseCount := longint(v);
end;

procedure parseArgs;
var
  i, n: longint;
  arg, opt, value: string;
  haveValue: boolean;

  { The value of "-opt=value" or of the next argument. }
  function takeValue: string;
  begin
    if haveValue then
      exit(value);
    if i >= paramcount then
      usageError('option ''-' + opt + ''' needs a value');
    i := i + 1;
    takeValue := paramstr(i);
  end;

begin
  inputFile := '';
  outputFile := '';
  action := actExe;
  mode := lmISO;
  errorLimit := 20;
  werror := false;
  noUnusedVariable := false;
  optLevel := 0;
  i := 1;
  while i <= paramcount do
  begin
    arg := paramstr(i);
    if (length(arg) < 2) or (arg[1] <> '-') then
    begin
      { '-' alone is standard input }
      if inputFile <> '' then
        usageError('more than one input file');
      inputFile := arg;
      i := i + 1;
      continue;
    end;
    { -opt / --opt, optionally =value }
    opt := copy(arg, 2, 255);
    if (opt <> '') and (opt[1] = '-') then
      opt := copy(opt, 2, 255);
    haveValue := pos('=', opt) > 0;
    value := '';
    if haveValue then
    begin
      value := copy(opt, pos('=', opt) + 1, 255);
      opt := copy(opt, 1, pos('=', opt) - 1);
    end;
    if opt = 'emit-tokens' then
      action := actTokens
    else if opt = 'emit-parse-tree' then
      action := actParseTree
    else if opt = 'emit-ast' then
      action := actAST
    else if opt = 'emit-llvm' then
      action := actLLVM
    else if opt = 'S' then
      action := actAsm
    else if opt = 'c' then
      action := actObj
    else if opt = 'o' then
      outputFile := takeValue
    else if opt = 'mode' then
    begin
      value := takeValue;
      if value = 'tp' then
        mode := lmTP
      else if value = 'iso' then
        mode := lmISO
      else
        usageError('invalid --mode ''' + value + ''' (expected iso or tp)');
    end
    else if opt = 'ferror-limit' then
    begin
      n := parseCount(takeValue);
      if n < 0 then
        usageError('invalid -ferror-limit value');
      errorLimit := n;
    end
    else if opt = 'fdiagnostics-color' then
      value := takeValue { colors are not supported yet }
    else if opt = 'Werror' then
      werror := true
    else if opt = 'Wno-unused-variable' then
      noUnusedVariable := true
    else if (opt = 'g') or (opt = 'fno-range-checks') or
            (opt = 'fno-overflow-checks') then
      { code generation options: accepted for later milestones }
    else if opt = 'target' then
      value := takeValue
    else if startsWith(opt, 'I') then
    begin
      if length(opt) > 1 then
        value := copy(opt, 2, 255)
      else
        value := takeValue;
      listAdd(includeDirs, intern(value));
    end
    else if startsWith(opt, 'O') and (length(opt) = 2) then
      optLevel := ord(opt[2]) - ord('0')
    else
      usageError('unknown option ''' + arg + '''');
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

procedure visitUnit(const u: TIdent);
var
  dirs: TPtrList;
  found, path, base, ext: string;
  i, b, x, fid: longint;
  unitDecl: PDecl;
begin
  if mapHas(unitsDone, u.name^.id) or diagShouldStop then
    exit;
  if mapGet(inProgress, u.name^.id) <> nil then
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
begin
  sysInit;
  diagInit;
  listInit(includeDirs);
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
  if outputFile = '' then
    outputFile := '-'; { the dumps; other outputs are not implemented yet }

  if not smLoadFile(inputFile, fid) then
    usageError('cannot open input file ''' + inputFile + ''': ' +
               sysOpenError(inputFile));

  ok := false;
  listInit(units);
  mapInit(unitsDone);
  mapInit(inProgress);
  initTypes;
  case action of
    actParseTree:
      begin
        prog := parseProgram(fid, mode);
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
      prog := parseProgram(fid, mode);
      if not diagHasErrors then
        loadUnits(prog^.progUses);
      semaOK := false;
      if not diagHasErrors then
        semaOK := semaCheck(prog, units, mode);
      if action = actAST then
      begin
        { the dump is written even after errors; types appear if Sema ran }
        printAST(outBuf, prog);
        ok := writeOutput;
      end
      else if semaOK then
        diagError(noLoc, 'code generation is not implemented in the ' +
                         'self-hosted compiler yet');
    end;
  end;
  if ok and not diagHasErrors then
    sysExit(0);
  sysExit(1);
end.
