unit pcdriver;
{ The back end of the driver: src/codegen/Backend.cpp and
  src/driver/Linker.cpp. The IR text pcgen writes is compiled in process
  by libLLVM (pcllvm): parsed, verified, optimized with the same pass
  pipeline as the C++ p3 and emitted by a target machine set up like its
  own (SELFHOST.md S11). Executables are linked with the Pascal runtime by
  a C compiler: $P3_CC, else clang-19, clang, cc or gcc. The runtime
  library is $P3_RUNTIME_LIB, else ../lib/libpascalrt.a next to the
  executable. }

interface

uses pcsys, pcutil, pcdiag, pcllvm;

type
  TOutputKind = (okLLVM, okAsm, okObj, okExe);

{ Checks --target and normalizes it into triple; empty: the default
  target. }
function validateTarget(var triple: string): boolean;
{ The data layout of triple's target. }
function moduleLayout(const triple: string; var layout: string): boolean;
{ Writes the IR module ir (named name) as kind to path ('-': standard
  output) at optimization level opt for triple. }
function emitOutput(const ir: TBuf; const name: string; kind: TOutputKind; opt: longint;
                    sizeLevel: longint;
                    const triple, cpu, features, path: string;
                    const unitObjects: TPtrList): boolean;
{ The directory of p3's unit library, searched after -I directories:
  $P3_UNIT_DIR, else ../lib/pascal next to the executable. }
function findUnitLibrary: string;
{ -l and -L options, passed to the linker. }
procedure addLinkerArg(const arg: string);
{ -fno-inline-runtime: from -O1 the runtime's small functions are inlined
  when compiling for the host (the runtime's LLVM IR, libpascalrt.ll, is
  linked into the module before it is optimized) unless this is called. }
procedure disableRuntimeInlining;
{ Writes the module's textual IR to path (the .ll next to a unit's object). }
function writeModuleText(const ir: TBuf; const path: string): boolean;
{ The host CPU and its features (-mcpu=native). }
function hostCPUName: string;
function hostCPUFeatures: string;
{ --version }
procedure printVersion;

implementation

function writeText(const ir: TBuf; const path: string): boolean; forward;

function writeModuleText(const ir: TBuf; const path: string): boolean;
begin
  writeModuleText := writeText(ir, path);
end;

function hostCPUName: string;
begin
  hostCPUName := llvmHostCPU;
end;

function hostCPUFeatures: string;
begin
  hostCPUFeatures := llvmHostCPUFeatures;
end;

{$I version.inc}

var
  libraryDirs, libraries: TPtrList; { of PName: -L, -l arguments }
  noInlineRuntime: boolean;

procedure disableRuntimeInlining;
begin
  noInlineRuntime := true;
end;

procedure addLinkerArg(const arg: string);
begin
  if copy(arg, 1, 2) = '-L' then
    listAdd(libraryDirs, intern(arg))
  else
    listAdd(libraries, intern(arg));
end;

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

function findLinkerDriver: string;
var
  fromEnv: boolean;
  value: string;
  cc: string;
