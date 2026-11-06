unit pcdriver;
{ The back end of the driver (SELFHOST.md decision D2): the IR text that
  pcgen writes is optimized, assembled and compiled by clang, and the
  object file linked with the Pascal runtime by a C compiler, like
  src/driver/Linker.cpp does.

  Tools: $PCC_CLANG, else clang-19 or clang on $PATH, compiles the IR;
  $PCC_CC, else clang-19, clang, cc or gcc, links. The runtime library is
  $PCC_RUNTIME_LIB, else ../lib/libpascalrt.a next to the executable.

  The generated code assumes the data layout of a 64-bit target (pcir). }

interface

uses pcsys, pcutil, pcdiag;

type
  TOutputKind = (okLLVM, okAsm, okObj, okExe);

{ Checks --target (empty: the host), and normalizes it into triple. }
function validateTarget(var triple: string): boolean;
{ Writes the IR module ir as kind to path ('-': standard output) at
  optimization level opt for triple ('' for the host). }
function emitOutput(const ir: TBuf; kind: TOutputKind; opt: longint;
                    const triple, path: string): boolean;
{ The module's target triple (as the C++ pcc writes it: --target
  normalized, else the default triple) and data layout (asked of clang). }
function moduleTarget(const triple: string; var moduleTriple, layout: string): boolean;
{ --version }
procedure printVersion;

implementation

{$I version.inc}

var
  clangPath: string;   { '' until found }
  hostTriple: string;  { '' until asked }

{ s quoted for the shell }
function shellQuote(const s: string): string;
var
  r: string;
  i: longint;
