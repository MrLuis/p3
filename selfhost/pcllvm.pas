unit pcllvm;
{ The self-hosted compiler's back end in process (SELFHOST.md S11): the
  LLVM-C API of libLLVM, used as src/codegen/Backend.cpp uses LLVM. The
  IR text pcgen writes is parsed into a module, verified, optimized with
  the same pass pipeline and compiled by a target machine set up like the
  C++ p3's (CPU "generic", PIC).

  The functions are declared without a library: the compiler is linked
  with libLLVM (LLVM-23 on Linux, LLVM on macOS; see selfhost/README.md).
  The target initialization functions depend on the LLVM build and are
  generated into llvmtargets.inc by utils/gen-llvm-targets.py. }

interface

uses pcsys, pcutil;

{ Initializes the targets, once. }
procedure llvmInit;
{ llvm::sys::getDefaultTargetTriple, Triple::normalize, the host CPU and
  LLVM's version }
function llvmDefaultTriple: string;
function llvmNormalizeTriple(const t: string): string;
function llvmHostCPU: string;
function llvmHostCPUFeatures: string;
function llvmVersion: string;
{ The target for triple, or nil and the error message. }
function llvmLookupTarget(const triple: string; var err: string): pointer;
{ A target machine for triple at an optimization level (0-3), with the
  given CPU and features ('' = the target's default). }
function llvmTargetMachine(target: pointer; const triple: string; opt: longint;
                           const cpu, features: string): pointer;
{ The data layout string of a target machine. }
function llvmDataLayout(tm: pointer): string;
{ Parses IR text into a module (named name), verifies it and, at opt > 0,
  runs default<O[opt]>. Returns the module, or nil and the error.
  unitIRs: the separately compiled units' IR files to link before the
  optimization (their definitions stay available_externally), or empty.
  runtimeIR: the runtime's LLVM IR (libpascalrt.ll) to link into the
  module before it is optimized, so that the runtime's functions can be
  inlined, or ''; if that file cannot be used, rtProblem says why (else
  it is ''). }
function llvmParseAndOptimize(const ir: TBuf; const name: string; tm: pointer; opt: longint;
                              sizeLevel: longint; const unitIRs: TPtrList;
                              const runtimeIR, cpu, features: string;
                              var rtProblem: string; var err: TBuf): pointer;
{ Writes the module as IR text, assembly or an object file. }
function llvmPrintModule(m: pointer; const path: string; var err: TBuf): boolean;
{ Appends the assembly or object code for the module to ob. }
function llvmEmit(tm, m: pointer; obj: boolean; var ob, err: TBuf): boolean;

implementation

{$I llvmtargets.inc}

function LLVMContextCreate: pointer; cdecl; external name 'LLVMContextCreate';
function LLVMCreateMemoryBufferWithMemoryRangeCopy(data: pointer; len: int64;
  name: pchar): pointer; cdecl; external name 'LLVMCreateMemoryBufferWithMemoryRangeCopy';
function LLVMParseIRInContext(ctx, buf: pointer; var m: pointer; var msg: pchar): longint;
  cdecl; external name 'LLVMParseIRInContext';
function LLVMVerifyModule(m: pointer; action: longint; var msg: pchar): longint;
  cdecl; external name 'LLVMVerifyModule';
function LLVMCreateMemoryBufferWithContentsOfFile(path: pchar; var buf: pointer;
  var msg: pchar): longint; cdecl; external name 'LLVMCreateMemoryBufferWithContentsOfFile';
function LLVMGetTarget(m: pointer): pchar; cdecl; external name 'LLVMGetTarget';
procedure LLVMSetTarget(m: pointer; triple: pchar); cdecl; external name 'LLVMSetTarget';
function LLVMGetDataLayoutStr(m: pointer): pchar; cdecl; external name 'LLVMGetDataLayoutStr';
procedure LLVMSetDataLayout(m: pointer; layout: pchar); cdecl;
  external name 'LLVMSetDataLayout';
function LLVMLinkModules2(dest, src: pointer): longint; cdecl;
  external name 'LLVMLinkModules2';
function LLVMGetFirstFunction(m: pointer): pointer; cdecl;
  external name 'LLVMGetFirstFunction';
function LLVMGetNextFunction(f: pointer): pointer; cdecl;
  external name 'LLVMGetNextFunction';
function LLVMGetFirstGlobal(m: pointer): pointer; cdecl;
  external name 'LLVMGetFirstGlobal';
function LLVMGetNextGlobal(g: pointer): pointer; cdecl;
  external name 'LLVMGetNextGlobal';
function LLVMIsDeclaration(v: pointer): longint; cdecl;
  external name 'LLVMIsDeclaration';
function LLVMGetLinkage(v: pointer): longint; cdecl;
  external name 'LLVMGetLinkage';
function LLVMGetFirstUse(v: pointer): pointer; cdecl;
  external name 'LLVMGetFirstUse';
function LLVMIsGlobalConstant(v: pointer): longint; cdecl;
  external name 'LLVMIsGlobalConstant';
procedure LLVMSetLinkage(v: pointer; linkage: longint); cdecl;
  external name 'LLVMSetLinkage';
procedure LLVMDisposeMessage(msg: pchar); cdecl; external name 'LLVMDisposeMessage';
function LLVMGetDefaultTargetTriple: pchar; cdecl; external name 'LLVMGetDefaultTargetTriple';
function LLVMNormalizeTargetTriple(t: pchar): pchar; cdecl;
  external name 'LLVMNormalizeTargetTriple';
function LLVMGetHostCPUName: pchar; cdecl; external name 'LLVMGetHostCPUName';
function LLVMGetHostCPUFeatures: pchar; cdecl; external name 'LLVMGetHostCPUFeatures';
procedure LLVMAddTargetDependentFunctionAttr(fn: pointer; a, v: pchar); cdecl;
  external name 'LLVMAddTargetDependentFunctionAttr';
procedure LLVMGetVersion(var major, minor, patch: longint); cdecl;
  external name 'LLVMGetVersion';
function LLVMGetTargetFromTriple(t: pchar; var target: pointer; var msg: pchar): longint;
  cdecl; external name 'LLVMGetTargetFromTriple';
function LLVMCreateTargetMachine(target: pointer; triple, cpu, features: pchar;
  level, reloc, model: longint): pointer; cdecl; external name 'LLVMCreateTargetMachine';
function LLVMCreateTargetDataLayout(tm: pointer): pointer; cdecl;
  external name 'LLVMCreateTargetDataLayout';
function LLVMCopyStringRepOfTargetData(td: pointer): pchar; cdecl;
  external name 'LLVMCopyStringRepOfTargetData';
function LLVMCreatePassBuilderOptions: pointer; cdecl;
  external name 'LLVMCreatePassBuilderOptions';
procedure LLVMPassBuilderOptionsSetSLPVectorization(o: pointer; v: longint); cdecl;
  external name 'LLVMPassBuilderOptionsSetSLPVectorization';
procedure LLVMPassBuilderOptionsSetLoopVectorization(o: pointer; v: longint); cdecl;
  external name 'LLVMPassBuilderOptionsSetLoopVectorization';
procedure LLVMPassBuilderOptionsSetLoopInterleaving(o: pointer; v: longint); cdecl;
  external name 'LLVMPassBuilderOptionsSetLoopInterleaving';
procedure LLVMPassBuilderOptionsSetLoopUnrolling(o: pointer; v: longint); cdecl;
  external name 'LLVMPassBuilderOptionsSetLoopUnrolling';
function LLVMRunPasses(m: pointer; passes: pchar; tm, options: pointer): pointer; cdecl;
  external name 'LLVMRunPasses';
function LLVMGetErrorMessage(e: pointer): pchar; cdecl; external name 'LLVMGetErrorMessage';
function LLVMPrintModuleToFile(m: pointer; path: pchar; var msg: pchar): longint; cdecl;
  external name 'LLVMPrintModuleToFile';
function LLVMTargetMachineEmitToMemoryBuffer(tm, m: pointer; kind: longint;
  var msg: pchar; var buf: pointer): longint; cdecl;
  external name 'LLVMTargetMachineEmitToMemoryBuffer';
function LLVMGetBufferStart(buf: pointer): pointer; cdecl; external name 'LLVMGetBufferStart';
function LLVMGetBufferSize(buf: pointer): int64; cdecl; external name 'LLVMGetBufferSize';
procedure LLVMDisposeMemoryBuffer(buf: pointer); cdecl;
  external name 'LLVMDisposeMemoryBuffer';

const
  verifierReturnStatus = 2; { LLVMReturnStatusAction }
  relocPIC = 2;             { LLVMRelocPIC }
  codeModelDefault = 0;     { LLVMCodeModelDefault }
  assemblyFile = 0;         { LLVMAssemblyFile }
  objectFile = 1;           { LLVMObjectFile }

var
  initialized: boolean;

procedure llvmInit;
begin
  if initialized then
    exit;
  llvmInitializeTargets;
  initialized := true;
end;

{ a C string (at most 255 characters) }
function str(p: pchar): string;
var
  s: string;
  i: longint;
  a: PCharArray;
begin
  s := '';
  if p = nil then
    exit(s);
  a := PCharArray(p);
  i := 0;
  {$R-}
  while (a^[i] <> #0) and (i < 255) do
  begin
    s := s + a^[i];
    i := i + 1;
  end;
  {$R+}
  str := s;
end;

{ appends a C string of any length and disposes of it }
procedure takeMessage(var b: TBuf; p: pchar);
var
  a: PCharArray;
  i: int64;
begin
  if p = nil then
    exit;
  a := PCharArray(p);
  i := 0;
  {$R-}
  while a^[i] <> #0 do
  begin
    bufAddChar(b, a^[i]);
    i := i + 1;
  end;
  {$R+}
  LLVMDisposeMessage(p);
end;

{ s as a C string in buf }
function cstr(const s: string; var buf: string): pchar;
begin
  buf := s + #0;
  cstr := @buf[1];
end;

function llvmDefaultTriple: string;
var
  p: pchar;
begin
  p := LLVMGetDefaultTargetTriple;
  llvmDefaultTriple := str(p);
  LLVMDisposeMessage(p);
end;

function llvmNormalizeTriple(const t: string): string;
var
  p: pchar;
  b: string;
begin
  p := LLVMNormalizeTargetTriple(cstr(t, b));
  llvmNormalizeTriple := str(p);
  LLVMDisposeMessage(p);
end;

function llvmHostCPU: string;
var
  p: pchar;
begin
  p := LLVMGetHostCPUName;
  llvmHostCPU := str(p);
  LLVMDisposeMessage(p);
end;

function llvmHostCPUFeatures: string;
var
  p: pchar;
begin
  p := LLVMGetHostCPUFeatures;
  if p = nil then
    exit('');
  llvmHostCPUFeatures := str(p);
  LLVMDisposeMessage(p);
end;

function llvmVersion: string;
var
  major, minor, patch: longint;
begin
  LLVMGetVersion(major, minor, patch);
  llvmVersion := intToStr(major) + '.' + intToStr(minor) + '.' + intToStr(patch);
end;

function llvmLookupTarget(const triple: string; var err: string): pointer;
var
  target: pointer;
  msg: pchar;
  b: string;
begin
  llvmInit;
  target := nil;
  msg := nil;
  err := '';
  if LLVMGetTargetFromTriple(cstr(triple, b), target, msg) <> 0 then
  begin
    err := str(msg);
    LLVMDisposeMessage(msg);
    exit(nil);
  end;
  llvmLookupTarget := target;
end;

function llvmTargetMachine(target: pointer; const triple: string; opt: longint;
                           const cpu, features: string): pointer;
var
  b, c, f: string;
  cpu_, feats: string;
begin
  if cpu = '' then
    cpu_ := 'generic'
  else
    cpu_ := cpu;
  feats := features;
  llvmTargetMachine := LLVMCreateTargetMachine(target, cstr(triple, b),
                                               cstr(cpu_, c), cstr(feats, f), opt,
                                               relocPIC, codeModelDefault);
end;

function llvmDataLayout(tm: pointer): string;
var
  p: pchar;
begin
  p := LLVMCopyStringRepOfTargetData(LLVMCreateTargetDataLayout(tm));
  llvmDataLayout := str(p);
  LLVMDisposeMessage(p);
end;

{ Links the runtime's LLVM IR at path into m (Backend.cpp's linkRuntimeIR):
  its functions stay available_externally, and calls that are not inlined
  go to the library. Returns '' or why the file cannot be used. }
function linkRuntimeIR(ctx, m: pointer; const path, cpu, features: string): string;
var
  buf, rt: pointer;
  msg: pchar;
  pb, a, b, c, d: string;
  f: pointer;
begin
  linkRuntimeIR := '';
  buf := nil;
  msg := nil;
  if LLVMCreateMemoryBufferWithContentsOfFile(cstr(path, pb), buf, msg) <> 0 then
  begin
    linkRuntimeIR := str(msg);
    LLVMDisposeMessage(msg);
    exit;
  end;
  rt := nil;
  msg := nil;
  if LLVMParseIRInContext(ctx, buf, rt, msg) <> 0 then
  begin
    linkRuntimeIR := str(msg);
    LLVMDisposeMessage(msg);
    exit;
  end;
  { the file has no target of its own (utils/gen-runtime-ir.py) }
  LLVMSetTarget(rt, LLVMGetTarget(m));
  LLVMSetDataLayout(rt, LLVMGetDataLayoutStr(m));
  { the runtime IR is generic: give its functions the program's target }
  f := LLVMGetFirstFunction(rt);
  while f <> nil do
  begin
    if LLVMIsDeclaration(f) = 0 then
    begin
      if cpu <> '' then
        LLVMAddTargetDependentFunctionAttr(f, cstr('target-cpu', a), cstr(cpu, b));
      if features <> '' then
        LLVMAddTargetDependentFunctionAttr(f, cstr('target-features', c), cstr(features, d));
    end;
    f := LLVMGetNextFunction(f);
  end;
  if LLVMLinkModules2(m, rt) <> 0 then
    linkRuntimeIR := 'it cannot be linked';
end;

{ A module's definitions with external linkage become available_externally:
  the body is available to inline, while the object still provides it. }
{ Does the unit have a mutable internal (or private) global? Then importing
  its IR would give the inlined code and the object different copies. }
function hasMutableInternalGlobal(u: pointer): boolean;
var
  g: pointer;
  l: longint;
begin
  hasMutableInternalGlobal := false;
  g := LLVMGetFirstGlobal(u);
  while g <> nil do
  begin
    l := LLVMGetLinkage(g);
    if ((l = 8) or (l = 9)) and (LLVMIsGlobalConstant(g) = 0) and
       (LLVMGetFirstUse(g) <> nil) then
      exit(true);
    g := LLVMGetNextGlobal(g);
  end;
end;

procedure markAvailableExternally(u: pointer);
var
  v: pointer;
begin
  v := LLVMGetFirstFunction(u);
  while v <> nil do
  begin
    if (LLVMIsDeclaration(v) = 0) and (LLVMGetLinkage(v) = 0) then
      LLVMSetLinkage(v, 1);
    v := LLVMGetNextFunction(v);
  end;
  v := LLVMGetFirstGlobal(u);
  while v <> nil do
  begin
    if (LLVMIsDeclaration(v) = 0) and (LLVMGetLinkage(v) = 0) then
      LLVMSetLinkage(v, 1);
    v := LLVMGetNextGlobal(v);
  end;
end;

{ Links the units' IR files (item 17) into m; unreadable or missing files
  are ignored. }
function linkUnitIRs(ctx, m: pointer; const paths: TPtrList): string;
var
  i: longint;
  u, buf: pointer;
  msg: pchar;
  pb: string;
begin
  linkUnitIRs := '';
  for i := 0 to paths.count - 1 do
  begin
    buf := nil;
    msg := nil;
    if LLVMCreateMemoryBufferWithContentsOfFile(
         cstr(nameStr(PName(listGet(paths, i))), pb), buf, msg) <> 0 then
    begin
      LLVMDisposeMessage(msg);
      continue;
    end;
    u := nil;
    msg := nil;
    if LLVMParseIRInContext(ctx, buf, u, msg) <> 0 then
    begin
      LLVMDisposeMessage(msg);
      continue;
    end;
    LLVMSetTarget(u, LLVMGetTarget(m));
    LLVMSetDataLayout(u, LLVMGetDataLayoutStr(m));
    { importing mutable internal state would duplicate it }
    if hasMutableInternalGlobal(u) then
      continue;
    markAvailableExternally(u);
    if LLVMLinkModules2(m, u) <> 0 then
      linkUnitIRs := 'a unit cannot be linked';
  end;
end;

function llvmParseAndOptimize(const ir: TBuf; const name: string; tm: pointer; opt: longint;
                              sizeLevel: longint; const unitIRs: TPtrList;
                              const runtimeIR, cpu, features: string;
                              var rtProblem: string; var err: TBuf): pointer;
var
  ctx, buf, m, e, opts: pointer;
  msg: pchar;
  nb, passes: string;
begin
  llvmParseAndOptimize := nil;
  rtProblem := '';
  ctx := LLVMContextCreate;
  { the buffer's name becomes the module identifier }
  buf := LLVMCreateMemoryBufferWithMemoryRangeCopy(ir.data, ir.len, cstr(name, nb));
  m := nil;
  msg := nil;
  if LLVMParseIRInContext(ctx, buf, m, msg) <> 0 then
  begin
    takeMessage(err, msg);
    exit;
  end;
  msg := nil;
  if LLVMVerifyModule(m, verifierReturnStatus, msg) <> 0 then
  begin
    takeMessage(err, msg);
    exit;
  end;
  if msg <> nil then
    LLVMDisposeMessage(msg);
  if opt > 0 then
  begin
    if unitIRs.count > 0 then
      linkUnitIRs(ctx, m, unitIRs);
    if runtimeIR <> '' then
      rtProblem := linkRuntimeIR(ctx, m, runtimeIR, cpu, features);
    { llvm::PipelineTuningOptions' defaults, as the C++ p3's PassBuilder
      has them }
    opts := LLVMCreatePassBuilderOptions;
    LLVMPassBuilderOptionsSetLoopInterleaving(opts, 1);
    LLVMPassBuilderOptionsSetLoopVectorization(opts, 1);
    if (sizeLevel > 0) or (opt >= 2) then
      LLVMPassBuilderOptionsSetSLPVectorization(opts, 1)
    else
      LLVMPassBuilderOptionsSetSLPVectorization(opts, 0);
    LLVMPassBuilderOptionsSetLoopUnrolling(opts, 1);
    if sizeLevel = 1 then
      e := LLVMRunPasses(m, cstr('default<Os>', passes), tm, opts)
    else if sizeLevel = 2 then
      e := LLVMRunPasses(m, cstr('default<Oz>', passes), tm, opts)
    else
      e := LLVMRunPasses(m, cstr('default<O' + intToStr(opt) + '>', passes), tm, opts);
    if e <> nil then
    begin
      takeMessage(err, LLVMGetErrorMessage(e));
      exit;
    end;
  end;
  llvmParseAndOptimize := m;
end;

function llvmPrintModule(m: pointer; const path: string; var err: TBuf): boolean;
var
  msg: pchar;
  b: string;
begin
  msg := nil;
  llvmPrintModule := LLVMPrintModuleToFile(m, cstr(path, b), msg) = 0;
  takeMessage(err, msg);
end;

function llvmEmit(tm, m: pointer; obj: boolean; var ob, err: TBuf): boolean;
var
  msg: pchar;
  kind: longint;
  buf: pointer;
begin
  msg := nil;
  buf := nil;
  kind := assemblyFile;
  if obj then
    kind := objectFile;
  llvmEmit := false;
  if LLVMTargetMachineEmitToMemoryBuffer(tm, m, kind, msg, buf) <> 0 then
  begin
    takeMessage(err, msg);
    exit;
  end;
  bufAddChars(ob, PCharArray(LLVMGetBufferStart(buf)), 0, LLVMGetBufferSize(buf));
  LLVMDisposeMemoryBuffer(buf);
  llvmEmit := true;
end;

begin
  initialized := false;
end.