begin
  cc := findTool('P3_CC', 'clang-19 clang cc gcc', fromEnv);
  if cc = '' then
  begin
    if fromEnv and sysGetEnv('P3_CC', value) then
      diagError(noLoc, 'cannot find linker driver ''' + value + ''' (from P3_CC)')
    else
      diagError(noLoc, 'cannot find a C compiler to link with (tried clang-19, ' +
                       'clang, cc, gcc); set P3_CC');
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
  if sysGetEnv('P3_RUNTIME_LIB', value) then
    exit(value);
  exe := sysExecutablePath;
  findRuntimeLibrary := removeDots(pathJoin(pathDirName(exe), '../lib/libpascalrt.a'));
end;

{ The runtime as LLVM IR, linked into optimized programs for inlining:
  $P3_RUNTIME_IR, else libpascalrt.ll next to the runtime library. It
  need not exist. }
function findRuntimeIR: string;
var
  value: string;
begin
  if sysGetEnv('P3_RUNTIME_IR', value) then
    exit(value);
  findRuntimeIR := pathJoin(pathDirName(findRuntimeLibrary), 'libpascalrt.ll');
end;

function findUnitLibrary: string;
var
  value, exe: string;
begin
  if sysGetEnv('P3_UNIT_DIR', value) then
    exit(value);
  exe := sysExecutablePath;
  findUnitLibrary := removeDots(pathJoin(pathDirName(exe), '../lib/pascal'));
end;

function validateTarget(var triple: string): boolean;
var
  err: string;
begin
  if triple = '' then
    triple := llvmDefaultTriple
  else
    triple := llvmNormalizeTriple(triple);
  if llvmLookupTarget(triple, err) = nil then
  begin
    diagError(noLoc, 'unsupported target ''' + triple + ''': ' + err);
    exit(false);
  end;
  validateTarget := true;
end;

function moduleLayout(const triple: string; var layout: string): boolean;
var
  err: string;
  target, tm: pointer;
begin
  moduleLayout := false;
  target := llvmLookupTarget(triple, err);
  if target = nil then
    exit;
  tm := llvmTargetMachine(target, triple, 0, '', '');
  if tm = nil then
  begin
    diagError(noLoc, 'could not create target machine for ''' + triple + '''');
    exit;
  end;
  layout := llvmDataLayout(tm);
  moduleLayout := true;
end;

{ -mmacosx-version-min for a Mach-O target: the object's target triple
  records the deployment target, while clang's default may be older (which
  makes ld warn). Darwin N maps to macOS N-9 (25 -> 16). }
function macDeploymentMin(const triple: string): string;
var
  p, i, major: longint;
  majorStr, minorStr: string;
begin
  macDeploymentMin := '';
  p := pos('darwin', triple);
  if p > 0 then
  begin
    i := p + 6;
    majorStr := '';
    while (i <= length(triple)) and (triple[i] >= '0') and (triple[i] <= '9') do
    begin
      majorStr := majorStr + triple[i];
      i := i + 1;
    end;
    if majorStr = '' then
      exit;
    major := 0;
    for i := 1 to length(majorStr) do
      major := major * 10 + (ord(majorStr[i]) - ord('0'));
    if major >= 20 then
      major := major - 9
    else
      major := 10;
    { the object's target version: the Darwin major, minor 0 }
    macDeploymentMin := intToStr(major) + '.0';
    exit;
  end;
  p := pos('macosx', triple);
  if p = 0 then
    p := pos('macos', triple);
  if p > 0 then
  begin
    if pos('macosx', triple) > 0 then
      i := p + 6
    else
      i := p + 5;
    majorStr := '';
    minorStr := '';
    while (i <= length(triple)) and (triple[i] >= '0') and (triple[i] <= '9') do
    begin
      majorStr := majorStr + triple[i];
      i := i + 1;
    end;
    if (i <= length(triple)) and (triple[i] = '.') then
    begin
      i := i + 1;
      while (i <= length(triple)) and (triple[i] >= '0') and (triple[i] <= '9') do
      begin
        minorStr := minorStr + triple[i];
        i := i + 1;
      end;
    end;
    if majorStr <> '' then
    begin
      if minorStr = '' then
        minorStr := '0';
      macDeploymentMin := majorStr + '.' + minorStr;
    end;
  end;
end;

function isMachOTriple(const triple: string): boolean;
begin
  isMachOTriple := (pos('darwin', triple) > 0) or (pos('macos', triple) > 0) or
                   (pos('apple', triple) > 0);
end;

{ Finds a library named as a file in the -L directories. }
function findLibraryFile(const fname: string; const dirs: TPtrList): string;
var
  i: longint;
  d, path: string;
begin
  findLibraryFile := '';
  if pos('/', fname) > 0 then
    exit(fname);
  for i := 0 to dirs.count - 1 do
  begin
    d := nameStr(PName(listGet(dirs, i)));
    if copy(d, 1, 2) = '-L' then
      d := copy(d, 3, length(d));
    path := pathJoin(d, fname);
    if sysFileExists(path) then
      exit(path);
  end;
end;

function linkExecutable(const obj: string; const unitObjects: TPtrList;
                        const path, triple: string): boolean;
var
  runtime, cc, e, l, f, found: string;
  cmd: TBuf;
  status, i: longint;
begin
  linkExecutable := false;
  runtime := findRuntimeLibrary;
  if not sysFileExists(runtime) then
  begin
    diagError(noLoc, 'cannot find the Pascal runtime library ''' + runtime +
                     '''; set P3_RUNTIME_LIB');
    exit;
  end;
  cc := findLinkerDriver;
  if cc = '' then
    exit;
  bufInit(cmd);
  addArg(cmd, cc);
  addArg(cmd, obj);
  for i := 0 to unitObjects.count - 1 do
    addArg(cmd, nameStr(PName(listGet(unitObjects, i))));
  addArg(cmd, runtime);
  { -L directories, then -l libraries (each in command-line order). A
    library named as a file ("-l:name") is a GNU ld extension; on Mach-O
    resolve the file in the -L directories and pass its path. }
  for i := 0 to libraryDirs.count - 1 do
    addArg(cmd, nameStr(PName(listGet(libraryDirs, i))));
  for i := 0 to libraries.count - 1 do
  begin
    l := nameStr(PName(listGet(libraries, i)));
    if isMachOTriple(triple) and (copy(l, 1, 3) = '-l:') then
    begin
      f := copy(l, 4, length(l));
      found := findLibraryFile(f, libraryDirs);
      if found <> '' then
        addArg(cmd, found)
      else
        addArg(cmd, f);
    end
    else
      addArg(cmd, l);
  end;
  e := macDeploymentMin(triple);
  if e <> '' then
    addArg(cmd, '-mmacosx-version-min=' + e);
  { dead-code stripping of the unused sections (item 18) }
  if (pos('darwin', triple) > 0) or (pos('macos', triple) > 0) or
     (pos('apple', triple) > 0) then
    addArg(cmd, '-Wl,-dead_strip')
  else if (pos('windows', triple) > 0) or (pos('mingw', triple) > 0) or
          (pos('win32', triple) > 0) then
    addArg(cmd, '-Wl,/OPT:REF')
  else
    addArg(cmd, '-Wl,--gc-sections');
  addArg(cmd, '-lm');
  addArg(cmd, '-o');
  addArg(cmd, path);
  status := sysRunChars(cmd.data, cmd.len);
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
    diagError(noLoc, 'cannot open output file ''' + path + ''': ' +
               sysOpenError(path));
    writeText := false;
  end;
end;

procedure reportLLVMError(sev: TSeverity; const prefix: string; const err: TBuf);
var
  b: TBuf;
begin
  bufInit(b);
  bufAdd(b, prefix);
  bufAddChars(b, err.data, 0, err.len);
  diagReportBuf(sev, noLoc, b, noRange, noRange);
  bufFree(b);
end;

{ The unit's IR next to its object: path.o -> path.ll. }
function objectIRPath(const obj: string): string;
var
  i: longint;
  r: string;
begin
  r := obj;
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
  objectIRPath := r + '.ll';
end;

function emitOutput(const ir: TBuf; const name: string; kind: TOutputKind; opt: longint;
                    sizeLevel: longint;
                    const triple, cpu, features, path: string;
                    const unitObjects: TPtrList): boolean;
var
  target, tm, m: pointer;
  err: TBuf;
  e, obj, runtimeIR, rtProblem: string;
  ok: boolean;
  i: longint;
  unitIRs: TPtrList;
  code: TBuf;
begin
  emitOutput := false;
  { at -O0 the IR is printed as generated, like the C++ p3 does }
  if (kind = okLLVM) and (opt = 0) then
    exit(writeText(ir, path));
  if (kind = okExe) and (triple <> llvmDefaultTriple) then
  begin
    diagError(noLoc, 'linking for a non-host target (''' + triple +
                     ''') is not supported yet; use -c, -S or --emit-llvm');
    exit;
  end;
  target := llvmLookupTarget(triple, e);
  tm := nil;
  if target <> nil then
    tm := llvmTargetMachine(target, triple, opt, cpu, features);
  if tm = nil then
  begin
    diagError(noLoc, 'could not create target machine for ''' + triple + '''');
    exit;
  end;
  bufInit(err);
  { the units' IR, for cross-unit inlining at -O1+ (item 17) }
  listInit(unitIRs);
  for i := 0 to unitObjects.count - 1 do
  begin
    e := objectIRPath(nameStr(PName(listGet(unitObjects, i))));
    if sysFileExists(e) then
      listAdd(unitIRs, intern(e));
  end;
  { the runtime's IR is built for the host }
  runtimeIR := '';
  if (opt > 0) and not noInlineRuntime and (triple = llvmDefaultTriple) then
  begin
    runtimeIR := findRuntimeIR;
    if not sysFileExists(runtimeIR) then
      runtimeIR := '';
  end;
  m := llvmParseAndOptimize(ir, name, tm, opt, sizeLevel, unitIRs, runtimeIR,
                            cpu, features, rtProblem, err);
  listFree(unitIRs);
  if rtProblem <> '' then
    diagWarning(noLoc, 'cannot read the runtime''s LLVM IR ''' + runtimeIR + ''' (' +
                       rtProblem + '); runtime functions are not inlined');
  if m = nil then
  begin
    reportLLVMError(sevFatal, 'internal compiler error: generated invalid LLVM IR:' + #10,
                    err);
    exit;
  end;
  if kind = okLLVM then
  begin
    ok := llvmPrintModule(m, path, err);
    if not ok then
      reportLLVMError(sevError, 'cannot open output file ''' + path + ''': ', err);
    bufFree(err);
    exit(ok);
  end;
  bufInit(code);
  if not llvmEmit(tm, m, kind <> okAsm, code, err) then
  begin
    reportLLVMError(sevError, 'target ''' + triple + ''' cannot emit this kind of file: ',
                    err);
    exit;
  end;
  bufFree(err);
  if kind = okExe then
  begin
    obj := sysTempFile('.o');
    if obj = '' then
    begin
      diagError(noLoc, 'cannot create temporary file');
      exit;
    end;
    ok := writeText(code, obj) and linkExecutable(obj, unitObjects, path, triple);
    sysRemoveFile(obj);
    exit(ok);
  end;
  ok := writeText(code, path);
  { an object file needs a seekable stream (as LLVM's writer does) }
  if ok and (kind = okObj) and (path = '-') and not sysIsSeekable(1) then
  begin
    diagError(noLoc, 'error writing ''-'': Illegal seek');
    ok := false;
  end;
  emitOutput := ok;
end;

procedure printVersion;
begin
  sysWrite(sysOut, 'p3 version ' + p3Version + ' (LLVM ' + llvmVersion + ')' + #10);
  sysWrite(sysOut, '  Default target: ' + llvmDefaultTriple + #10);
  sysWrite(sysOut, '  Host CPU: ' + llvmHostCPU + #10);
end;

begin
  listInit(libraryDirs);
  listInit(libraries);
end.