begin
  r := '''';
  for i := 1 to length(s) do
    if s[i] = '''' then
      r := r + '''\'''''
    else
      r := r + s[i];
  shellQuote := r + '''';
end;

procedure addArg(var cmd: TBuf; const s: string);
begin
  if cmd.len > 0 then
    bufAddChar(cmd, ' ');
  bufAdd(cmd, shellQuote(s));
end;

function run(const cmd: TBuf): longint;
begin
  run := sysRunChars(cmd.data, cmd.len);
end;

{ The tool named by the environment variable env (a path, or a name to
  look up in $PATH), else the first of names (separated by spaces) found
  in $PATH; '' if none. }
function findTool(const env, names: string; var fromEnv: boolean): string;
var
  value, name, found: string;
  i: longint;
begin
  fromEnv := false;
  if sysGetEnv(env, value) and (value <> '') then
  begin
    fromEnv := true;
    if pos('/', value) > 0 then
      exit(value);
    findTool := sysFindProgram(value);
    exit;
  end;
  name := '';
  for i := 1 to length(names) + 1 do
    if (i > length(names)) or (names[i] = ' ') then
    begin
      if name <> '' then
      begin
        found := sysFindProgram(name);
        if found <> '' then
          exit(found);
      end;
      name := '';
    end
    else
      name := name + names[i];
  findTool := '';
end;

{ clang, for compiling IR; reports an error if none is found. }
function findClang(var path: string): boolean;
var
  fromEnv: boolean;
  value: string;
begin
  if clangPath = '' then
  begin
    clangPath := findTool('PCC_CLANG', 'clang-19 clang', fromEnv);
    if clangPath = '' then
    begin
      if fromEnv and sysGetEnv('PCC_CLANG', value) then
        diagError(noLoc, 'cannot find clang ''' + value + ''' (from PCC_CLANG)')
      else
        diagError(noLoc, 'cannot find clang to compile the generated LLVM IR ' +
                         '(tried clang-19, clang); set PCC_CLANG');
      exit(false);
    end;
  end;
  path := clangPath;
  findClang := true;
end;

function findLinkerDriver: string;
var
  fromEnv: boolean;
  value: string;
  cc: string;
begin
  cc := findTool('PCC_CC', 'clang-19 clang cc gcc', fromEnv);
  if cc = '' then
  begin
    if fromEnv and sysGetEnv('PCC_CC', value) then
      diagError(noLoc, 'cannot find linker driver ''' + value + ''' (from PCC_CC)')
    else
      diagError(noLoc, 'cannot find a C compiler to link with (tried clang-19, ' +
                       'clang, cc, gcc); set PCC_CC');
  end;
  findLinkerDriver := cc;
end;

{ path with '.' and '..' components removed (an absolute path) }
function removeDots(const path: string): string;
var
  parts: array[0..63] of string[255];
  n, i, start: longint;
  part, r: string;
begin
  n := 0;
  start := 1;
  for i := 1 to length(path) + 1 do
    if (i > length(path)) or (path[i] = '/') then
    begin
      part := copy(path, start, i - start);
      start := i + 1;
      if (part = '') or (part = '.') then
        continue;
      if part = '..' then
      begin
        if n > 0 then
          n := n - 1;
        continue;
      end;
      if n <= 63 then
      begin
        parts[n] := part;
        n := n + 1;
      end;
    end;
  r := '';
  for i := 0 to n - 1 do
    r := r + '/' + parts[i];
  if r = '' then
    r := '/';
  removeDots := r;
end;

function findRuntimeLibrary: string;
var
  value, exe: string;
begin
  if sysGetEnv('PCC_RUNTIME_LIB', value) then
    exit(value);
  exe := sysExecutablePath;
  findRuntimeLibrary := removeDots(pathJoin(pathDirName(exe), '../lib/libpascalrt.a'));
end;

{ The first line a command prints on standard output ('' if it fails). }
function captureLine(var cmd: TBuf): string;
var
  tmp, line: string;
  data: PCharArray;
  len, i: int64;
begin
  captureLine := '';
  tmp := sysTempFile('.txt');
  if tmp = '' then
    exit;
  bufAdd(cmd, ' > ' + shellQuote(tmp) + ' 2>/dev/null');
  if (run(cmd) = 0) and sysReadFile(tmp, data, len) then
  begin
    line := '';
    i := 0;
    {$R-}
    while (i < len) and (data^[i] <> #10) and (length(line) < 255) do
    begin
      line := line + data^[i];
      i := i + 1;
    end;
    {$R+}
    captureLine := line;
  end;
  sysRemoveFile(tmp);
end;

{ The line of a command's standard output that starts with prefix, after
  the prefix ('' if none). }
function captureAfter(var cmd: TBuf; const prefix: string): string;
var
  tmp, line: string;
  data: PCharArray;
  len, i: int64;
begin
  captureAfter := '';
  tmp := sysTempFile('.txt');
  if tmp = '' then
    exit;
  bufAdd(cmd, ' > ' + shellQuote(tmp) + ' 2>/dev/null');
  if (run(cmd) = 0) and sysReadFile(tmp, data, len) then
  begin
    line := '';
    {$R-}
    for i := 0 to len do
      if (i = len) or (data^[i] = #10) then
      begin
        if copy(line, 1, length(prefix)) = prefix then
        begin
          captureAfter := copy(line, length(prefix) + 1, 255);
          break;
        end;
        line := '';
      end
      else if length(line) < 255 then
        line := line + data^[i];
    {$R+}
  end;
  sysRemoveFile(tmp);
end;

{ The host's target triple, as clang reports it. }
function defaultTriple: string;
var
  cmd: TBuf;
  clang: string;
begin
  if (hostTriple = '') and findClang(clang) then
  begin
    bufInit(cmd);
    addArg(cmd, clang);
    addArg(cmd, '-print-target-triple');
    hostTriple := captureLine(cmd);
    bufFree(cmd);
  end;
  defaultTriple := hostTriple;
end;

{ Architectures LLVM has targets for (the first triple component). }
function knownArch(const arch: string): boolean;
const
  numArchs = 46;
  archs: array[1..numArchs] of string[15] = (
    'aarch64', 'aarch64_be', 'aarch64_32', 'arm64', 'arm64_32', 'arm', 'armeb',
    'thumb', 'thumbeb', 'x86_64', 'x86_64h', 'amd64', 'i386', 'i486', 'i586',
    'i686', 'x86', 'riscv32', 'riscv64', 'wasm32', 'wasm64', 'powerpc',
    'powerpcle', 'powerpc64', 'powerpc64le', 'ppc', 'ppc64', 'ppc64le',
    'mips', 'mipsel', 'mips64', 'mips64el', 'sparc', 'sparcv9', 'sparcel',
    's390x', 'systemz', 'loongarch32', 'loongarch64', 'hexagon', 'avr',
    'msp430', 'bpf', 'bpfel', 'bpfeb', 've');
var
  i: longint;
begin
  for i := 1 to numArchs do
    if arch = archs[i] then
      exit(true);
  { arm and thumb carry a version: armv7a, thumbv7m, ... }
  knownArch := (copy(arch, 1, 4) = 'armv') or (copy(arch, 1, 6) = 'thumbv') or
               (copy(arch, 1, 7) = 'aarch64');
end;

{ llvm::Triple::normalize for the common cases: a missing vendor becomes
  'unknown' (x86_64-linux-gnu -> x86_64-unknown-linux-gnu). }
function normalizeTriple(const t: string): string;
var
  parts: array[0..5] of string[64];
  n, i: longint;
  os, r: string;
begin
  n := 0;
  parts[0] := '';
  for i := 1 to length(t) do
    if (t[i] = '-') and (n < 5) then
    begin
      n := n + 1;
      parts[n] := '';
    end
    else
      parts[n] := parts[n] + t[i];
  n := n + 1;
  if n < 2 then
    exit(t);
  os := parts[1];
  if (copy(os, 1, 5) = 'linux') or (copy(os, 1, 6) = 'darwin') or
     (copy(os, 1, 5) = 'macos') or (copy(os, 1, 3) = 'ios') or
     (copy(os, 1, 7) = 'windows') or (copy(os, 1, 7) = 'freebsd') or
     (copy(os, 1, 6) = 'netbsd') or (copy(os, 1, 7) = 'openbsd') or
     (os = 'wasi') or (os = 'none') or (os = 'emscripten') then
  begin
    r := parts[0] + '-unknown';
    for i := 1 to n - 1 do
      r := r + '-' + parts[i];
    exit(r);
  end;
  normalizeTriple := t;
end;

function moduleTarget(const triple: string; var moduleTriple, layout: string): boolean;
var
  clang: string;
  cmd: TBuf;
  l: string;
begin
  moduleTarget := false;
  if triple = '' then
    moduleTriple := defaultTriple
  else
    moduleTriple := triple;
  if not findClang(clang) then
    exit;
  bufInit(cmd);
  addArg(cmd, clang);
  addArg(cmd, '--target=' + moduleTriple);
  addArg(cmd, '-S');
  addArg(cmd, '-emit-llvm');
  addArg(cmd, '-x');
  addArg(cmd, 'c');
  addArg(cmd, '/dev/null');
  addArg(cmd, '-o');
  addArg(cmd, '-');
  l := captureAfter(cmd, 'target datalayout = "');
  bufFree(cmd);
  if l = '' then
  begin
    diagError(noLoc, 'cannot get the data layout of target ''' + moduleTriple +
                     ''' from ''' + clang + '''');
    exit;
  end;
  layout := copy(l, 1, length(l) - 1); { without the closing quote }
  moduleTarget := true;
end;

function validateTarget(var triple: string): boolean;
var
  arch: string;
begin
  if triple = '' then
  begin
    { the host: nothing to check (clang knows it) }
    validateTarget := true;
    exit;
  end;
  triple := normalizeTriple(triple);
  arch := triple;
  if pos('-', arch) > 0 then
    arch := copy(arch, 1, pos('-', arch) - 1);
  if not knownArch(arch) then
  begin
    diagError(noLoc, 'unsupported target ''' + triple +
                     ''': No available targets are compatible with triple "' +
                     triple + '"');
    exit(false);
  end;
  validateTarget := true;
end;

{ Runs clang on the IR in llPath: '-S', '-c' or '-S -emit-llvm'. }
function runClang(const llPath, mode1, mode2: string; opt: longint;
                  const triple, path: string): boolean;
var
  cmd: TBuf;
  clang: string;
  status: longint;
begin
  runClang := false;
  if not findClang(clang) then
    exit;
  bufInit(cmd);
  addArg(cmd, clang);
  addArg(cmd, mode1);
  if mode2 <> '' then
    addArg(cmd, mode2);
  if mode2 = '' then
  begin
    { assembly and objects as the C++ pcc writes them: no comments, no
      address-significance table }
    addArg(cmd, '-fno-verbose-asm');
    addArg(cmd, '-fno-addrsig');
  end;
  addArg(cmd, '-O' + intToStr(opt));
  if triple <> '' then
    addArg(cmd, '--target=' + triple);
  { the IR has no triple: clang supplies the target's }
  addArg(cmd, '-Wno-override-module');
  addArg(cmd, llPath);
  addArg(cmd, '-o');
  addArg(cmd, path);
  status := run(cmd);
  bufFree(cmd);
  if status <> 0 then
  begin
    diagError(noLoc, 'compiling the generated LLVM IR with ''' + clang +
                     ''' failed with exit code ' + intToStr(status));
    exit;
  end;
  runClang := true;
end;

function linkExecutable(const obj, path: string): boolean;
var
  runtime, cc: string;
  cmd: TBuf;
  status: longint;
begin
  linkExecutable := false;
  runtime := findRuntimeLibrary;
  if not sysFileExists(runtime) then
  begin
    diagError(noLoc, 'cannot find the Pascal runtime library ''' + runtime +
                     '''; set PCC_RUNTIME_LIB');
    exit;
  end;
  cc := findLinkerDriver;
  if cc = '' then
    exit;
  bufInit(cmd);
  addArg(cmd, cc);
  addArg(cmd, obj);
  addArg(cmd, runtime);
  addArg(cmd, '-lm');
  addArg(cmd, '-o');
  addArg(cmd, path);
  status := run(cmd);
  bufFree(cmd);
  if status < 0 then
  begin
    diagError(noLoc, 'failed to run linker ''' + cc + '''');
    exit;
  end;
  if status <> 0 then
  begin
    diagError(noLoc, 'linker command failed with exit code ' + intToStr(status));
    exit;
  end;
  linkExecutable := true;
end;

function writeText(const ir: TBuf; const path: string): boolean;
begin
  writeText := true;
  if path = '-' then
    bufWrite(ir, sysOut)
  else if not sysWriteFile(path, ir.data, ir.len) then
  begin
    diagError(noLoc, 'cannot open output file ''' + path + '''');
    writeText := false;
  end;
end;

function emitOutput(const ir: TBuf; kind: TOutputKind; opt: longint;
                    const triple, path: string): boolean;
var
  ll, obj: string;
  ok: boolean;
begin
  if (kind = okLLVM) and (opt = 0) then
    exit(writeText(ir, path));
  if (kind = okExe) and (triple <> '') and (triple <> defaultTriple) then
  begin
    diagError(noLoc, 'linking for a non-host target (''' + triple +
                     ''') is not supported yet; use -c, -S or --emit-llvm');
    exit(false);
  end;
  ll := sysTempFile('.ll');
  if ll = '' then
  begin
    diagError(noLoc, 'cannot create temporary file');
    exit(false);
  end;
  ok := writeText(ir, ll);
  if ok then
    case kind of
      okLLVM: ok := runClang(ll, '-S', '-emit-llvm', opt, triple, path);
      okAsm: ok := runClang(ll, '-S', '', opt, triple, path);
      okObj: ok := runClang(ll, '-c', '', opt, triple, path);
    else
      begin
        obj := sysTempFile('.o');
        if obj = '' then
        begin
          diagError(noLoc, 'cannot create temporary file');
          ok := false;
        end
        else
        begin
          ok := runClang(ll, '-c', '', opt, triple, obj) and linkExecutable(obj, path);
          sysRemoveFile(obj);
        end;
      end;
    end;
  sysRemoveFile(ll);
  emitOutput := ok;
end;

{ "clang version 19.1.7 (...)" -> 19.1.7 }
function versionIn(const line: string): string;
var
  i: longint;
  v: string;
begin
  versionIn := 'unknown';
  i := pos('version ', line);
  if i = 0 then
    exit;
  i := i + 8;
  v := '';
  while (i <= length(line)) and (line[i] <> ' ') do
  begin
    v := v + line[i];
    i := i + 1;
  end;
  versionIn := v;
end;

procedure printVersion;
var
  clang, llvmVersion, cpu, line, llc: string;
  cmd: TBuf;
  data: PCharArray;
  len, i: int64;
  tmp: string;
begin
  llvmVersion := 'unknown';
  cpu := '';
  if findClang(clang) then
  begin
    bufInit(cmd);
    addArg(cmd, clang);
    addArg(cmd, '--version');
    llvmVersion := versionIn(captureLine(cmd));
    bufFree(cmd);
    { llc next to clang knows the host CPU }
    llc := pathJoin(pathDirName(sysRealPath(clang)), 'llc');
    tmp := sysTempFile('.txt');
    if sysIsExecutable(llc) and (tmp <> '') then
    begin
      bufInit(cmd);
      addArg(cmd, llc);
      addArg(cmd, '--version');
      bufAdd(cmd, ' > ' + shellQuote(tmp) + ' 2>/dev/null');
      if (run(cmd) = 0) and sysReadFile(tmp, data, len) then
      begin
        line := '';
        {$R-}
        for i := 0 to len - 1 do
          if data^[i] = #10 then
          begin
            if pos('Host CPU: ', line) > 0 then
              cpu := copy(line, pos('Host CPU: ', line) + 10, 255);
            line := '';
          end
          else if length(line) < 255 then
            line := line + data^[i];
        {$R+}
      end;
      bufFree(cmd);
    end;
    if tmp <> '' then
      sysRemoveFile(tmp);
  end;
  sysWrite(sysOut, 'pcc version ' + pccVersion + ' (LLVM ' + llvmVersion + ')' + #10);
  sysWrite(sysOut, '  Default target: ' + defaultTriple + #10);
  if (cpu = '') or (cpu = '(unknown)') then
    cpu := 'generic'; { as llvm::sys::getHostCPUName says }
  sysWrite(sysOut, '  Host CPU: ' + cpu + #10);
end;

begin
  clangPath := '';
  hostTriple := '';
end.
