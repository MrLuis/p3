program pcc;
{ The self-hosted Pascal compiler: command line and pipeline. It accepts
  the C++ pcc's options (src/driver/main.cpp) and must behave the same.
  Implemented so far: --emit-tokens (S4). }

uses pcsys, pcutil, pcdiag, pclex;

type
  TAction = (actTokens, actAST, actLLVM, actAsm, actObj, actExe);

var
  inputFile, outputFile: string;
  action: TAction;
  mode: TLangMode;
  errorLimit: longint;
  werror, noUnusedVariable: boolean;
  optLevel: longint;
  out: TBuf;

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
    bufAddInt(out, line);
    bufAddChar(out, ':');
    bufAddInt(out, col);
    bufAddChar(out, ' ');
    bufAdd(out, tokenNames[t.kind]);
    bufAdd(out, ' ''');
    bufAddTokenText(out, t);
    bufAddChar(out, '''');
    case t.kind of
      tkIdentifier:
        begin
          bufAdd(out, ' name=');
          bufAddName(out, t.name);
        end;
      tkIntegerLiteral:
        bufAdd(out, ' value=' + uintToStr(t.intValue));
      tkRealLiteral:
        bufAdd(out, ' value=' + formatG(t.realValue, 6));
      tkStringLiteral:
        begin
          bufAdd(out, ' value="');
          bufAddEscaped(out, t.strValue^.chars, 0, t.strValue^.len);
          bufAddChar(out, '"');
        end;
    else
      ;
    end;
    bufAddChar(out, #10);
  until (t.kind = tkEof) or diagShouldStop;
end;

{ Writes out to the output file ('-': standard output). }
function writeOutput: boolean;
begin
  writeOutput := true;
  if outputFile = '-' then
    bufWrite(out, sysOut)
  else if not sysWriteFile(outputFile, out.data, out.len) then
  begin
    diagError(noLoc, 'cannot open output file ''' + outputFile + '''');
    writeOutput := false;
  end;
end;

var
  fid: longint;
  ok: boolean;
begin
  sysInit;
  diagInit;
  listInit(includeDirs);
  bufInit(out);
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
  case action of
    actTokens:
      begin
        dumpTokens(fid);
        ok := writeOutput;
      end;
  else
    diagError(noLoc, 'this action is not implemented in the self-hosted ' +
                     'compiler yet (only --emit-tokens)');
  end;
  if ok and not diagHasErrors then
    sysExit(0);
  sysExit(1);
end.
