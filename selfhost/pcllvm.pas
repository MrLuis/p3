unit pcllvm;
{ The self-hosted compiler's back end in process (SELFHOST.md S11): the
  LLVM-C API of libLLVM, used as src/codegen/Backend.cpp uses LLVM. The
  IR text pcgen writes is parsed into a module, verified, optimized with
  the same pass pipeline and compiled by a target machine set up like the
  C++ pcc's (CPU "generic", PIC).

  The functions are declared without a library: the compiler is linked
  with libLLVM (LLVM-19 on Linux, LLVM on macOS; see selfhost/README.md).
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
function llvmVersion: string;
{ The target for triple, or nil and the error message. }
function llvmLookupTarget(const triple: string; var err: string): pointer;
{ A target machine for triple at an optimization level (0-3). }
function llvmTargetMachine(target: pointer; const triple: string; opt: longint): pointer;
{ The data layout string of a target machine. }
function llvmDataLayout(tm: pointer): string;
{ Parses IR text into a module (named name), verifies it and, at opt > 0,
  runs default<O[opt]>. Returns the module, or nil and the error.
  runtimeIR: the runtime's LLVM IR (libpascalrt.ll) to link into the
  module before it is optimized, so that the runtime's functions can be
  inlined, or ''; if that file cannot be used, rtProblem says why (else
  it is ''). }
function llvmParseAndOptimize(const ir: TBuf; const name: string; tm: pointer; opt: longint;
                              const runtimeIR: string; var rtProblem: string;
                              var err: TBuf): pointer;
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
procedure LLVMDisposeMessage(msg: pchar); cdecl; external name 'LLVMDisposeMessage';
function LLVMGetDefaultTargetTriple: pchar; cdecl; external name 'LLVMGetDefaultTargetTriple';
function LLVMNormalizeTargetTriple(t: pchar): pchar; cdecl;
  external name 'LLVMNormalizeTargetTriple';
function LLVMGetHostCPUName: pchar; cdecl; external name 'LLVMGetHostCPUName';
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

function llvmTargetMachine(target: pointer; const triple: string; opt: longint): pointer;
var
  b, cpu, features: string;
begin
  llvmTargetMachine := LLVMCreateTargetMachine(target, cstr(triple, b),
                                               cstr('generic', cpu), cstr('', features),
                                               opt, relocPIC, codeModelDefault);
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
function linkRuntimeIR(ctx, m: pointer; const path: string): string;
var
  buf, rt: pointer;
  msg: pchar;
  pb: string;
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
  if LLVMLinkModules2(m, rt) <> 0 then
    linkRuntimeIR := 'it cannot be linked';
end;

function llvmParseAndOptimize(const ir: TBuf; const name: string; tm: pointer; opt: longint;
                              const runtimeIR: string; var rtProblem: string;
                              var err: TBuf): pointer;
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
    if runtimeIR <> '' then
      rtProblem := linkRuntimeIR(ctx, m, runtimeIR);
    { llvm::PipelineTuningOptions' defaults, as the C++ pcc's PassBuilder
      has them }
    opts := LLVMCreatePassBuilderOptions;
    LLVMPassBuilderOptionsSetLoopInterleaving(opts, 1);
    LLVMPassBuilderOptionsSetLoopVectorization(opts, 1);
    LLVMPassBuilderOptionsSetSLPVectorization(opts, 0);
    LLVMPassBuilderOptionsSetLoopUnrolling(opts, 1);
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
