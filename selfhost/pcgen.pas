unit pcgen;
{ Code generation: a checked program and its units to LLVM IR; the port of
  src/codegen/CodeGen.cpp, whose header describes the lowering:

    integer types -> iN, real -> double, char -> i8, enum -> i32,
    boolean -> i1 in registers and i8 in memory, set -> i256,
    array -> [N x T], record -> a named struct (variants overlay a buffer)
    program variables -> internal globals @pas.<name>
    routines -> internal functions @pas.<outer>.<name> whose locals,
      parameters and result live in a frame struct; nested routines reach
      enclosing frames through static links (frame field 0)
    objects (objfpc) -> a pointer to a heap block [VMT pointer, fields];
      a class's VMT is a constant [2 + N x ptr]: parent VMT, class name,
      virtual methods; methods take Self after the static link

  The module must print exactly as the C++ p3's (SELFHOST.md S10), so
  this code creates the same values, with the same names, in the same
  order as CodeGen.cpp; pcir folds constants and prints like LLVM. Where
  the C++ code passes several instruction-creating expressions to one
  call, they are evaluated left to right (in locals), as clang, which
  builds the C++ p3, evaluates them.

  Not yet: debug information (-g). }

interface

uses pcsys, pcutil, pcdiag, pclex, pcast, pctypes, pcir;

{ Appends the module for prog and its units (dependencies first). }
procedure generateModule(prog: PDecl; const units: TPtrList; m: TLangMode;
                         rangeChecksOn, overflowChecksOn: boolean;
                         const sourceName, triple, dataLayout: string;
                         debugInfo, optimized, separateUnits, zeroInit,
                         keepUnused: boolean;
                         var outText: TBuf);
{ The module for unit u, compiled on its own (CodeGen.cpp's
  generateUnitModule); deps: the units it uses, dependency order. }
procedure generateUnitModule(u: PDecl; const deps: TPtrList; m: TLangMode;
                             rangeChecksOn, overflowChecksOn: boolean;
                             const sourceName, triple, dataLayout: string;
                             debugInfo, optimized, zeroInit, keepUnused: boolean;
                             var outText: TBuf);

{ -ffast-math: set the fast-math flags on the floating-point instructions
  of the modules generated from now on. }
procedure cgSetFastMath(b: boolean);
{ -mcpu / -mattr: the target CPU and features the functions' target
  attributes carry into the IR. }
procedure cgSetTarget(const cpu, features: string);
{ -Os (1) / -Oz (2): the definitions' optsize/minsize attributes. }
procedure cgSetSizeLevel(n: longint);

var
  { -ffast-math, for the modules generated from now on; cgSetFastMath sets it }
  optFastMath: boolean;
  optCPU, optFeatures: string;
  optSizeLevel: longint;

implementation

procedure cgSetFastMath(b: boolean);
begin
  optFastMath := b;
end;

procedure cgSetTarget(const cpu, features: string);
begin
  optCPU := cpu;
  optFeatures := features;
end;

procedure cgSetSizeLevel(n: longint);
begin
  optSizeLevel := n;
end;

type
  PLargeRange = ^TLargeRange;
  TLargeRange = record
    lo, hi: int64;
    target: PIRBlock;
  end;

  { A short-string temporary and its lifetime size (item 5). }
  PStringTemp = ^TStringTemp;
  TStringTemp = record
    addr: TV;
    size: int64;
  end;

  { Exceptions (objfpc): a try statement or running handler around the
    statement being emitted. exit, break and continue leaving it pop its
    PasExcFrame (emitJump):
      clTryBody  the protected statements of try..except: pop frame
      clHandler  an exception handler: pop frame (the handler's), free obj
      clFinally  the protected statements of try..finally: pop frame, then
                 run the finally part with state = 2 + k, after which it
                 continues at exits[k] (state 0: normal end, 1: exception) }
  TCleanupKind = (clTryBody, clHandler, clFinally);
  PCleanup = ^TCleanup;
  TCleanup = record
    kind: TCleanupKind;
    frame, obj, state: TV;
    finallyB: PIRBlock;
    exits: TPtrList; { of PIRBlock }
  end;

var
  mode: TLangMode;
  optRange, optOverflow: boolean;
  { local arrays, records and strings start zeroed (-fzero-init; the
    default at -O0); scalars, and what holds managed values or files,
    always do }
  optZeroInit: boolean;
  { dynamic arrays: the PasDynInfo of each array type (type id -> PTVBox),
    and the element functions to emit: for each type used, the element
    type twice and the fini and addref functions }
  dynInfos: TIntMap;
  typeInfos: TIntMap; { of PTVBox, by type id: typeInfoOf's }
  ignoredW: TV; { (a call's value, unused) }
  { the async routines whose task's functions are still to be emitted }
  asyncsToEmit: TPtrList;
  { how the next async call starts its task (set by spawn and Pool.Run,
    taken by the call): 0 here, 1 spawned in the scope, 2 on the pool }
  nextStart: longint;
  dynOpElems, dynOpFns: TPtrList;
  progDecl: PDecl;
  curRoutine: PDecl;         { nil while emitting main }
  curFn: PIRFunc;
  curFrame: TV;
  pinFn: PIRFunc;            { the function pinSlot belongs to; see pinInMemory }
  pinSlot: TV;
  { optimization2 item 3: the pin analysis's result variable and a scratch
    set it passes where no writes are collected }
  paCurResult: PDecl;
  paDummy: TPtrList;
  { optimization3 item 4: the current routine's local variables whose
    range is followed from store to store (cgRangeLocal), and the pin
    analysis leaving out the targets of plain x := e (paSkipPlain) }
  rangeLocals: TPtrList;
  paSkipPlain: boolean;
  { the value the last assignment to a scalar stored (see visitAssign) }
  assignStored: TV;
  assignStoredSet: boolean;
  { Are temporaries holding managed values pinned when they are made (see
    pinInMemory)? In a routine without try statements they are pinned
    later, if its body turns out to need protection. }
  pinTemps: boolean;
  resultDst: TV;             { optimization3 item 1: the destination of the call being
                               emitted, where its record result goes }
  hasResultDst: boolean;
  { A direct call whose ansistring result is moved into the assignment
    target: no temporary holds it (noHoldAnsiResult), and the call that
    returned it took that path (tookAnsiResult). }
  noHoldAnsiResult, tookAnsiResult: boolean;
  lowered: TIntMap;          { type id -> PIRType }
  ansiConsts: TIntMap;       { name id of the characters -> PTVBox of the .astr }
  { item 13: the checks' shared error messages, file strings and locations }
  errMsgs, fileStrs, locRecs: TIntMap;
  stringConsts: TIntMap;     { name id of the bytes -> PTVBox of the global }
  enumNameTables: TIntMap;   { type id -> PTVBox: an enumeration's names }
  routinesToEmit: TPtrList;  { of PDecl }
  thunksToEmit: TPtrList;    { of PDecl: cdecl routines with a cthunk }
  { continue and break targets and the innermost cleanup around the loop
    (a PCleanup, or nil), in triples }
  loopBlocks: TPtrList;
  { exceptions (objfpc): the try statements and running handlers around the
    statement being emitted, innermost last (of PCleanup); the frames of the
    try statements whose handlers are running (of PTVBox), innermost last: a
    bare raise raises the exception of the last; PasExcFrame's type }
  cleanups: TPtrList;
  handledFrames: TPtrList;
  excFrameT: PIRType;
  { PasOwnedExc (runtime/include/pascalrt.h): object, where, address }
  ownedExcT: PIRType;
  exitBlock: PIRBlock;
  mainJmpBuf: TV;
  hasMainJmpBuf: boolean;
  cgTriple: string; { the target (the C library's errno accessor) }
  routinePairT, jmpBufT: PIRType;
  separate: boolean;         { units compiled separately (S11) }
  { classes (objfpc): instance layouts (for offsets and sizes only) and
    VMTs by type id; the classes whose VMTs are defined in this module, in
    declaration order }
  classLayouts, vmts: TIntMap;
  { interfaces' iids (by type id, PTVBox) }
  iids: TIntMap;
  vmtsToDefine: TPtrList;
  { item 11: every class declared in the module, and those whose interface
    a separately compiled unit can see (by type id) }
  allClasses: TPtrList;
  exportedClasses: TIntMap;
  { reference counting (COM interfaces): a function's COM interface result
    is held in a temporary until the statement ends (liveTemps: the current
    statements', innermost last, of PTVBox); a routine releases the
    references in its value parameters, local variables and temporaries
    (routineTemps) when it ends, also when an exception leaves it. The
    classes whose cleanup functions are to be emitted (of PType) and the
    functions (of PIRFunc). }
  liveTemps, routineTemps: TPtrList;
  stringTemps: TPtrList; { of PStringTemp, the current statement's }
  { static variables whose initial values hold procedural values: set once
    the routines are declared (finishStaticInits) }
  deferredInits: TPtrList;
  cleanupClasses, cleanupFns: TPtrList;

{ --- Small helpers --- }

function imin(a, b: int64): int64;
begin
  if a < b then
    imin := a
  else
    imin := b;
end;

function i64c(v: int64): TV;
begin
  i64c := irConstInt(irI64, v);
end;

function i32c(v: int64): TV;
begin
  i32c := irConstInt(irI32, v);
end;

function i8c(v: int64): TV;
begin
  i8c := irConstInt(irI8, v);
end;

function isSignedOrdinal(t: PType): boolean;
begin
  isSignedOrdinal := isSignedIntegerLike(t);
end;

function isBoolBase(t: PType): boolean;
begin
  isBoolBase := getBase(t)^.kind = tyBoolean;
end;

function isCharBase(t: PType): boolean;
begin
  isCharBase := getBase(t)^.kind = tyChar;
end;

function intBits(t: PType): longint;
begin
  intBits := getBase(t)^.bits;
end;

{ Methods have the hidden parameter Self, except a record's class methods
  and operators (static). }
function hasSelf(r: PDecl): boolean;
begin
  hasSelf := ((r^.methodKind <> mtNone) and not r^.isStatic) or r^.isMethodLambda;
end;

{ Does r take the hidden static-link first parameter? Only a nested routine
  needs it; methods (and method lambdas) keep it for method pointers, as do
  async and iterator routines, whose argument record has a link slot
  (item 20). }
function needsLink(r: PDecl): boolean;
begin
  needsLink := (r^.parent <> nil) or hasSelf(r) or (r^.methodKind <> mtNone) or
               (r^.asyncType <> nil) or (r^.iterType <> nil);
end;

function canon(r: PDecl): PDecl;
begin
  if r^.canonical <> nil then
    canon := r^.canonical
  else
    canon := r;
end;

function nm(n: PName): string;
begin
  nm := nameStr(n);
end;

{ --- Calls --- }

function typeOfSpec(const s: string): PIRType;
begin
  if s = 'i8' then
    typeOfSpec := irI8
  else if s = 'i32' then
    typeOfSpec := irI32
  else if s = 'i64' then
    typeOfSpec := irI64
  else if s = 'double' then
    typeOfSpec := irDoubleT
  else if s = 'ptr' then
    typeOfSpec := irPtrT
  else
    typeOfSpec := irVoidT;
end;

{ Item 9: facts about the runtime functions that LLVM cannot infer across
  the library boundary (the same tables as addRuntimeAttrs in CodeGen.cpp). }
function runtimeIsPureQuery(const name: string): boolean;
begin
  runtimeIsPureQuery :=
    (name = '__pas_str_compare') or (name = '__pas_str_pos') or
    (name = '__pas_astr_compare') or (name = '__pas_astr_pos') or
    (name = '__pas_astr_length') or (name = '__pas_astr_bad_index') or
    (name = '__pas_dyn_length') or (name = '__pas_dyn_bad_index') or
    (name = '__pas_aoc_bad_index') or (name = '__pas_class_is') or
    (name = '__pas_class_inherits') or (name = '__pas_intf_lookup') or
    (name = 'memcmp');
end;

function runtimeReturnsFresh(const name: string): boolean;
begin
  runtimeReturnsFresh :=
    (name = '__pas_new') or (name = '__pas_astr_from_chars') or
    (name = '__pas_astr_from_pstr') or (name = '__pas_astr_from_char') or
    (name = '__pas_astr_from_cstr') or (name = '__pas_astr_join') or
    (name = '__pas_dyn_new') or (name = '__pas_dyn_copy') or
    (name = '__pas_format') or (name = '__pas_tmpl_add');
end;

function runtimeReadsParam(const name: string; i: longint): boolean;
begin
  runtimeReadsParam := false;
  if (name = '__pas_str_compare') or (name = '__pas_str_pos') or
     (name = '__pas_astr_compare') or (name = '__pas_astr_pos') or
     (name = '__pas_class_is') or (name = '__pas_class_inherits') or
     (name = '__pas_intf_lookup') or (name = 'memcmp') then
    runtimeReadsParam := i <= 1
  else if (name = '__pas_astr_length') or (name = '__pas_astr_bad_index') or
          (name = '__pas_dyn_length') or (name = '__pas_dyn_bad_index') or
          (name = '__pas_aoc_bad_index') or (name = '__pas_astr_from_chars') or
          (name = '__pas_astr_from_pstr') or (name = '__pas_astr_from_cstr') or
          (name = '__pas_astr_pchar') or (name = '__pas_dyn_copy') or
          (name = '__pas_dyn_new') or (name = '__pas_astr_copy') then
    runtimeReadsParam := i = 0
  else if name = '__pas_tmpl_add' then
    runtimeReadsParam := i <= 1
  else if name = '__pas_write_enum' then
    runtimeReadsParam := i = 1 { the names table, not the file it writes to }
  else if (name = '__pas_str_assign') or (name = '__pas_str_copy') then
    runtimeReadsParam := (i = 1) or (i = 2)
  else if name = '__pas_str_concat' then
    runtimeReadsParam := (i = 1) or (i = 2)
  else if name = '__pas_str_append' then
    runtimeReadsParam := i = 2
  else if name = '__pas_str_insert' then
    runtimeReadsParam := i = 0
  else if (name = '__pas_write_str') or (name = '__pas_write_pstr') or
          (name = '__pas_write_cstr') or (name = '__pas_write_astr') then
    runtimeReadsParam := i = 1
  else if name = '__pas_astr_to_pstr' then
    runtimeReadsParam := i = 2
  else if name = '__pas_astr_join' then
    runtimeReadsParam := (i = 0) or (i = 1) or (i = 3)
  else if name = '__pas_format' then
    runtimeReadsParam := (i = 0) or (i = 1)
  else if name = '__pas_format_pstr' then
    runtimeReadsParam := (i = 1) or (i = 2)
  else if (name = '__pas_intf_as') or (name = '__pas_class_as') then
    runtimeReadsParam := i <= 2;
end;

{ A runtime destination that it only writes, and that cannot alias a
  source of the call (item 4). }
function runtimeWritesParam(const name: string; i: longint): boolean;
begin
  runtimeWritesParam := ((name = '__pas_read_elem') and (i = 1)) or
                        ((name = '__pas_read_str') and (i = 1)) or
                        ((name = '__pas_paramstr') and (i = 0));
end;

function runtimeSlotParam(const name: string; i: longint): boolean;
begin
  runtimeSlotParam := ((name = '__pas_astr_setlength') or
                       (name = '__pas_astr_clear') or
                       (name = '__pas_astr_assign') or
                       (name = '__pas_astr_take') or
                       (name = '__pas_astr_append') or
                       (name = '__pas_dyn_setlength') or
                       (name = '__pas_dyn_clear') or
                       (name = '__pas_dyn_assign')) and (i = 0);
end;

{ The runtime function Name never frees its pointer arguments: `nofree`,
  from the functions' own code (llvm-23.md LB-3). The functions that may
  free (the releases, the clears, the appends' slot owners, the unique's
  release, the reallocs and free) stay out. }
function runtimeNeverFree(const name: string): boolean;
begin
  runtimeNeverFree := runtimeIsPureQuery(name) or
                      (name = '__pas_astr_from_chars') or
                      (name = '__pas_astr_from_pstr') or
                      (name = '__pas_astr_from_cstr') or
                      (name = '__pas_astr_from_char') or
                      (name = '__pas_astr_join') or
                      (name = '__pas_astr_copy') or
                      (name = '__pas_astr_pchar') or
                      (name = '__pas_astr_concat') or
                      (name = '__pas_astr_addref') or
                      (name = '__pas_astr_pos') or
                      (name = '__pas_dyn_copy') or
                      (name = '__pas_dyn_length') or
                      (name = '__pas_dyn_new') or
                      (name = '__pas_write_str') or
                      (name = '__pas_write_pstr') or
                      (name = '__pas_write_cstr') or
                      (name = '__pas_write_astr') or
                      (name = '__pas_write_char') or
                      (name = '__pas_write_int') or
                      (name = '__pas_write_bool') or
                      (name = '__pas_write_real') or
                      (name = '__pas_write_enum') or
                      (name = '__pas_write_elem') or
                      (name = '__pas_read_elem') or
                      (name = '__pas_read_int') or
                      (name = '__pas_read_int64') or
                      (name = '__pas_read_char') or
                      (name = '__pas_read_real') or
                      (name = '__pas_read_str') or
                      (name = '__pas_format') or
                      (name = '__pas_format_pstr') or
                      (name = '__pas_tmpl_add') or
                      (name = '__pas_paramstr');
end;

{ The function attributes (the base is 'nounwind' for plain runtime
  functions). }
function runtimeAttrs(const name, base: string): string;
var
  r: string;
begin
  if base <> 'nounwind' then
  begin
    runtimeAttrs := base;
    exit;
  end;
  if runtimeIsPureQuery(name) then
    r := 'nounwind willreturn memory(argmem: read)'
  else if name = '__pas_astr_pchar' then
    r := 'nounwind willreturn memory(none)'
  else if name = '__pas_new' then
    r := 'nounwind allockind("alloc,zeroed") allocsize(0)'
  else if name = '__pas_dyn_new' then
    r := 'nounwind allockind("alloc,zeroed")'
  else if name = '__pas_realloc' then
    r := 'nounwind allockind("realloc") allocsize(1)'
  else if name = '__pas_abstract_error' then
    r := 'cold noreturn nounwind'
  else
    r := base;
  if runtimeNeverFree(name) then
    r := 'nofree ' + r;
  runtimeAttrs := r;
end;

{ The runtime function Name returns 0 or 1: its result's `range` return
  attribute, as the runtime's code keeps (llvm-23.md LB-2). }
function runtimeReturnsBool(const name: string): boolean;
begin
  runtimeReturnsBool := (name = '__pas_astr_bad_index') or
                        (name = '__pas_dyn_bad_index') or
                        (name = '__pas_aoc_bad_index') or
                        (name = '__pas_token_cancelled');
end;

function runtimeRetAttr(const name: string): string;
begin
  if runtimeReturnsFresh(name) then
    runtimeRetAttr := 'noalias'
  else if runtimeReturnsBool(name) then
    runtimeRetAttr := 'range(i32 0, 2)'
  else
    runtimeRetAttr := '';
end;

{ A declaration with parameter types given as 'i32,ptr'. }
procedure declareRT(const name, ret, params, attrs: string);
var
  list: TPtrList;
  part: string;
  i: longint;
  ignored: PIRFunc;
begin
  listInit(list);
  part := '';
  for i := 1 to length(params) + 1 do
    if (i > length(params)) or (params[i] = ',') then
    begin
      if part <> '' then
        listAdd(list, typeOfSpec(part));
      part := '';
    end
    else
      part := part + params[i];
  ignored := irDeclareFunction(name, typeOfSpec(ret), list,
                               runtimeAttrs(name, attrs), '');
  irSetRetAttrs(ignored, runtimeRetAttr(name));
  for i := 0 to list.count - 1 do
    if PIRType(listGet(list, i))^.kind = ikPtr then
    begin
      if runtimeReadsParam(name, i) then
        irSetArgAttrs(ignored, i, 'readonly captures(none)')
      else if runtimeWritesParam(name, i) then
        irSetArgAttrs(ignored, i, 'writeonly captures(none)');
      if runtimeSlotParam(name, i) then
        irSetArgAttrs(ignored, i, 'dereferenceable_or_null(8)');
    end;
  listFree(list);
  { Can a call of it raise an exception? Those that raise one, and those
    that may run a destructor (which is Pascal code): releasing an object
    or an interface reference. }
  if (name <> '__pas_raise') and (name <> '__pas_raise_at') and
     (name <> '__pas_raise_divzero') and (name <> '__pas_raise_overflow') and
     (name <> '__pas_reraise') and (name <> '__pas_reraise_free') and
     (name <> '__pas_free_object') and (name <> '__pas_free_instance') and
     (name <> '__pas_intf_assign') and (name <> '__pas_intf_clear') and
     (name <> '__pas_frame_release') and (name <> '__pas_closure_assign') and
     (name <> '__pas_task_await') and (name <> '__pas_scope_leave') and
     (name <> '__pas_task_assign') and (name <> '__pas_task_clear') and
     (name <> '__pas_gen_next') and (name <> '__pas_gen_yield') and
     (name <> '__pas_gen_assign') and (name <> '__pas_gen_clear') and
     (name <> '__pas_closure_clear') and
     (name <> '__pas_dyn_clear') and (name <> '__pas_dyn_assign') and
     (name <> '__pas_dyn_setlength') and
     (name <> '__pas_format_pstr') and
     (name <> '__pas_owned_set') and (name <> '__pas_owned_done') then
    irSetNoRaise(ignored);
end;

{ The runtime and C library functions, in the order of the C++ Runtime. }
procedure declareRuntime;
const
  nu = 'nounwind';
begin
  declareRT('__pas_init', 'void', 'i32,ptr', nu);
  declareRT('__pas_fini', 'void', '', nu);
  declareRT('__pas_write_begin', 'void', 'ptr', nu);
  declareRT('__pas_write_end', 'void', 'ptr', nu);
  declareRT('__pas_write_discard', 'void', 'ptr', nu);
  declareRT('__pas_write_str', 'void', 'ptr,ptr,i64,i32', nu);
  declareRT('__pas_write_char', 'void', 'ptr,i8,i32', nu);
  declareRT('__pas_write_int', 'void', 'ptr,i64,i32', nu);
  declareRT('__pas_write_bool', 'void', 'ptr,i32,i32', nu);
  declareRT('__pas_write_real', 'void', 'ptr,double,i32,i32', nu);
  declareRT('__pas_writeln', 'void', 'ptr', nu);
  declareRT('__pas_read_int', 'i32', 'ptr', nu);
  declareRT('__pas_read_int64', 'i64', 'ptr', nu);
  declareRT('__pas_read_char', 'i8', 'ptr', nu);
  declareRT('__pas_read_real', 'double', 'ptr', nu);
  declareRT('__pas_readln', 'void', 'ptr', nu);
  declareRT('__pas_file_new', 'ptr', 'i64,i32,ptr', nu);
  declareRT('__pas_file_free', 'void', 'ptr', nu);
  declareRT('__pas_program_param', 'ptr', 'i32,ptr', nu);
  declareRT('__pas_reset', 'void', 'ptr', nu);
  declareRT('__pas_rewrite', 'void', 'ptr', nu);
  declareRT('__pas_get', 'void', 'ptr', nu);
  declareRT('__pas_put', 'void', 'ptr', nu);
  declareRT('__pas_page', 'void', 'ptr', nu);
  declareRT('__pas_eof', 'i32', 'ptr', nu);
  declareRT('__pas_eoln', 'i32', 'ptr', nu);
  declareRT('__pas_buffer', 'ptr', 'ptr', nu);
  declareRT('__pas_read_elem', 'void', 'ptr,ptr', nu);
  declareRT('__pas_write_elem', 'void', 'ptr,ptr', nu);
  declareRT('__pas_runtime_error', 'void', 'ptr', 'cold noreturn nounwind');
  declareRT('__pas_runtime_error_at', 'void', 'ptr,ptr', 'cold noreturn nounwind');
  declareRT('memcmp', 'i32', 'ptr,ptr,i64', nu);
  declareRT('__pas_new', 'ptr', 'i64', nu);
  declareRT('__pas_dispose', 'void', 'ptr', nu);
  declareRT('atan', 'double', 'double', nu);
  declareRT('free', 'void', 'ptr', nu);
  declareRT('__pas_realloc', 'ptr', 'ptr,i64', nu);
  declareRT('__pas_halt', 'void', 'i32', 'noreturn nounwind');
  declareRT('__pas_paramcount', 'i32', '', nu);
  declareRT('__pas_paramstr', 'void', 'ptr,i32', nu);
  declareRT('__pas_str_assign', 'void', 'ptr,i32,ptr', nu);
  declareRT('__pas_str_concat', 'void', 'ptr,ptr,ptr', nu);
  declareRT('__pas_str_compare', 'i32', 'ptr,ptr', nu);
  declareRT('__pas_str_copy', 'void', 'ptr,ptr,i32,i32', nu);
  declareRT('__pas_str_pos', 'i32', 'ptr,ptr', nu);
  declareRT('__pas_str_delete', 'void', 'ptr,i32,i32', nu);
  declareRT('__pas_str_insert', 'void', 'ptr,ptr,i32,i32', nu);
  declareRT('__pas_write_pstr', 'void', 'ptr,ptr,i32', nu);
  declareRT('__pas_read_str', 'void', 'ptr,ptr,i32', nu);
  declareRT('_setjmp', 'i32', 'ptr', 'nounwind returns_twice');
  declareRT('_longjmp', 'void', 'ptr,i32', 'noreturn nounwind');
end;

const
  speculatable = 'nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)';
  speculatablePlain = 'nocallback nofree nosync nounwind speculatable willreturn memory(none)';

{ An intrinsic, declared on first use (Intrinsic::getDeclaration). }
function intrinsic(const name: string; ret: PIRType; const p1, p2: PIRType;
                   const attrs, paramText: string): PIRFunc;
var
  f: PIRFunc;
  list: TPtrList;
begin
  f := irFindFunction(name);
  if f <> nil then
    exit(f);
  listInit(list);
  if p1 <> nil then
    listAdd(list, p1);
  if p2 <> nil then
    listAdd(list, p2);
  f := irDeclareFunction(name, ret, list, attrs, paramText);
  listFree(list);
  intrinsic := f;
end;

function rtFn(const name: string): PIRFunc;
begin
  rtFn := irFindFunction(name);
end;

function call0(f: PIRFunc; const name: string): TV;
var
  a: TBuf;
begin
  bufInit(a);
  call0 := irCall(f, a, name);
  bufFree(a);
end;

function call1(f: PIRFunc; const x: TV; const name: string): TV;
var
  a: TBuf;
begin
  bufInit(a);
  irArgsAdd(a, x);
  call1 := irCall(f, a, name);
  bufFree(a);
end;

function call2(f: PIRFunc; const x, y: TV; const name: string): TV;
var
  a: TBuf;
begin
  bufInit(a);
  irArgsAdd(a, x);
  irArgsAdd(a, y);
  call2 := irCall(f, a, name);
  bufFree(a);
end;

function call3(f: PIRFunc; const x, y, z: TV; const name: string): TV;
var
  a: TBuf;
begin
  bufInit(a);
  irArgsAdd(a, x);
  irArgsAdd(a, y);
  irArgsAdd(a, z);
  call3 := irCall(f, a, name);
  bufFree(a);
end;

function call4(f: PIRFunc; const x, y, z, w: TV; const name: string): TV;
var
  a: TBuf;
begin
  bufInit(a);
  irArgsAdd(a, x);
  irArgsAdd(a, y);
  irArgsAdd(a, z);
  irArgsAdd(a, w);
  call4 := irCall(f, a, name);
  bufFree(a);
end;

{ void runtime calls }
procedure rtv1(const fn: string; const x: TV);
var
  ignored: TV;
begin
  ignored := call1(rtFn(fn), x, '');
end;

procedure rtv2(const fn: string; const x, y: TV);
var
  ignored: TV;
begin
  ignored := call2(rtFn(fn), x, y, '');
end;

procedure rtv3(const fn: string; const x, y, z: TV);
var
  ignored: TV;
begin
  ignored := call3(rtFn(fn), x, y, z, '');
end;

procedure rtv4(const fn: string; const x, y, z, w: TV);
var
  ignored: TV;
begin
  ignored := call4(rtFn(fn), x, y, z, w, '');
end;

{ IRBuilder::CreateMemCpy / CreateMemMove / CreateMemSet }
procedure memTransfer2(const fname, paramText: string; const dst, src, n: TV;
                       dstAlign, srcAlign: int64);
var
  f: PIRFunc;
  a: TBuf;
  ignored: TV;
begin
  f := intrinsic(fname, irVoidT, nil, nil,
                 'nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)',
                 paramText);
  if f^.params.count = 0 then
  begin
    listAdd(f^.params, irPtrT);
    listAdd(f^.params, irPtrT);
    listAdd(f^.params, irI64);
    listAdd(f^.params, irI1);
  end;
  bufInit(a);
  irArgsAddAttr(a, dst, 'align ' + intToStr(dstAlign));
  irArgsAddAttr(a, src, 'align ' + intToStr(srcAlign));
  irArgsAdd(a, n);
  irArgsAdd(a, irConstBool(false));
  ignored := irCall(f, a, '');
  bufFree(a);
end;

procedure memTransfer(const fname, paramText: string; const dst, src, n: TV;
                      align: int64);
begin
  memTransfer2(fname, paramText, dst, src, n, align, align);
end;

procedure emitMemCpyAligned(const dst, src, n: TV; align: int64);
begin
  memTransfer('llvm.memcpy.p0.p0.i64',
              'ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg',
              dst, src, n, align);
end;

procedure emitMemCpy(const dst, src, n: TV);
begin
  emitMemCpyAligned(dst, src, n, 1);
end;

procedure emitMemMove(const dst, src, n: TV);
begin
  memTransfer('llvm.memmove.p0.p0.i64',
              'ptr writeonly captures(none), ptr readonly captures(none), i64, i1 immarg',
              dst, src, n, 1);
end;

procedure emitMemSet(const dst, v, n: TV; align: int64);
var
  f: PIRFunc;
  a: TBuf;
  ignored: TV;
begin
  f := intrinsic('llvm.memset.p0.i64', irVoidT, nil, nil,
                 'nocallback nofree nosync nounwind willreturn memory(argmem: write)',
                 'ptr writeonly captures(none), i8, i64, i1 immarg');
  if f^.params.count = 0 then
  begin
    listAdd(f^.params, irPtrT);
    listAdd(f^.params, irI8);
    listAdd(f^.params, irI64);
    listAdd(f^.params, irI1);
  end;
  bufInit(a);
  irArgsAddAttr(a, dst, 'align ' + intToStr(align));
  irArgsAdd(a, v);
  irArgsAdd(a, n);
  irArgsAdd(a, irConstBool(false));
  ignored := irCall(f, a, '');
  bufFree(a);
end;

{ a double -> double intrinsic (CreateUnaryIntrinsic) }
function unaryIntrinsic(const which: string; const x: TV; const name: string): TV;
begin
  unaryIntrinsic := call1(intrinsic('llvm.' + which + '.f64', irDoubleT, irDoubleT, nil,
                                    speculatable, ''), x, name);
end;

{ --- Types --- }

function memType(t: PType): PIRType; forward;

procedure shiftFieldOffsets(fl: PFieldList; delta: int64);
var
  i: longint;
  f: PDecl;
  vp: PVariantPart;
begin
  for i := 0 to fl^.fixed.count - 1 do
  begin
    f := PDecl(listGet(fl^.fixed, i));
    f^.cgOffset := f^.cgOffset + delta;
  end;
  vp := fl^.variant;
  if vp <> nil then
  begin
    if vp^.tag <> nil then
    begin
      f := vp^.tag;
      f^.cgOffset := f^.cgOffset + delta;
    end;
    for i := 0 to vp^.variants.count - 1 do
      shiftFieldOffsets(PVariant(listGet(vp^.variants, i))^.fields, delta);
  end;
end;

{ The struct for a record field list; records each field's byte offset
  from the start of the record. Variants are laid out as their own
  structs, all overlaying one buffer. }
function layoutFieldList(fl: PFieldList; const name: string; baseOffset: int64): PIRType;
var
  elems, fields: TPtrList;
  vp: PVariantPart;
  i: longint;
  maxSize, maxAlign, bufferOffset: int64;
  vt, st: PIRType;
  f: PDecl;
  tagType: PType;
begin
  listInit(elems);
  listInit(fields);
  for i := 0 to fl^.fixed.count - 1 do
  begin
    f := PDecl(listGet(fl^.fixed, i));
    listAdd(elems, memType(f^.fieldType));
    listAdd(fields, f);
  end;
  vp := fl^.variant;
  if (vp <> nil) and (vp^.tag <> nil) then
  begin
    tagType := vp^.tag^.fieldType;
    if tagType = nil then
      tagType := vp^.tagType;
    listAdd(elems, memType(tagType));
    listAdd(fields, vp^.tag);
  end;
  maxSize := 0;
  maxAlign := 1;
  if vp <> nil then
  begin
    for i := 0 to vp^.variants.count - 1 do
    begin
      vt := layoutFieldList(PVariant(listGet(vp^.variants, i))^.fields,
                            name + '.variant', 0);
      if vt^.size > maxSize then
        maxSize := vt^.size;
      if vt^.align > maxAlign then
        maxAlign := vt^.align;
    end;
    listAdd(elems, irArray((maxSize + maxAlign - 1) div maxAlign,
                           irInt(longint(maxAlign * 8))));
  end;
  st := irStructNamed(name, elems);
  for i := 0 to fields.count - 1 do
  begin
    f := PDecl(listGet(fields, i));
    f^.cgOffset := baseOffset + irFieldOffset(st, i);
  end;
  if vp <> nil then
  begin
    bufferOffset := baseOffset + irFieldOffset(st, fields.count);
    for i := 0 to vp^.variants.count - 1 do
      shiftFieldOffsets(PVariant(listGet(vp^.variants, i))^.fields, bufferOffset);
  end;
  listFree(elems);
  listFree(fields);
  layoutFieldList := st;
end;

{ The alignment of a value of type t (a field's natural alignment). }
function alignOf(t: PType): int64;
begin
  alignOf := memType(t)^.align;
end;

{ Lays out the fields of fl from byte en as Free Pascal does under
  $packrecords n: each field at a multiple of the smaller of its alignment
  and n; a variant part (each variant from the same offset) at a multiple
  of the smaller of n and 16. en becomes the end of the last field; align
  the largest alignment used. }
procedure layoutAligned(fl: PFieldList; n: int64; var en, align: int64);
var
  vp: PVariantPart;
  i: longint;
  va, start, mx, e: int64;
  tagType: PType;

  procedure place(f: PDecl; t: PType);
  var
    a: int64;
  begin
    a := alignOf(t);
    if n < a then
      a := n;
    en := (en + a - 1) div a * a;
    f^.cgOffset := en;
    en := en + memType(t)^.size;
    if a > align then
      align := a;
  end;

begin
  for i := 0 to fl^.fixed.count - 1 do
    place(PDecl(listGet(fl^.fixed, i)), PDecl(listGet(fl^.fixed, i))^.fieldType);
  vp := fl^.variant;
  if vp = nil then
    exit;
  if vp^.tag <> nil then
  begin
    tagType := vp^.tag^.fieldType;
    if tagType = nil then
      tagType := vp^.tagType;
    place(vp^.tag, tagType);
  end;
  va := n;
  if va > 16 then
    va := 16;
  start := (en + va - 1) div va * va;
  mx := start;
  for i := 0 to vp^.variants.count - 1 do
  begin
    e := start;
    layoutAligned(PVariant(listGet(vp^.variants, i))^.fields, n, e, align);
    if e > mx then
      mx := e;
  end;
  en := mx;
end;

{ Lays out the fields of a bitpacked record from bit start, as Free
  Pascal does: ordinals take their bitPackedWidth bits, one after another;
  other fields and the variant part start at a byte. Returns the bit after
  the last field. }
function layoutBitPacked(fl: PFieldList; start: int64): int64;
var
  bit, varStart, e, en: int64;
  i: longint;
  vp: PVariantPart;

  procedure place(f: PDecl);
  begin
    if isOrdinal(f^.fieldType) then
    begin
      f^.cgBitOffset := bit;
      bit := bit + bitPackedWidth(f^.fieldType);
      exit;
    end;
    bit := (bit + 7) div 8 * 8;
    f^.cgOffset := bit div 8;
    bit := bit + memType(f^.fieldType)^.size * 8;
  end;

begin
  bit := start;
  for i := 0 to fl^.fixed.count - 1 do
    place(PDecl(listGet(fl^.fixed, i)));
  vp := fl^.variant;
  if vp <> nil then
  begin
    if vp^.tag <> nil then
      place(vp^.tag);
    varStart := (bit + 7) div 8 * 8;
    en := varStart;
    for i := 0 to vp^.variants.count - 1 do
    begin
      e := (layoutBitPacked(PVariant(listGet(vp^.variants, i))^.fields, varStart) + 7)
           div 8 * 8;
      if e > en then
        en := e;
    end;
    bit := en;
  end;
  layoutBitPacked := bit;
end;

function lowerType(t: PType): PIRType;
var
  bd: TOrdinalBounds;
  name: string;
  bits, en, al, size: int64;
  elems: TPtrList;
begin
  case t^.kind of
    tyInteger: lowerType := irInt(t^.bits);
    tyReal: lowerType := irDoubleT;
    tyBoolean, tyChar: lowerType := irI8;
    tyWideChar: lowerType := irI16;
    tyEnum: lowerType := irI32;
    tySubrange: lowerType := memType(t^.host);
    tyStringLit: lowerType := irArray(t^.strLength, irI8);
    tyVector: { vector[N] of T (llvm-23.md LB-6): an LLVM vector of T }
      lowerType := irVector(t^.vecCount, memType(t^.vecElement));
    tyArray:
      begin
        bd := getOrdinalBounds(t^.indexType);
        if hasBitElements(t) then { the elements' bits, in whole bytes }
          lowerType := irArray(((bd.hi - bd.lo + 1) * bitPackedWidth(t^.elementType) + 7)
                               div 8, irI8)
        else
          lowerType := irArray(bd.hi - bd.lo + 1, memType(t^.elementType));
      end;
    tyRecord:
      begin
        if t^.declName = nil then
          name := 'record'
        else
          name := 'record.' + nm(t^.declName);
        if t^.isBitPackedRecord then
        begin
          bits := layoutBitPacked(t^.fieldList, 0);
          listInit(elems);
          listAdd(elems, irArray((bits + 7) div 8, irI8));
          lowerType := irStructNamed(name, elems);
          listFree(elems);
        end
        else if fieldAlignment(t) <> 0 then
        begin
          { Free Pascal's layout under $packrecords N (packed: 1), as the
            record's bytes in units of its alignment (which LLVM then gives
            it as a field or an element) }
          en := 0;
          al := 1;
          layoutAligned(t^.fieldList, fieldAlignment(t), en, al);
          size := (en + al - 1) div al * al;
          listInit(elems);
          listAdd(elems, irArray(size div al, irInt(longint(al * 8))));
          lowerType := irStructNamed(name, elems);
          listFree(elems);
        end
        else
          lowerType := layoutFieldList(t^.fieldList, name, 0);
      end;
    tyPointer, tyNil, tyFile: lowerType := irPtrT;
    tySet: lowerType := irInt(setStorageBits(t));
    tyShortString: lowerType := irArray(t^.maxLength + 1, irI8);
    tyAnsiString, tyArrayOfConst, tyOpenArray, tyDynArray: lowerType := irPtrT;
    tyRoutine: lowerType := routinePairT;
    tyClass: lowerType := irPtrT; { a reference to the object }
    tyClassRef: lowerType := irPtrT; { a reference to the class's VMT }
    tyTask: lowerType := irPtrT;  { a counted reference to the task }
    tyToken: lowerType := irPtrT; { a counted reference to the token }
    tyIterator: lowerType := irPtrT; { a counted reference to the generator }
    tyInterface: lowerType := routinePairT; { (object, method table) }
  else
    lowerType := irI8;
  end;
end;

{ The in-memory representation of a Pascal type. }
function memType(t: PType): PIRType;
var
  l: PIRType;
begin
  l := PIRType(mapGet(lowered, t^.id));
  if l <> nil then
    exit(l);
  l := lowerType(t);
  mapPut(lowered, t^.id, l);
  memType := l;
end;

{ What pointer arithmetic on a pt counts in: its pointee (bytes, for an
  untyped pointer). }
function pointeeMemType(pt: PType): PIRType;
begin
  if pt^.untyped or (pt^.pointee = nil) then
    pointeeMemType := irI8
  else
    pointeeMemType := memType(pt^.pointee);
end;

{ The register representation: booleans are i1. }
function regType(t: PType): PIRType;
begin
  if isBoolBase(t) then
    regType := irI1
  else
    regType := memType(t);
end;

function sizeOf_(t: PType): int64;
begin
  sizeOf_ := memType(t)^.size;
end;

{ The layout of an object of class c: VMT pointer, then all fields;
  records each field's offset (a prefix of fields has the same offsets in
  every descendant). }
function classLayout(c: PType): PIRType;
var
  st: PIRType;
  elems: TPtrList;
  i: longint;
  f: PDecl;
begin
  st := PIRType(mapGet(classLayouts, c^.id));
  if st <> nil then
    exit(st);
  listInit(elems);
  listAdd(elems, irPtrT);
  for i := 0 to c^.classAllFields.count - 1 do
  begin
    f := PDecl(listGet(c^.classAllFields, i));
    listAdd(elems, memType(f^.fieldType));
  end;
  st := irStructNamed('class.' + nm(c^.declName), elems);
  listFree(elems);
  for i := 0 to c^.classAllFields.count - 1 do
  begin
    f := PDecl(listGet(c^.classAllFields, i));
    f^.cgOffset := irFieldOffset(st, i + 1);
  end;
  mapPut(classLayouts, c^.id, st);
  classLayout := st;
end;

function instanceSize(c: PType): int64;
begin
  instanceSize := classLayout(c)^.size;
end;

{ --- Scalars in memory --- }

{ The addresses (in the current function) of fields and elements that a
  field alignment ($packrecords, packed) leaves less aligned than their
  types: the alignment they have, by the address's operand text (entries
  of other functions are stale: their generation differs). Loads, stores
  and copies through them say so. }
type
  PAddrAlign = ^TAddrAlign;
  TAddrAlign = record
    gen: longint;
    align: int64;
  end;

var
  addrAligns: TIntMap;
  addrAlignGen: longint;

{ Forgets the addresses of the previous function. }
procedure resetAddrAligns;
begin
  addrAlignGen := addrAlignGen + 1;
end;

function knownAddrAlign(const p: TV; var align: int64): boolean;
var
  e: PAddrAlign;
begin
  knownAddrAlign := false;
  e := PAddrAlign(mapGet(addrAligns, intern(p.v)^.id));
  if (e = nil) or (e^.gen <> addrAlignGen) then
    exit;
  align := e^.align;
  knownAddrAlign := true;
end;

procedure setAddrAlign(const p: TV; align: int64);
var
  e: PAddrAlign;
begin
  e := PAddrAlign(mapGet(addrAligns, intern(p.v)^.id));
  if e = nil then
  begin
    e := PAddrAlign(arenaAlloc(sizeof(TAddrAlign)));
    mapPut(addrAligns, intern(p.v)^.id, e);
  end;
  e^.gen := addrAlignGen;
  e^.align := align;
end;

{ The alignment of the value of type t at address p. }
function addrAlign(const p: TV; t: PType): int64;
var
  a: int64;
begin
  if knownAddrAlign(p, a) then
    exit(a);
  addrAlign := alignOf(t);
end;

{ Notes that the component of type t at addr, offset bytes into a value
  aligned to baseAlign, may be less aligned than t. }
procedure noteComponentAlign(const addr: TV; baseAlign, offset: int64; t: PType);
var
  a: int64;
begin
  a := baseAlign;
  if offset <> 0 then
  begin
    {$Q-}{$R-}
    a := offset and (not offset + 1);
    {$Q+}{$R+}
    if baseAlign < a then
      a := baseAlign;
  end;
  if a < alignOf(t) then
    setAddrAlign(addr, a);
end;

function loadScalar(const p: TV; t: PType; const name: string): TV;
var
  raw: TV;
  a: int64;
begin
  if knownAddrAlign(p, a) then
    raw := irLoadAlign(memType(t), p, longint(a), name)
  else
    raw := irLoad(memType(t), p, name);
  if isBoolBase(t) then
    loadScalar := irCast('trunc', raw, irI1, '')
  else
    loadScalar := raw;
end;

procedure storeScalar(const p, v: TV; t: PType);
var
  x: TV;
  a: int64;
begin
  x := v;
  if isBoolBase(t) then
    x := irCast('zext', v, irI8, '');
  if knownAddrAlign(p, a) then
    irStoreAlign(x, p, longint(a))
  else
    irStore(x, p);
end;

{ Copies the array, record or string t: both are aligned as t is. }
procedure copyAggregate(const dst, src: TV; t: PType);
begin
  memTransfer2('llvm.memcpy.p0.p0.i64',
               'ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg',
               dst, src, i64c(sizeOf_(t)), addrAlign(dst, t), addrAlign(src, t));
end;

{ The address of a private constant holding the bytes (no NUL), shared by
  equal byte strings. }
function getStringConstant(p: PCharArray; start, count: int64): TV;
var
  key: PName;
  g: PTVBox;
begin
  key := internChars(p, start, count);
  g := PTVBox(mapGet(stringConsts, key^.id));
  if g = nil then
  begin
    g := irBox(irGlobal('.str', 'private', irArray(count, irI8),
                        irConstBytes(p, start, count), true, true, false, 0));
    mapPut(stringConsts, key^.id, g);
  end;
  getStringConstant := g^;
end;

function getNameConstant(n: PName): TV;
begin
  getNameConstant := getStringConstant(n^.chars, 0, n^.len);
end;

{ IRBuilder::CreateGlobalString: a NUL-terminated private constant. }
function globalString(p: PCharArray; start, count: int64; const name: string): TV;
var
  b: TBuf;
begin
  bufInit(b);
  bufAddChars(b, p, start, count);
  bufAddChar(b, #0);
  globalString := irGlobal(name, 'private', irArray(count + 1, irI8),
                           irConstBytes(b.data, 0, b.len), true, true, false, 1);
  bufFree(b);
end;

{ --- Runtime checks --- }

procedure bufAddWhere(var b: TBuf; loc: TLoc);
var
  fid, line, col: longint;
begin
  if loc = noLoc then
    exit;
  smPresumed(loc, fid, line, col);
  bufAdd(b, ' at ');
  bufAdd(b, smFileName(fid));
  bufAdd(b, ':' + intToStr(line) + ':' + intToStr(col));
end;

function bufToStr(const b: TBuf): string;
var
  i: longint;
  s: string;
begin
  s := '';
  for i := 0 to b.len - 1 do
    s := s + bufCharAt(b, i);
  bufToStr := s;
end;

{ The .rterr global of a message (shared by the checks that use it). }
function errorMessageB(const b: TBuf): TV;
var
  key: PName;
  m: PTVBox;
begin
  key := intern(bufToStr(b));
  m := PTVBox(mapGet(errMsgs, key^.id));
  if m = nil then
  begin
    m := irBox(globalString(b.data, 0, b.len, '.rterr'));
    mapPut(errMsgs, key^.id, m);
  end;
  errorMessageB := m^;
end;

{ The .file global of a source file name (one per file, item 13). }
function fileString(const name: string): TV;
var
  key: PName;
  p: PTVBox;
begin
  key := intern(name);
  p := PTVBox(mapGet(fileStrs, key^.id));
  if p = nil then
  begin
    if length(name) = 0 then
      p := irBox(irNull)
    else
      p := irBox(globalString(PCharArray(@name[1]), 0, length(name), '.file'));
    mapPut(fileStrs, key^.id, p);
  end;
  fileString := p^;
end;

{ The pooled (file, line, column) record of loc, or null (item 13). }
function locationRecord(loc: TLoc): TV;
var
  key: PName;
  p: PTVBox;
  fid, line, col: longint;
  fname: string;
  fields, elems: TPtrList;
  ty: PIRType;
begin
  if loc = noLoc then
    exit(irNull);
  smPresumed(loc, fid, line, col);
  fname := smFileName(fid);
  key := intern(fname + ':' + intToStr(line) + ':' + intToStr(col));
  p := PTVBox(mapGet(locRecs, key^.id));
  if p = nil then
  begin
    listInit(fields);
    listAdd(fields, irPtrT);
    listAdd(fields, irI32);
    listAdd(fields, irI32);
    ty := irStructLit(fields);
    listFree(fields);
    listInit(elems);
    listAdd(elems, irBox(fileString(fname)));
    listAdd(elems, irBox(i32c(line)));
    listAdd(elems, irBox(i32c(col)));
    p := irBox(irGlobal('.loc', 'private', ty, irConstStruct(ty, elems), true,
                        false, false, 8));
    listFree(elems);
    mapPut(locRecs, key^.id, p);
  end;
  locationRecord := p^;
end;

{ __pas_runtime_error_at("<msg>", &loc); unreachable. Callers set a new
  insertion point afterwards. }
procedure emitRuntimeErrorB(const msg: TBuf; loc: TLoc);
var
  m, l: TV;
begin
  { in locals: FPC evaluates arguments right to left }
  m := errorMessageB(msg);
  l := locationRecord(loc);
  rtv2('__pas_runtime_error_at', m, l);
  irUnreachable;
end;

procedure emitRuntimeError(const msg: string; loc: TLoc);
var
  b: TBuf;
begin
  bufInit(b);
  bufAdd(b, msg);
  emitRuntimeErrorB(b, loc);
  bufFree(b);
end;

{ if cond then __pas_runtime_error(...) }
procedure emitRuntimeCheckB(const cond: TV; const msg: TBuf; loc: TLoc);
var
  errB, okB: PIRBlock;
begin
  if (cond.kind = vkInt) and irIsNullValue(cond) then
    exit; { provably fine }
  errB := irNewBlock('rt.error');
  okB := irNewBlock('rt.ok');
  irCondBr(cond, errB, okB, true);
  irSetInsertPoint(errB);
  emitRuntimeErrorB(msg, loc);
  irSetInsertPoint(okB);
end;

procedure emitRuntimeCheck(const cond: TV; const msg: string; loc: TLoc);
var
  b: TBuf;
begin
  bufInit(b);
  bufAdd(b, msg);
  emitRuntimeCheckB(cond, b, loc);
  bufFree(b);
end;

function rangeChecks(loc: TLoc): boolean;
begin
  rangeChecks := checkEnabled(swRange, loc, optRange);
end;

function overflowChecks(loc: TLoc): boolean;
begin
  overflowChecks := checkEnabled(swOverflow, loc, optOverflow);
end;

{ In an unchecked block at loc: integer values converted to a narrower
  integer type wrap instead of being range-checked. }
function wraps(loc: TLoc): boolean;
begin
  wraps := checkEnabled(swWrap, loc, false);
end;

{ Whether t is an untyped file type (file, with no element). }
function isUntypedFileType(t: PType): boolean;
begin
  isUntypedFileType := (t <> nil) and (t^.kind = tyFile) and not t^.isText and
                       (t^.fileElement = nil);
end;

{ A widechar's code as a char, as Free Pascal converts it: '?' above 255. }
function wideToChar(const v: TV): TV;
var
  fits, ch: TV;
begin
  fits := irICmp('ule', v, irConstInt(irI16, 255), '');
  ch := irCast('trunc', v, irI8, '');
  wideToChar := irSelect(fits, ch, i8c(ord('?')), 'tochar');
end;

function signedTypeRange(bits: longint; var lo, hi: int64): boolean; forward;

{ An ordinal register value of type src as the register type of dst
  (extended by src's signedness, or truncated). No range check. }
function convertOrdinal(const v: TV; src, dst: PType): TV;
var
  d: PIRType;
  r: TV;
  tyLo, tyHi: int64;
begin
  d := regType(dst);
  if irSameType(v.t, d) then
    exit(v);
  if (getBase(src)^.kind = tyWideChar) and (getBase(dst)^.kind = tyChar) then
    exit(wideToChar(v));
  if d^.bits = 1 then
    exit(irICmp('ne', v, irConstInt(v.t, 0), ''));
  if d^.bits < v.t^.bits then
    r := irCast('trunc', v, d, '')
  else if isSignedOrdinal(src) then
    r := irCast('sext', v, d, '')
  else
    r := irCast('zext', v, d, '');
  { a known range survives a conversion that keeps the value: a sign
    extension, or a truncation to a signed type that holds the range
    (optimization3 item 4) }
  if v.rKnown and isSignedOrdinal(src) and isSignedOrdinal(dst) and
     not irIsConst(r) and signedTypeRange(d^.bits, tyLo, tyHi) and
     (v.rLo >= tyLo) and (v.rHi <= tyHi) then
  begin
    r.rKnown := true;
    r.rLo := v.rLo;
    r.rHi := v.rHi;
  end;
  convertOrdinal := r;
end;

{ An ordinal register value widened to i64 (CreateSExt / CreateZExt). }
function toI64(const v: TV; t: PType): TV;
begin
  if isSignedOrdinal(t) then
    toI64 := irCast('sext', v, irI64, '')
  else
    toI64 := irCast('zext', v, irI64, '');
end;

function toDouble(const v: TV; t: PType): TV;
begin
  if t^.kind = tyReal then
    exit(v);
  if isSignedOrdinal(t) then
    toDouble := irCast('sitofp', v, irDoubleT, 'toreal')
  else
    toDouble := irCast('uitofp', v, irDoubleT, 'toreal');
end;

{ x < lo or x > hi, for an i64 x }
function outsideRange(const x: TV; lo, hi: int64): TV;
var
  off, span: TV;
begin
  { one unsigned comparison: (uint64)(x - lo) > (uint64)(hi - lo); the
    subtraction wraps, so x < lo becomes a huge offset }
  off := irBin('sub', x, i64c(lo), '');
  span := irBin('sub', i64c(hi), i64c(lo), '');
  outsideRange := irICmp('ugt', off, span, '');
end;

{ Checks that ordinal value v (of type src) lies within target's bounds. }
procedure emitOverflowCheck(const cond: TV; const what: string; loc: TLoc); forward;

{ In a checked block or expression at loc (Wrap explicitly off): integer
  values converted to a narrower integer type are checked, whatever $R
  says. }
function checksConversions(loc: TLoc): boolean;
begin
  checksConversions := checkStateAt(swWrap, loc) = 0;
end;

procedure emitRangeCheckMsg(const v: TV; src, target: PType; loc: TLoc;
                            const msg: string);
var
  tb, fb: TOrdinalBounds;
  x, above, below: TV;
  intConv, checked: boolean;

  procedure fail(const cond: TV);
  begin
    if checked then
      emitOverflowCheck(cond, 'integer overflow', loc)
    else
      emitRuntimeCheck(cond, msg, loc);
  end;

begin
  { an integer converted to an integer type (not a subrange): wraps in an
    unchecked block, is checked (EIntOverflow) in a checked one whatever $R
    says }
  intConv := (target^.kind = tyInteger) and isIntegerLike(src);
  checked := intConv and checksConversions(loc);
  if (not rangeChecks(loc) and not checked) or not isOrdinal(target) then
    exit;
  if intConv and wraps(loc) then
    exit;
  tb := getOrdinalBounds(target);
  fb := getOrdinalBounds(src);
  if isSignedOrdinal(src) and isSignedOrdinal(target) and v.rKnown and
     (v.rLo >= tb.lo) and (v.rHi <= tb.hi) then
    exit;
  if isQWord(getBase(src)) then { its values, unsigned: those above target's }
  begin
    if isQWord(target) then
      exit;
    x := toI64(v, src);
    above := irICmp('ugt', x, i64c(tb.hi), '');
    if tb.lo > 0 then
    begin
      below := irICmp('ult', x, i64c(tb.lo), '');
      above := irBin('or', below, above, '');
    end;
    fail(above);
    exit;
  end;
  { (to qword, tb is 0..high(int64): the negative values are caught) }
  if (fb.lo >= tb.lo) and (fb.hi <= tb.hi) then
    exit; { every value of src fits }
  fail(outsideRange(toI64(v, src), tb.lo, tb.hi));
end;

procedure emitRangeCheck(const v: TV; src, target: PType; loc: TLoc);
begin
  emitRangeCheckMsg(v, src, target, loc, 'value out of range');
end;

{ The set value of bits bits with the members lo..hi, or with all the
  others. }
function setMaskConst(lo, hi: int64; invert: boolean; bits: longint): TV;
var
  l: TLimbs;
  k: longint;
  inside: boolean;
begin
  limbsOfInt(0, l);
  for k := 0 to bits - 1 do
  begin
    inside := (k >= lo) and (k <= hi);
    if inside <> invert then
      l[k div 32] := l[k div 32] or (int64(1) shl (k mod 32));
  end;
  setMaskConst := irConstLimbs(irInt(bits), l);
end;

{ A set value of fromBits bits (or an i64 bit index) as one of toBits bits. }
function setCast(const v: TV; fromBits, toBits: longint): TV;
begin
  if fromBits = toBits then
    setCast := v
  else if fromBits < toBits then
    setCast := irCast('zext', v, irInt(toBits), '')
  else
    setCast := irCast('trunc', v, irInt(toBits), '');
end;

{ The set value v of type src as a value of the set type dst (the members
  that do not fit are dropped). }
function convertSet(const v: TV; src, dst: PType): TV;
begin
  convertSet := setCast(v, setStorageBits(src), setStorageBits(dst));
end;

{ Converts a value for assignment or parameter passing: integer to real,
  and range checks of ordinal values (and set members) against the target. }
function toInterface(const obj: TV; i: PType): TV; forward;

function convertForAssign(const v: TV; src, dst: PType; loc: TLoc): TV;
var
  tb, fb: TOrdinalBounds;
  outside, w: TV;
  bits: longint;
begin
  if dst^.kind = tyInterface then
  begin
    if src^.kind = tyNil then
      exit(irZero(memType(dst)));
    if src^.kind = tyClass then
      exit(toInterface(v, dst));
    exit(v); { an interface (the same or a descendant) }
  end;
  if (dst^.kind = tyReal) and (src^.kind <> tyReal) then
    exit(toDouble(v, src));
  if isOrdinal(dst) and isOrdinal(src) then
  begin
    if (getBase(src)^.kind = tyWideChar) and (getBase(dst)^.kind = tyChar) then
    begin { then checked as a char }
      w := wideToChar(v);
      emitRangeCheck(w, getBase(dst), dst, loc);
      exit(convertOrdinal(w, getBase(dst), dst));
    end;
    emitRangeCheck(v, src, dst, loc);
    exit(convertOrdinal(v, src, dst));
  end;
  if (dst^.kind = tySet) and rangeChecks(loc) then
  begin
    tb := getOrdinalBounds(dst^.setElement);
    if isEmptySet(src) then
      exit(convertSet(v, src, dst));
    fb := getOrdinalBounds(getBase(src^.setElement));
    if (fb.lo >= tb.lo) and (fb.hi <= tb.hi) then
      exit(convertSet(v, src, dst));
    { (in src's representation: a member beyond it cannot be there) }
    bits := setStorageBits(src);
    outside := irBin('and', v, setMaskConst(tb.lo, tb.hi, true, bits), '');
    emitRuntimeCheck(irICmp('ne', outside, irZero(irInt(bits)), ''),
                     'set member out of range', loc);
  end;
  if dst^.kind = tySet then
    exit(convertSet(v, src, dst));
  convertForAssign := v;
end;

procedure excCall1(const name: string; const x: TV); forward;
function whereText(loc: TLoc): TV; forward;

{ if cond: integer overflow (what): a runtime error, or (objfpc) an
  EIntOverflow exception, as in Free Pascal with SysUtils. }
procedure emitOverflowCheck(const cond: TV; const what: string; loc: TLoc);
var
  errB, okB: PIRBlock;
begin
  if mode <> lmObjFPC then
  begin
    emitRuntimeCheck(cond, what, loc);
    exit;
  end;
  errB := irNewBlock('rt.error');
  okB := irNewBlock('rt.ok');
  irCondBr(cond, errB, okB, true);
  irSetInsertPoint(errB);
  excCall1('__pas_raise_overflow', whereText(loc));
  irUnreachable;
  irSetInsertPoint(okB);
end;

function signedTypeRange(bits: longint; var lo, hi: int64): boolean;
begin
  signedTypeRange := false;
  if (bits <= 0) or (bits > 64) then
    exit;
  if bits = 64 then
  begin
    lo := low(int64);
    hi := high(int64);
  end
  else
  begin
    lo := -(int64(1) shl (bits - 1));
    hi := (int64(1) shl (bits - 1)) - 1;
  end;
  signedTypeRange := true;
end;

function addOverflows(a, b: int64; var r: int64): boolean;
begin
  addOverflows := false;
  if b > 0 then
  begin
    if a > high(int64) - b then
      exit(true);
  end
  else if b < 0 then
    if a < low(int64) - b then
      exit(true);
  r := a + b;
end;

function subOverflows(a, b: int64; var r: int64): boolean;
var
  t: int64;
begin
  subOverflows := false;
  if b > 0 then
  begin
    { low(int64) + b, in steps: Free Pascal 3.2 (-Co) computes the sum as
      b - $8000000000000000, which overflows }
    t := high(int64) - b;
    t := -t - 1;
    if a < t then
      exit(true);
  end
  else if b < 0 then
    if a > high(int64) + b then
      exit(true);
  r := a - b;
end;

{ a * b in r, unless it overflows int64 (the tests of CERT INT32-C; div
  truncates, as in C). }
function mulOverflows(a, b: int64; var r: int64): boolean;
begin
  mulOverflows := true;
  if a > 0 then
  begin
    if b > 0 then
    begin
      if a > high(int64) div b then
        exit;
    end
    else if b < low(int64) div a then
      exit;
  end
  else if b > 0 then
  begin
    if a < low(int64) div b then
      exit;
  end
  else if (a <> 0) and (b < high(int64) div a) then
    exit;
  r := a * b;
  mulOverflows := false;
end;

{ The range of l op r in lo..hi, when it fits int64. }
function combineRange(const op: string; aKnown: boolean; aLo, aHi: int64;
                      bKnown: boolean; bLo, bHi: int64;
                      var lo, hi: int64): boolean;
var
  p1, p2, p3, p4: int64;
begin
  combineRange := false;
  if not (aKnown and bKnown) then
    exit;
  if op = 'add' then
  begin
    if addOverflows(aLo, bLo, lo) or addOverflows(aHi, bHi, hi) then
      exit;
    combineRange := true;
    exit;
  end;
  if op = 'sub' then
  begin
    if subOverflows(aLo, bHi, lo) or subOverflows(aHi, bLo, hi) then
      exit;
    combineRange := true;
    exit;
  end;
  if op = 'mul' then
  begin
    if mulOverflows(aLo, bLo, p1) or mulOverflows(aLo, bHi, p2) or
       mulOverflows(aHi, bLo, p3) or mulOverflows(aHi, bHi, p4) then
      exit;
    lo := p1;
    if p2 < lo then lo := p2;
    if p3 < lo then lo := p3;
    if p4 < lo then lo := p4;
    hi := p1;
    if p2 > hi then hi := p2;
    if p3 > hi then hi := p3;
    if p4 > hi then hi := p4;
    combineRange := true;
  end;
end;

{ l op r ('add', 'sub' or 'mul') with an overflow check if enabled }
function emitCheckedArith(const op: string; signed: boolean; const l, r: TV;
                          loc: TLoc; const name: string): TV;
var
  fname: string;
  fields: TPtrList;
  pairT: PIRType;
  f: PIRFunc;
  res, ovf, rv: TV;
  lo, hi, tyLo, tyHi: int64;
  safe: boolean;
begin
  safe := signed and combineRange(op, l.rKnown, l.rLo, l.rHi, r.rKnown,
                                  r.rLo, r.rHi, lo, hi) and
          signedTypeRange(l.t^.bits, tyLo, tyHi) and
          (lo >= tyLo) and (hi <= tyHi);
  if (not overflowChecks(loc)) or safe then
  begin
    { a proven result gets nsw (optimization3 item 4); an unchecked one
      wraps, as Pascal's unchecked arithmetic does }
    if safe then
      rv := irBinNsw(op, l, r, name)
    else
      rv := irBin(op, l, r, name);
    if safe then
    begin
      rv.rKnown := true;
      rv.rLo := lo;
      rv.rHi := hi;
    end;
    exit(rv);
  end;
  if signed then
    fname := 'llvm.s' + op + '.with.overflow.' + l.t^.text
  else
    fname := 'llvm.u' + op + '.with.overflow.' + l.t^.text;
  listInit(fields);
  listAdd(fields, l.t);
  listAdd(fields, irI1);
  pairT := irStructLit(fields);
  listFree(fields);
  f := intrinsic(fname, pairT, l.t, l.t, speculatable, '');
  res := call2(f, l, r, '');
  ovf := irExtract(res, 1, irI1, '');
  emitOverflowCheck(ovf, 'integer overflow', loc);
  rv := irExtract(res, 0, l.t, name);
  if signed and signedTypeRange(l.t^.bits, tyLo, tyHi) then
  begin
    rv.rKnown := true;
    rv.rLo := tyLo;
    rv.rHi := tyHi;
  end;
  emitCheckedArith := rv;
end;

{ --- Variables and frames --- }

{ Is a function result of type t written through a pointer the caller
  passes (to a temporary of its own), instead of being returned? Short
  strings are: as a value LLVM would move all their 256 bytes, one by
  one; so are records of more than 16 bytes without counted fields. The
  function's result variable is then that temporary. }
function returnsByPointer(t: PType): boolean;
begin
  if t = nil then
    returnsByPointer := false
  else if t^.kind = tyShortString then
    returnsByPointer := true
  else
    { optimization3 item 1: a record of more than two registers, without
      counted fields (which the callee releases on an exception) }
    returnsByPointer := (t^.kind = tyRecord) and not isManaged(t) and
                        (sizeOf_(t) > 16);
end;

{ Item 9: a by-reference parameter is an address: never nil, and it points
  to a value of its type. }
function byRefParamAttr(t: PType): string;
begin
  if t^.kind = tyUntyped then
    byRefParamAttr := 'nonnull'
  else
    byRefParamAttr := 'nonnull dereferenceable(' + intToStr(sizeOf_(t)) + ')';
end;

{ Item 4: the hidden result pointer is a fresh caller temporary: it cannot
  alias an argument, and LLVM's ABI can return through it. }
function resultParamAttr(t: PType): string;
begin
  resultParamAttr := 'noalias nonnull sret(' + memType(t)^.text + ') ' +
                     'dereferenceable(' + intToStr(sizeOf_(t)) + ')';
end;

function resultCallAttr(t: PType): string;
begin
  resultCallAttr := 'noalias sret(' + memType(t)^.text + ')';
end;

function resultArgIndex(r: PDecl): longint;
begin
  if needsLink(r) then
    resultArgIndex := 1
  else
    resultArgIndex := 0;
end;

{ A constref parameter passed as a pointer where a const one is not: of a
  type other than structured, short string, open array, array of const or
  procedural (those are passed as for const). }
function isConstRefPtr(p: PDecl): boolean;
var
  t: PType;
begin
  t := p^.varType;
  isConstRefPtr := p^.isConstRef and not isStructured(t) and
                   not (t^.kind in [tyShortString, tyOpenArray, tyArrayOfConst,
                                    tyUntyped, tyRoutine]);
end;

function isByRef(v: PDecl): boolean;
begin
  isByRef := ((v^.varKind = vkVarParam) and not (v^.varType^.kind in [tyArrayOfConst, tyOpenArray])) or
             isConstRefPtr(v) or
             (v^.varType^.kind = tyUntyped) or
             ((v^.varKind = vkConstParam) and isStructured(v^.varType)) or
             ((v^.varKind = vkResult) and returnsByPointer(v^.varType));
end;

function isParamDecl(v: PDecl): boolean;
begin
  isParamDecl := v^.varKind in [vkValueParam, vkVarParam, vkConstParam];
end;

{ pas.<outer>.<name> for routines, pas.<class>.<name> for methods. }
{ "$N" for the N-th routine of an overloaded name (objfpc) }
function overloadSuffix(d: PDecl): string;
begin
  overloadSuffix := '';
  if (d^.kind = dkRoutine) and (d^.overloadIndex <> 0) then
    overloadSuffix := '$' + intToStr(d^.overloadIndex);
end;

function mangle(r: PDecl): string;
var
  s: string;
  p: PDecl;
begin
  s := nm(r^.ident.name) + overloadSuffix(r);
  p := r;
  while p^.parent <> nil do
  begin
    p := p^.parent;
    s := nm(p^.ident.name) + overloadSuffix(p) + '.' + s;
  end;
  if p^.methodKind <> mtNone then
    { s starts with the method's name }
    s := lowerStr(nm(p^.ownerClass^.declName)) + '.' + s;
  { a class constructor or destructor (beside a method of the same name) }
  if r^.classInit = ciConstructor then
    s := s + '$cctor'
  else if r^.classInit = ciDestructor then
    s := s + '$cdtor';
  mangle := 'pas.' + s;
end;

function frameType(r: PDecl): PIRType;
begin
  frameType := PIRType(r^.cgFrame);
end;

function fnOf(r: PDecl): PIRFunc;
begin
  fnOf := PIRFunc(r^.cgFunc);
end;

{ The frame of routine target, reached from the current routine through
  static links. }
function framePtrFor(target: PDecl): TV;
var
  frame, slot: TV;
  r: PDecl;
begin
  { a routine without a frame record (curFrame is null): nothing reads
    the pointer }
  frame := curFrame;
  r := curRoutine;
  while (r <> target) and (r <> nil) do
  begin
    if not r^.cgLinkInFrame then { only the current routine: no nested ones }
      frame := irArg(curFn, 0)
    else
    begin
      slot := irStructGEP(frameType(r), frame, 0, '');
      frame := irLoad(irPtrT, slot, 'link');
    end;
    r := r^.parent;
  end;
  framePtrFor := frame;
end;

{ The variable holding the PasFile of input / output. }
function standardFile(const name: string): TV;
var
  g: TV;
begin
  if irFindGlobal('__pas_' + name, g) then
    exit(g);
  standardFile := irGlobal('__pas_' + name, 'external', irPtrT, irNull, false, false,
                           true, 0);
end;

function loadFile(const varAddr: TV): TV;
begin
  loadFile := irLoad(irPtrT, varAddr, 'file');
end;

function outputFile: TV;
begin
  outputFile := loadFile(standardFile('output'));
end;

function inputFile: TV;
begin
  inputFile := loadFile(standardFile('input'));
end;

{ Where a routine's variable v is kept (a by-reference parameter: the
  pointer to it): a slot of its routine's frame record, or an alloca of
  the current routine. }
function varSlot(v: PDecl): TV;
var
  frame: TV;
begin
  if v^.cgInFrame then
  begin
    frame := framePtrFor(v^.owner);
    exit(irStructGEP(frameType(v^.owner), frame, v^.cgSlot, nm(v^.ident.name)));
  end;
  { Sema marks every variable a nested routine can name }
  if (v^.owner <> curRoutine) or (v^.cgLocal = nil) then
  begin
    diagFatal(noLoc, 'internal compiler error: variable ''' + nm(v^.ident.spelling) +
                     ''' is not in the frame of its routine');
    sysExit(1);
  end;
  varSlot := PTVBox(v^.cgLocal)^;
end;

function listHasPtr(const l: TPtrList; p: pointer): boolean;
var
  i: longint;
begin
  for i := 0 to l.count - 1 do
    if listGet(l, i) = p then
      exit(true);
  listHasPtr := false;
end;

procedure listAddUnique(var l: TPtrList; p: pointer);
begin
  if not listHasPtr(l, p) then
    listAdd(l, p);
end;

procedure listAppendUnique(var dst: TPtrList; const src: TPtrList);
var
  i: longint;
begin
  for i := 0 to src.count - 1 do
    listAddUnique(dst, listGet(src, i));
end;

{ Is v kept in its routine's frame record? Variables that nested routines
  use are, those whose write must survive an exception are (pins), and the
  managed ones, which are released on the way out. The other variables get
  an alloca each, which the optimizer turns into registers. }
function inFrame(v: PDecl; pinAll: boolean; const pins: TPtrList): boolean;
begin
  inFrame := pinAll or listHasPtr(pins, v) or v^.captured or
             (not isByRef(v) and isManaged(v^.varType));
end;

{ The start value of a local variable or function result at addr: zero.
  ISO leaves them undefined; this makes programs deterministic. Without
  zero initialization (-fno-zero-init, the default from -O1) arrays,
  records and strings are only cleared when they hold managed values or
  files; a string starts empty. }
procedure emitZeroInit(const addr: TV; t: PType);
var
  ty: PIRType;
begin
  ty := memType(t);
  if not isStructured(t) then
  begin
    irStore(irZero(ty), addr);
    exit;
  end;
  if optZeroInit or isManaged(t) or containsFile(t) then
    emitMemSet(addr, i8c(0), i64c(ty^.size), ty^.align)
  else if t^.kind = tyShortString then
    irStore(i8c(0), addr);
end;

{ Keeps the alloca addr in memory: what is stored there before an
  exception is raised must be there when the routine continues after the
  _setjmp that catches it, where _longjmp has put the registers back. LLVM
  does not know that control arrives there from the protected code, and
  would read values it has in registers; once the address has been stored
  somewhere (a volatile store is never removed), it assumes that any call,
  _setjmp too, reads and writes the alloca. Used for the frame record of a
  routine that protects code and for the temporaries that hold managed
  values (read when an exception leaves the routine, or by a later
  statement). }
procedure pinInMemory(const addr: TV);
begin
  if pinFn <> curFn then
  begin
    pinSlot := irCreateTemp(irPtrT, 'pin');
    pinFn := curFn;
  end;
  irStoreVolatile(addr, pinSlot);
end;

{ Whether the index node is a plain variable whose range the analysis
  knows at or above loNeeded and small enough that the scaled offset
  stays inside 64 bits: nuw on the index GEP then (llvm-23.md LB-1). }
function indexFlags(n: PNode; loNeeded: int64): boolean;
var
  d: PDecl;
begin
  indexFlags := false;
  if n^.kind = nkParen then
    n := n^.operand;
  if (n^.kind <> nkNameRef) or (n^.refDecl = nil) then
    exit;
  d := n^.refDecl;
  if not d^.cgRangeKnown then
    exit;
  indexFlags := (d^.cgRangeLo >= loNeeded) and
                (d^.cgRangeHi <= loNeeded + 1073741824);
end;

function emitAddress(e: PNode): TV; forward;
function emitTuple(e: PNode): TV; forward;

{ A constant used as a member (TFoo.C, obj.C): its reference, which is
  emitted instead; otherwise e. }
function unwrapConst(e: PNode): PNode;
begin
  unwrapConst := e;
  if (e^.kind = nkField) and (e^.constRef <> nil) then
    unwrapConst := e^.constRef;
end;

{ Does e name a type (TFoo, TOuter.TInner)? }
function namesType(e: PNode): boolean;
begin
  if e^.kind = nkNameRef then
    exit((e^.refDecl <> nil) and (e^.refDecl^.kind = dkType));
  namesType := (e^.kind = nkField) and (e^.fieldDecl <> nil) and
               (e^.fieldDecl^.kind = dkType);
end;

{ The symbol of C variable v: the name as written unless given. }
function externalVarName(v: PDecl): string;
begin
  if v^.varExtName <> nil then
    externalVarName := nameStr(v^.varExtName)
  else
    externalVarName := nameStr(v^.ident.spelling);
end;

{ The C library's errno is per thread, not a variable: reached through the
  library's accessor (__errno_location, __error, _errno). }
function isErrno(v: PDecl): boolean;
begin
  isErrno := externalVarName(v) = 'errno';
end;

function classRuntime(const name, ret, params: string): PIRFunc; forward;

function errnoAddress: TV;
var
  name: string;
begin
  if (pos('darwin', cgTriple) > 0) or (pos('macos', cgTriple) > 0) or
     (pos('apple', cgTriple) > 0) then
    name := '__error'
  else if (pos('windows', cgTriple) > 0) or (pos('mingw', cgTriple) > 0) or
          (pos('win32', cgTriple) > 0) then
    name := '_errno'
  else
    name := '__errno_location';
  errnoAddress := call0(classRuntime(name, 'ptr', ''), 'errno');
end;

function varAddress(v: PDecl): TV;
var
  owner: PDecl;
  slot: TV;
begin
  if v^.absTarget <> nil then { an overlay has no storage }
    exit(emitAddress(v^.absTarget));
  if v^.varExternal and isErrno(v) then
    exit(errnoAddress);
  owner := v^.owner;
  { on E: TFoo do: E's storage (a temporary) }
  if (owner <> nil) and (v^.cgValue <> nil) then
    exit(PTVBox(v^.cgValue)^);
  if owner = nil then
  begin
    if v^.loc = noLoc then
      exit(standardFile(nm(v^.ident.name)));
    exit(PTVBox(v^.cgValue)^);
  end;
  slot := varSlot(v);
  if isByRef(v) then
    exit(irLoad(irPtrT, slot, nm(v^.ident.name) + '.ref'));
  varAddress := slot;
end;

{ The address of field f of record type rt at recAddr. The record is laid
  out first: it may be reached only through pointers so far. }
function fieldAddress(const recAddr: TV; rt: PType; f: PDecl): TV;
var
  ignored: PIRType;
  baseAlign: int64;
  addr: TV;
begin
  if rt^.kind = tyClass then
    ignored := classLayout(rt) { recAddr is the object }
  else
    ignored := memType(rt);
  baseAlign := addrAlign(recAddr, rt);
  addr := irGEP1(irI8, recAddr, i64c(f^.cgOffset), true, nm(f^.ident.name));
  noteComponentAlign(addr, baseAlign, f^.cgOffset, f^.fieldType);
  fieldAddress := addr;
end;

{ Creates (init) or releases the PasFile objects of all files in an object
  of type t at addr: files, arrays of files and file fields of records
  (fixed parts). name: the external name of a program parameter. }
procedure emitFileLifetime(const addr: TV; t: PType; init: boolean; const name: TV);
var
  size: int64;
  f, i, elem, next, done: TV;
  bd: TOrdinalBounds;
  pre, loopB, doneB: PIRBlock;
  phi: PIRInst;
  j: longint;
  fd: PDecl;
  textFlag: longint;
begin
  if not containsFile(t) then
    exit;
  case t^.kind of
    tyFile:
      begin
        if not init then
        begin
          rtv1('__pas_file_free', loadFile(addr));
          exit;
        end;
        { (an untyped file: kind 2, records of 128 bytes until reset or
          rewrite) }
        if t^.isText then
        begin
          size := 1;
          textFlag := 1;
        end
        else if t^.fileElement = nil then
        begin
          size := 1;
          textFlag := 2;
        end
        else
        begin
          size := sizeOf_(t^.fileElement);
          textFlag := 0;
        end;
        f := call3(rtFn('__pas_file_new'), i64c(size), i32c(textFlag), name, '');
        irStore(f, addr);
      end;
    tyArray:
      begin
        { forEachElement }
        bd := getOrdinalBounds(t^.indexType);
        pre := irInsertBlock;
        loopB := irNewBlock('files.loop');
        doneB := irNewBlock('files.done');
        irBr(loopB);
        irSetInsertPoint(loopB);
        i := irPhi(irI64, 'i', phi);
        irAddIncoming(phi, i64c(0), pre);
        elem := irGEP2(memType(t), addr, i64c(0), i, '');
        emitFileLifetime(elem, t^.elementType, init, irNull);
        next := irBin('add', i, i64c(1), '');
        irAddIncoming(phi, next, irInsertBlock);
        done := irICmp('eq', next, i64c(bd.hi - bd.lo + 1), '');
        irCondBr(done, doneB, loopB, false);
        irSetInsertPoint(doneB);
      end;
    tyRecord:
      for j := 0 to t^.fieldList^.fixed.count - 1 do
      begin
        fd := PDecl(listGet(t^.fieldList^.fixed, j));
        emitFileLifetime(fieldAddress(addr, t, fd), fd^.fieldType, init, irNull);
      end;
  else
    ;
  end;
end;

{ The argv position of a program parameter (other than input/output):
  1, 2, ... for distinct names in order; 0 if none. }
function programParamIndex(name: PName): longint;
var
  i, j, n: longint;
  id, prev: PIdent;
  seen: boolean;
begin
  programParamIndex := 0;
  n := 0;
  for i := 0 to progDecl^.progParams.count - 1 do
  begin
    id := PIdent(listGet(progDecl^.progParams, i));
    if nameEquals(id^.name, 'input') or nameEquals(id^.name, 'output') then
      continue;
    seen := false;
    for j := 0 to i - 1 do
    begin
      prev := PIdent(listGet(progDecl^.progParams, j));
      if prev^.name = id^.name then
        seen := true;
    end;
    if seen then
      continue;
    n := n + 1;
    if id^.name = name then
      exit(n);
  end;
end;

{ Creates the PasFile objects of the file variables among decls; program
  parameters are bound to their command-line arguments. }
procedure initFileVars(const decls: TPtrList; isProgram: boolean);
var
  i, idx: longint;
  v: PDecl;
  name, s: TV;
begin
  for i := 0 to decls.count - 1 do
  begin
    v := PDecl(listGet(decls, i));
    if (v^.kind <> dkVar) or (v^.absTarget <> nil) or not containsFile(v^.varType) then
      continue;
    name := irNull;
    if isProgram then
    begin
      idx := programParamIndex(v^.ident.name);
      if idx > 0 then
      begin
        s := globalString(v^.ident.spelling^.chars, 0, v^.ident.spelling^.len, '');
        name := call2(rtFn('__pas_program_param'), i32c(idx), s, '');
      end;
    end;
    emitFileLifetime(varAddress(v), v^.varType, true, name);
  end;
end;

{ Where routine r's body is: a method's implementation, else r. }
function bodyLoc(r: PDecl): TLoc;
begin
  if (r^.methodKind <> mtNone) and (r^.implDecl <> nil) then
    bodyLoc := r^.implDecl^.loc
  else
    bodyLoc := r^.loc;
end;

{$I pcgendbg.inc}

{ --- Expressions: forward declarations --- }

function visitExpr(e: PNode): TV; forward;
procedure visitStmt(s: PNode); forward;
function withBase(w: PWithContext): TV; forward;
function emitObject(e: PNode): TV; forward;
function emitPropertyRead(p: PDecl; const obj: TV; c: PType): TV; forward;
function emitArrayPropertyRead(e: PNode): TV; forward;
function emitMemberValue(e: PNode): TV; forward;

function emitI32(e: PNode): TV;
begin
  emitI32 := irCast('trunc', toI64(visitExpr(e), e^.typ), irI32, '');
end;

{ --- Short strings (Turbo/FPC) --- }

function getPStrConstant(p: PCharArray; start, count: int64): TV;
var
  b: TBuf;
  n: int64;
begin
  bufInit(b);
  n := imin(count, 255);
  bufAddChar(b, chr(n));
  bufAddChars(b, p, start, n);
  getPStrConstant := getStringConstant(b.data, 0, b.len);
  bufFree(b);
end;

function holdResult(const v: TV): TV; forward;
function emitAsyncCall(r: PDecl; const link, self_: TV; const args: TPtrList): TV; forward;
function emitIteratorCall(r: PDecl; const link, self_: TV; const args: TPtrList): TV; forward;
function emitIteratorNext(const it: TV): TV; forward;
function iteratorValueAddr(const it: TV): TV; forward;
function emitAwait(const t: TV; tt: PType): TV; forward;
function emitWhen(e: PNode; all: boolean): TV; forward;
procedure emitWriteArg(const f: TV; arg: PNode); forward;
function holdDynResult(const v: TV; t: PType): TV; forward;

procedure astrV1(const name: string; const x: TV);
var
  ignored: TV;
begin
  ignored := call1(classRuntime(name, 'void', 'ptr'), x, '');
end;

procedure astrV2(const name, params: string; const x, y: TV);
var
  ignored: TV;
begin
  ignored := call2(classRuntime(name, 'void', params), x, y, '');
end;

procedure astrV3(const name, params: string; const x, y, z: TV);
var
  ignored: TV;
begin
  ignored := call3(classRuntime(name, 'void', params), x, y, z, '');
end;

{ --- Dynamic arrays --- }

function dynLength(const a: TV): TV;
begin
  dynLength := call1(classRuntime('__pas_dyn_length', 'i64', 'ptr'), a, '');
end;

{ The constant PasDynInfo of arrays of type t (runtime/include/pascalrt.h);
  for managed elements, with functions that release and count one. }
function dynInfoOf(t: PType): TV;
var
  b: PTVBox;
  elem: PType;
  fini, addRef, g: TV;
  fields, elems: TPtrList;
  ty: PIRType;

  function op(const name: string): TV;
  var
    params: TPtrList;
    f: PIRFunc;
  begin
    listInit(params);
    listAdd(params, irPtrT);
    f := irCreateFunction(name, 'internal', irVoidT, params);
    listFree(params);
    irSetArgName(f, 0, 'elem');
    listAdd(dynOpElems, elem);
    listAdd(dynOpFns, f);
    op := irFuncRef(f);
  end;

begin
  b := PTVBox(mapGet(dynInfos, t^.id));
  if b <> nil then
    exit(b^);
  elem := t^.dynElement;
  fini := irNull;
  addRef := irNull;
  if isManaged(elem) then
  begin
    fini := op('dyn.fini');
    addRef := op('dyn.addref');
  end;
  listInit(fields);
  listAdd(fields, irI64);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  ty := irStructLit(fields);
  listInit(elems);
  listAdd(elems, irBox(i64c(sizeOf_(elem))));
  listAdd(elems, irBox(fini));
  listAdd(elems, irBox(addRef));
  g := irGlobal('dyninfo', 'internal', ty, irConstStruct(ty, elems), true, false, false, 0);
  mapPut(dynInfos, t^.id, irBox(g));
  dynInfoOf := g;
end;

{ The constant PasDynInfo of values of type t (size, fini and addref, as
  for a dynamic array's elements): a task's result. }
function typeInfoOf(t: PType): TV;
var
  b: PTVBox;
  fini, addRef, g: TV;
  fields, elems: TPtrList;
  ty: PIRType;

  function op(const name: string): TV;
  var
    params: TPtrList;
    f: PIRFunc;
  begin
    listInit(params);
    listAdd(params, irPtrT);
    f := irCreateFunction(name, 'internal', irVoidT, params);
    listFree(params);
    irSetArgName(f, 0, 'elem');
    listAdd(dynOpElems, t);
    listAdd(dynOpFns, f);
    op := irFuncRef(f);
  end;

begin
  b := PTVBox(mapGet(typeInfos, t^.id));
  if b <> nil then
    exit(b^);
  fini := irNull;
  addRef := irNull;
  if isManaged(t) then
  begin
    fini := op('type.fini');
    addRef := op('type.addref');
  end;
  listInit(fields);
  listAdd(fields, irI64);
  listAdd(fields, irPtrT);
  listAdd(fields, irPtrT);
  ty := irStructLit(fields);
  listInit(elems);
  listAdd(elems, irBox(i64c(sizeOf_(t))));
  listAdd(elems, irBox(fini));
  listAdd(elems, irBox(addRef));
  g := irGlobal('typeinfo', 'internal', ty, irConstStruct(ty, elems), true, false, false, 0);
  mapPut(typeInfos, t^.id, irBox(g));
  typeInfoOf := g;
end;

{ A string constant as an ansistring: static characters after a header
  (count -1, length); null when empty. }
function getAnsiConstant(p: PCharArray; start, count: int64): TV;
var
  key: PName;
  g: PTVBox;
  b: TBuf;
  i: longint;
begin
  if count = 0 then
    exit(irNull);
  key := internChars(p, start, count);
  g := PTVBox(mapGet(ansiConsts, key^.id));
  if g = nil then
  begin
    bufInit(b);
    for i := 1 to 8 do
      bufAddChar(b, chr(255));
    for i := 0 to 7 do
      bufAddChar(b, chr((count shr (8 * i)) and 255));
    bufAddChars(b, p, start, count);
    bufAddChar(b, chr(0));
    g := irBox(irGlobal('.astr', 'private', irArray(b.len, irI8),
                        irConstBytes(b.data, 0, b.len), true, false, false, 8));
    mapPut(ansiConsts, key^.id, g);
    bufFree(b);
  end;
  getAnsiConstant := irGEP1(irI8, g^, i64c(16), true, '');
end;

procedure emitLifetime(const start: boolean; const addr: TV; size: int64);
var
  f: PIRFunc;
  ignored: TV;
begin
  { (LLVM 23 dropped the size operand: the lifetime is the pointer's) }
  if start then
    f := intrinsic('llvm.lifetime.start.p0', irVoidT, irPtrT, nil,
                   'nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)',
                   'ptr captures(none)')
  else
    f := intrinsic('llvm.lifetime.end.p0', irVoidT, irPtrT, nil,
                   'nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)',
                   'ptr captures(none)');
  ignored := call1(f, addr, '');
end;

function newStringTemp(maxLen: int64): TV;
var
  tmp: TV;
  st: PStringTemp;
begin
  tmp := irCreateTemp(irArray(maxLen + 1, irI8), 'str');
  st := PStringTemp(arenaAlloc(sizeof(TStringTemp)));
  st^.addr := tmp;
  st^.size := maxLen + 1;
  listAdd(stringTemps, st);
  emitLifetime(true, tmp, st^.size);
  newStringTemp := tmp;
end;

{ Ends the lifetime of the short-string temporaries made since mark. }
procedure releaseStringTemps(mark: longint);
var
  k: longint;
  st: PStringTemp;
begin
  for k := mark to stringTemps.count - 1 do
  begin
    st := PStringTemp(listGet(stringTemps, k));
    emitLifetime(false, st^.addr, st^.size);
  end;
  while stringTemps.count > mark do
    listPop(stringTemps);
end;

{ A char or widechar expression's value as a char. }
function emitCharValue(e: PNode): TV;
var
  v: TV;
begin
  v := visitExpr(e);
  if getBase(e^.typ)^.kind = tyWideChar then
    v := wideToChar(v);
  emitCharValue := v;
end;

{ A character as the (pointer, count) piece of a join: an own entry byte
  holding it, instead of a fresh one-character string. }
function emitCharPiece(e: PNode): TV;
var
  slot: TV;
begin
  slot := irCreateTempInit(irI8, 'char.tmp', irZero(irI8));
  irStore(emitCharValue(e), slot);
  emitCharPiece := slot;
end;


{ Any string-like expression as a pointer to a length-prefixed string. }
function emitPStr(e: PNode): TV;
var
  t: PType;
  d: PDecl;
  tmp, c, dst, src: TV;
  n: int64;
begin
  t := e^.typ;
  if t^.kind = tyShortString then
    exit(visitExpr(e)); { designators give their address, results a temp }
  if t^.kind = tyAnsiString then
  begin
    c := visitExpr(e);
    tmp := newStringTemp(255);
    astrV3('__pas_astr_to_pstr', 'ptr,i32,ptr', tmp, i32c(255), c);
    exit(tmp);
  end;
  if e^.kind = nkStringLit then
    exit(getPStrConstant(e^.strValue^.chars, 0, e^.strValue^.len));
  if unwrapConst(e)^.kind = nkNameRef then
  begin
    d := unwrapConst(e)^.refDecl;
    if (d^.kind = dkConst) and (d^.constValue.kind = ckString) then
      exit(getPStrConstant(d^.constValue.strVal^.chars, 0, d^.constValue.strVal^.len));
  end;
  if isCharLike(t) then
  begin
    tmp := newStringTemp(1);
    irStore(i8c(1), tmp);
    c := emitCharValue(e);
    dst := irGEP1(irI8, tmp, i64c(1), false, '');
    irStore(c, dst);
    exit(tmp);
  end;
  { packed array [1..n] of char }
  n := imin(getStringLength(t), 255);
  tmp := newStringTemp(n);
  irStore(i8c(n), tmp);
  dst := irGEP1(irI8, tmp, i64c(1), false, '');
  src := emitAddress(e);
  emitMemCpy(dst, src, i64c(n));
  emitPStr := tmp;
end;

{ Any string-like expression, or a pchar, as an ansistring (borrowed: a
  new one is held until the statement ends). }
function emitAnsi(e: PNode): TV;
var
  t: PType;
  d: PDecl;
  s: TV;
begin
  t := e^.typ;
  if t^.kind = tyAnsiString then
    exit(visitExpr(e));
  if e^.kind = nkStringLit then
    exit(getAnsiConstant(e^.strValue^.chars, 0, e^.strValue^.len));
  if unwrapConst(e)^.kind = nkNameRef then
  begin
    d := unwrapConst(e)^.refDecl;
    if (d^.kind = dkConst) and (d^.constValue.kind = ckString) then
      exit(getAnsiConstant(d^.constValue.strVal^.chars, 0, d^.constValue.strVal^.len));
  end;
  if isCharLike(t) then
  begin
    s := emitCharValue(e);
    exit(holdResult(call1(classRuntime('__pas_astr_from_char', 'ptr', 'i8'), s, '')));
  end;
  if t^.kind = tyPointer then
  begin
    s := visitExpr(e);
    exit(holdResult(call1(classRuntime('__pas_astr_from_cstr', 'ptr', 'ptr'), s, '')));
  end;
  s := emitPStr(e);
  emitAnsi := holdResult(call1(classRuntime('__pas_astr_from_pstr', 'ptr', 'ptr'), s, ''));
end;

{ An element's value as stored in its TVarRec (kind: its VType). }
function emitConstArrayValue(e: PNode; var kind: longint): TV;
var
  t: PType;
  v, tmp: TV;
begin
  t := e^.typ;
  if (t^.kind = tyStringLit) or (t^.kind = tyAnsiString) then
  begin
    kind := 11; { vtAnsiString }
    exit(emitAnsi(e));
  end;
  if (t^.kind = tyShortString) or isStringType(t) then
  begin
    kind := 4; { vtString }
    exit(emitPStr(e));
  end;
  if (t^.kind = tyReal) or isIntegerLike(t) then
  begin
    v := visitExpr(e);
    if isIntegerLike(t) and (intBits(t) <> 64) then
    begin
      kind := 0; { vtInteger }
      exit(toI64(v, t));
    end;
    if t^.kind = tyReal then { vtExtended, vtInt64: by reference }
      kind := 3
    else
      kind := 16;
    tmp := irCreateTemp(v.t, 'arg');
    irStore(v, tmp);
    exit(tmp);
  end;
  if isCharBase(t) or (getBase(t)^.kind = tyBoolean) then
  begin
    if isCharBase(t) then { vtChar, vtBoolean }
      kind := 2
    else
      kind := 1;
    exit(irCast('zext', visitExpr(e), irI64, ''));
  end;
  if t^.kind = tyClass then { vtObject, vtClass, vtPointer }
    kind := 7
  else if t^.kind = tyClassRef then
    kind := 8
  else
    kind := 5;
  if (t^.kind = tyPointer) and not t^.untyped and (t^.pointee <> nil) and
     (t^.pointee^.kind = tyChar) then
    kind := 6; { vtPChar }
  emitConstArrayValue := visitExpr(e);
end;

{ The array of const passed for arg: another array of const, or [...] built
  in a temporary: a header slot (the count), then (VType, value) slots. }
function emitConstArray(arg: PNode): TV;
var
  tmp, v: TV;
  i, kind: longint;
  slot: int64;
begin
  if arg^.kind <> nkSetCtor then
    exit(visitExpr(arg));
  tmp := irCreateTemp(irArray(2 * (arg^.setElems.count + 1), irI64), 'args');
  irStore(i64c(arg^.setElems.count), tmp);
  slot := 2;
  for i := 0 to arg^.setElems.count - 1 do
  begin
    v := emitConstArrayValue(PSetElement(listGet(arg^.setElems, i))^.lo, kind);
    irStore(i64c(kind), irGEP1(irI64, tmp, i64c(slot), true, ''));
    irStore(v, irGEP1(irI64, tmp, i64c(slot + 1), true, ''));
    slot := slot + 2;
  end;
  emitConstArray := tmp;
end;

procedure emitStringAssign(const dst: TV; t: PType; value: PNode);
var
  src: TV;
begin
  src := emitPStr(value);
  rtv3('__pas_str_assign', dst, i32c(t^.maxLength), src);
end;

function cStringFor(paramTy: PType; arg: PNode; var v: TV): boolean; forward;

{ slice(a, n): the first n elements of a, as an open array. }
function emitSlice(c_: PNode): TV; forward;

{ The descriptor (data, count) of the open array parameter e. }
function openDescriptor(e: PNode): TV;
begin
  openDescriptor := irLoad(irPtrT, varAddress(e^.refDecl), 'open');
end;

function openCount(const desc: TV): TV;
begin
  openCount := irLoad(irI64, irGEP1(irI64, desc, i64c(1), true, ''), 'count');
end;

function newOpenDescriptor(const data, count: TV): TV;
var
  desc: TV;
begin
  desc := irCreateTemp(irArray(2, irI64), 'open');
  irStore(data, desc);
  irStore(count, irGEP1(irI64, desc, i64c(1), true, ''));
  newOpenDescriptor := desc;
end;

function emitSlice(c_: PNode): TV;
var
  a, ne: PNode;
  data, count, n: TV;
  bd: TOrdinalBounds;
begin
  a := PNode(listGet(c_^.args, 0));
  ne := PNode(listGet(c_^.args, 1));
  data := emitAddress(a);
  if a^.typ^.kind = tyArray then
  begin
    bd := getOrdinalBounds(a^.typ^.indexType);
    count := i64c(bd.hi - bd.lo + 1);
  end
  else
    count := openCount(openDescriptor(a));
  n := toI64(visitExpr(ne), ne^.typ);
  if rangeChecks(ne^.b) then
    emitRuntimeCheck(irICmp('ugt', n, count, ''), 'slice length out of bounds', ne^.b);
  emitSlice := newOpenDescriptor(data, n);
end;

function emitOpenArrayCtor(s: PNode; oa: PType): TV;
var
  elemT: PType;
  arrT: PIRType;
  arr, slot, v: TV;
  i: longint;
  e: PNode;
begin
  elemT := oa^.openElement;
  if s^.setElems.count = 0 then
    exit(newOpenDescriptor(irNull, i64c(0)));
  arrT := irArray(s^.setElems.count, memType(elemT));
  arr := irCreateTemp(arrT, 'elems');
  for i := 0 to s^.setElems.count - 1 do
  begin
    e := PSetElement(listGet(s^.setElems, i))^.lo;
    slot := irGEP2(arrT, arr, i64c(0), i64c(i), '');
    { borrowed: the callee counts what it keeps }
    if elemT^.kind = tyShortString then
      emitStringAssign(slot, elemT, e)
    else if elemT^.kind = tyAnsiString then
      irStore(emitAnsi(e), slot)
    else if isStructured(elemT) then
      copyAggregate(slot, emitAddress(e), elemT)
    else if cStringFor(elemT, e, v) then
      irStore(v, slot)
    else
      storeScalar(slot, convertForAssign(visitExpr(e), e^.typ, elemT, e^.b), elemT);
  end;
  emitOpenArrayCtor := newOpenDescriptor(arr, i64c(s^.setElems.count));
end;

{ The open array passed for arg: another one, [...] built in a temporary,
  an array, or a string constant. }
function emitOpenArrayArg(arg: PNode; oa: PType): TV;
var
  t: PType;
  d: PDecl;
  bd: TOrdinalBounds;
  data, c: TV;
begin
  t := arg^.typ;
  if arg^.kind = nkSetCtor then { typed as the parameter }
    exit(emitOpenArrayCtor(arg, oa));
  d := arg^.callDecl;
  if (arg^.kind = nkCall) and (d <> nil) and (d^.kind = dkBuiltin) and
     (d^.builtin = bkSlice) then
    exit(emitSlice(arg));
  if arg^.kind = nkCall then
  begin
    { a call's own result (a Copy, or a routine's array result): the
      count is the result's length }
    data := visitExpr(arg);
    exit(newOpenDescriptor(data, dynLength(data)));
  end;
  if t = oa then
    exit(openDescriptor(arg));
  if t^.kind = tyArray then
  begin
    bd := getOrdinalBounds(t^.indexType);
    data := emitAddress(arg);
    exit(newOpenDescriptor(data, i64c(bd.hi - bd.lo + 1)));
  end;
  if t^.kind = tyDynArray then
  begin
    data := visitExpr(arg);
    exit(newOpenDescriptor(data, dynLength(data)));
  end;
  if t^.kind = tyStringLit then
  begin
    data := emitAddress(arg);
    exit(newOpenDescriptor(data, i64c(getStringLength(t))));
  end;
  c := irCreateTemp(irI8, 'chr'); { a char constant }
  irStore(visitExpr(arg), c);
  emitOpenArrayArg := newOpenDescriptor(c, i64c(1));
end;

{ A value parameter's own copy of the open array in frame slot slot. }
procedure copyOpenArray(const slot: TV; oa: PType);
var
  et: PIRType;
  desc, data, n, copy_, size: TV;
begin
  et := memType(oa^.openElement);
  desc := irLoad(irPtrT, slot, 'open');
  data := irLoad(irPtrT, desc, 'data');
  n := openCount(desc);
  copy_ := irDynAlloca(et, n, 'copy');
  size := irBin('mul', n, i64c(sizeOf_(oa^.openElement)), '');
  emitMemCpy(copy_, data, size);
  irStore(newOpenDescriptor(copy_, n), slot);
end;

function emitStringFunction(e: PNode; k: TBuiltinKind): TV;
var
  a0: PNode;
  t: PType;
  c, lo, hi, isLower, dec, s0, s1, tmp, n1, n2, n32, ignored, len: TV;
  i: longint;
begin
  a0 := PNode(listGet(e^.args, 0));
  case k of
    bkLength:
      begin
        t := a0^.typ;
        if t^.kind = tyOpenArray then
          exit(irCast('trunc', openCount(openDescriptor(a0)), irI32, ''));
        if t^.kind = tyDynArray then
          exit(irCast('trunc', dynLength(visitExpr(a0)), irI32, ''));
        if t^.kind = tyArrayOfConst then
          exit(irCast('trunc', irLoad(irI64, visitExpr(a0), ''), irI32, ''));
        if t^.kind = tyAnsiString then
        begin
          s0 := visitExpr(a0);
          exit(call1(classRuntime('__pas_astr_length', 'i32', 'ptr'), s0, ''));
        end;
        if t^.kind = tyShortString then
        begin
          len := irLoad(irI8, emitPStr(a0), '');
          exit(irCast('zext', len, irI32, 'length'));
        end;
        if isCharLike(t) then
        begin
          ignored := visitExpr(a0);
          exit(i32c(1));
        end;
        exit(i32c(getStringLength(t)));
      end;
    bkUpCase:
      begin
        c := visitExpr(a0); { a char or a widechar }
        lo := irICmp('uge', c, irConstInt(c.t, ord('a')), '');
        hi := irICmp('ule', c, irConstInt(c.t, ord('z')), '');
        isLower := irBin('and', lo, hi, '');
        dec := irBin('sub', c, irConstInt(c.t, 32), '');
        exit(irSelect(isLower, dec, c, 'upcase'));
      end;
    bkPos:
      begin
        if (a0^.typ^.kind = tyAnsiString) or
           (PNode(listGet(e^.args, 1))^.typ^.kind = tyAnsiString) then
        begin
          s0 := emitAnsi(a0);
          s1 := emitAnsi(PNode(listGet(e^.args, 1)));
          exit(call2(classRuntime('__pas_astr_pos', 'i32', 'ptr,ptr'), s0, s1, ''));
        end;
        s0 := emitPStr(a0);
        s1 := emitPStr(PNode(listGet(e^.args, 1)));
        exit(call2(rtFn('__pas_str_pos'), s0, s1, ''));
      end;
    bkCopy:
      begin
        if a0^.typ^.kind = tyDynArray then
        begin
          s0 := visitExpr(a0);
          if e^.args.count > 1 then
            s1 := toI64(visitExpr(PNode(listGet(e^.args, 1))), PNode(listGet(e^.args, 1))^.typ)
          else
            s1 := i64c(0);
          if e^.args.count > 2 then
            tmp := toI64(visitExpr(PNode(listGet(e^.args, 2))), PNode(listGet(e^.args, 2))^.typ)
          else
            tmp := i64c($7FFFFFFFFFFFFFFF);
          exit(holdDynResult(call3(classRuntime('__pas_dyn_copy', 'ptr', 'ptr,i64,i64'),
                                   s0, s1, tmp, ''), a0^.typ));
        end;
        if a0^.typ^.kind = tyAnsiString then
        begin
          s0 := visitExpr(a0);
          n1 := emitI32(PNode(listGet(e^.args, 1)));
          n2 := emitI32(PNode(listGet(e^.args, 2)));
          exit(holdResult(call3(classRuntime('__pas_astr_copy', 'ptr', 'ptr,i32,i32'),
                                s0, n1, n2, '')));
        end;
        tmp := newStringTemp(255);
        s0 := emitPStr(a0);
        n1 := emitI32(PNode(listGet(e^.args, 1)));
        n2 := emitI32(PNode(listGet(e^.args, 2)));
        rtv4('__pas_str_copy', tmp, s0, n1, n2);
        exit(tmp);
      end;
  else
    begin
      { concat: three joined in one allocation, then pairwise. The middle
        piece may be a character: an own byte, instead of a fresh string. }
      if e^.typ^.kind = tyAnsiString then
      begin
        if (e^.args.count >= 3) and
           (PNode(listGet(e^.args, 0))^.typ^.kind = tyAnsiString) and
           ((PNode(listGet(e^.args, 1))^.typ^.kind = tyAnsiString) or
            isCharLike(PNode(listGet(e^.args, 1))^.typ)) and
           (PNode(listGet(e^.args, 2))^.typ^.kind = tyAnsiString) then
        begin
          tmp := emitAnsi(a0);
          if PNode(listGet(e^.args, 1))^.typ^.kind = tyAnsiString then
          begin
            s1 := emitAnsi(PNode(listGet(e^.args, 1)));
            n32 := call1(classRuntime('__pas_astr_length', 'i32', 'ptr'), s1, '');
            n1 := irCast('zext', n32, irI64, '');
          end
          else
          begin
            s1 := emitCharPiece(PNode(listGet(e^.args, 1)));
            n1 := i64c(1);
          end;
          s0 := emitAnsi(PNode(listGet(e^.args, 2)));
          tmp := holdResult(call4(classRuntime('__pas_astr_join', 'ptr',
                                                'ptr,ptr,i64,ptr'),
                                  tmp, s1, n1, s0, ''));
          i := 3;
        end
        else
        begin
          tmp := emitAnsi(a0);
          i := 1;
        end;
        while i < e^.args.count do
        begin
          s1 := emitAnsi(PNode(listGet(e^.args, i)));
          tmp := holdResult(call2(classRuntime('__pas_astr_concat', 'ptr', 'ptr,ptr'),
                                  tmp, s1, ''));
          i := i + 1;
        end;
        exit(tmp);
      end;
      tmp := newStringTemp(255);
      s0 := emitPStr(a0);
      rtv3('__pas_str_assign', tmp, i32c(255), s0);
      for i := 1 to e^.args.count - 1 do
      begin
        s1 := emitPStr(PNode(listGet(e^.args, i)));
        rtv3('__pas_str_concat', tmp, tmp, s1);
      end;
      emitStringFunction := tmp;
    end;
  end;
end;

{ --- Sets --- }

{ An ordinal value as a bit index of a set of bits bits, checked to be in
  0..255. (A set constructor's representation holds every value of its
  elements' type that is in 0..255.) }
function setIndex(const v: TV; t: PType; loc: TLoc; bits: longint): TV;
var
  x, masked: TV;
  bd: TOrdinalBounds;
begin
  x := toI64(v, t);
  bd := getOrdinalBounds(t);
  if rangeChecks(loc) and ((bd.lo < 0) or (bd.hi > 255)) then
    emitRuntimeCheck(irICmp('ugt', x, i64c(255), ''), 'set element out of range 0..255',
                     loc);
  masked := irBin('and', x, i64c(bits - 1), '');
  setIndex := setCast(masked, 64, bits);
end;

function emitSetCtor(e: PNode): TV;
var
  res, one, allOnes, lo, hi, empty, loI, hiI, isTop, shifted, below, hiMask, loBits,
  loMask, both, range_: TV;
  i, bits: longint;
  el: PSetElement;
  lt, ht: PType;
  ty: PIRType;
begin
  bits := setStorageBits(e^.typ);
  ty := irInt(bits);
  res := irZero(ty);
  one := irConstInt(ty, 1);
  allOnes := irConstInt(ty, -1);
  for i := 0 to e^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(e^.setElems, i));
    lt := el^.lo^.typ;
    lo := visitExpr(el^.lo);
    if el^.hi = nil then
    begin
      loI := setIndex(lo, lt, el^.lo^.b, bits);
      shifted := irBin('shl', one, loI, '');
      res := irBin('or', res, shifted, '');
      continue;
    end;
    hi := visitExpr(el^.hi);
    ht := el^.hi^.typ;
    { bits lo..hi, or none if lo > hi }
    if isSignedOrdinal(lt) then
      empty := irICmp('sgt', lo, hi, '')
    else
      empty := irICmp('ugt', lo, hi, '');
    loI := setIndex(lo, lt, el^.lo^.b, bits);
    hiI := setIndex(hi, ht, el^.hi^.b, bits);
    isTop := irICmp('eq', hiI, irConstInt(ty, bits - 1), '');
    shifted := irBin('add', hiI, one, '');
    shifted := irBin('shl', one, shifted, '');
    below := irBin('sub', shifted, one, '');
    hiMask := irSelect(isTop, allOnes, below, '');
    loBits := irBin('shl', one, loI, '');
    loBits := irBin('sub', loBits, one, '');
    loMask := irBin('xor', loBits, allOnes, '');
    both := irBin('and', hiMask, loMask, '');
    range_ := irSelect(empty, irZero(ty), both, '');
    res := irBin('or', res, range_, '');
  end;
  emitSetCtor := res;
end;

{ x in [a, b..c, ...] without building the set (optimization.md, item 6).
  The members given by constants are tested together: one value or one
  range by comparisons; values at most 63 apart by a bit of a 64-bit mask;
  up to four ranges by comparisons; else the bit of the 256-bit set. Each
  other element is a comparison (a range: two). All elements are
  evaluated, in order, after x. }
procedure inOr(var res: TV; var has: boolean; const v: TV);
begin
  if has then
    res := irBin('or', res, v, 'in')
  else
    res := v;
  has := true;
end;

{ An element as an i64, checked like a set member (0..255). }
function inElement(el: PNode): TV;
var
  v: TV;
  bd: TOrdinalBounds;
begin
  v := toI64(visitExpr(el), el^.typ);
  bd := getOrdinalBounds(el^.typ);
  if not irIsConst(v) and rangeChecks(el^.b) and ((bd.lo < 0) or (bd.hi > 255)) then
    emitRuntimeCheck(irICmp('ugt', v, i64c(255), ''), 'set element out of range 0..255',
                     el^.b);
  inElement := v;
end;

function inRangeTest(const x: TV; l, h: int64): TV;
var
  atLeast, atMost: TV;
begin
  if l = h then
    exit(irICmp('eq', x, i64c(l), 'in'));
  atLeast := irICmp('sge', x, i64c(l), '');
  atMost := irICmp('sle', x, i64c(h), '');
  inRangeTest := irBin('and', atLeast, atMost, 'in');
end;

function emitInCtor(const x: TV; s: PNode): TV;
var
  members: array[0..255] of boolean;
  rangeLo, rangeHi: array[0..127] of longint;
  nRanges, i, j, k: longint;
  any: boolean;
  el: PSetElement;
  lo, hi, atLeast, atMost, idx, inside, masked, shifted, bit: TV;
  cl, ch, lmin, lmax, mask: int64;
  limbs: TLimbs;
  res: TV;
  has: boolean;
begin
  has := false;
  any := false;
  for k := 0 to 255 do
    members[k] := false;
  for i := 0 to s^.setElems.count - 1 do
  begin
    el := PSetElement(listGet(s^.setElems, i));
    lo := inElement(el^.lo);
    if el^.hi <> nil then
      hi := inElement(el^.hi)
    else
      hi := lo;
    if irIsConstInt(lo, cl) and irIsConstInt(hi, ch) then
    begin
      if cl < 0 then
        cl := 0;
      if ch > 255 then
        ch := 255;
      for k := cl to ch do
      begin
        members[k] := true;
        any := true;
      end;
      continue;
    end;
    if el^.hi = nil then
    begin
      inOr(res, has, irICmp('eq', x, lo, 'in'));
      continue;
    end;
    atLeast := irICmp('sge', x, lo, '');
    atMost := irICmp('sle', x, hi, '');
    inOr(res, has, irBin('and', atLeast, atMost, 'in'));
  end;
  if any then
  begin
    { the constant members: their ranges, lowest first }
    nRanges := 0;
    i := 0;
    while i < 256 do
    begin
      if members[i] then
      begin
        j := i;
        while (j + 1 < 256) and members[j + 1] do
          j := j + 1;
        rangeLo[nRanges] := i;
        rangeHi[nRanges] := j;
        nRanges := nRanges + 1;
        i := j;
      end;
      i := i + 1;
    end;
    lmin := rangeLo[0];
    lmax := rangeHi[nRanges - 1];
    if nRanges = 1 then
      inOr(res, has, inRangeTest(x, lmin, lmax))
    else if lmax - lmin < 64 then
    begin
      idx := irBin('sub', x, i64c(lmin), '');
      inside := irICmp('ule', idx, i64c(lmax - lmin), '');
      mask := 0;
      for k := lmin to lmax do
        if members[k] then
          mask := mask or (int64(1) shl (k - lmin));
      masked := irBin('and', idx, i64c(63), '');
      shifted := irBin('lshr', i64c(mask), masked, '');
      bit := irCast('trunc', shifted, irI1, '');
      inOr(res, has, irBin('and', inside, bit, 'in'));
    end
    else if nRanges <= 4 then
      for i := 0 to nRanges - 1 do
        inOr(res, has, inRangeTest(x, rangeLo[i], rangeHi[i]))
    else
    begin
      inside := irICmp('ule', x, i64c(255), '');
      idx := irBin('and', x, i64c(255), '');
      idx := irCast('zext', idx, irI256, '');
      limbsOfInt(0, limbs);
      for k := 0 to 255 do
        if members[k] then
          limbs[k div 32] := limbs[k div 32] or (int64(1) shl (k mod 32));
      shifted := irBin('lshr', irConstLimbs(irI256, limbs), idx, '');
      bit := irCast('trunc', shifted, irI1, '');
      inOr(res, has, irBin('and', inside, bit, 'in'));
    end;
  end;
  if has then
    emitInCtor := res
  else
    emitInCtor := irConstBool(false); { x in [] }
end;

{ x in s:  x is within s's representation and bit x of s is set }
function emitIn(e: PNode): TV;
var
  x, s, inRange, idx, shifted, bit: TV;
  bits: longint;
begin
  x := toI64(visitExpr(e^.lhs), e^.lhs^.typ);
  if e^.rhs^.kind = nkSetCtor then
    exit(emitInCtor(x, e^.rhs));
  s := visitExpr(e^.rhs);
  bits := setStorageBits(e^.rhs^.typ);
  inRange := irICmp('ule', x, i64c(bits - 1), '');
  idx := irBin('and', x, i64c(bits - 1), '');
  idx := setCast(idx, 64, bits);
  shifted := irBin('lshr', s, idx, '');
  bit := irCast('trunc', shifted, irI1, '');
  emitIn := irBin('and', inRange, bit, 'in');
end;

{ --- Addresses --- }

function emitConversion(e: PNode): TV; forward;

{ Is e a const parameter (whose value its caller owns)? }
function isConstParamRef(e: PNode): boolean;
begin
  isConstParamRef := (e^.kind = nkNameRef) and (e^.refDecl^.kind = dkVar) and
                     (e^.refDecl^.varKind = vkConstParam);
end;

procedure emitAnsiIndexCheck(const s, x: TV; loc: TLoc);
var
  bad: TV;
begin
  if rangeChecks(loc) then
  begin
    bad := call2(classRuntime('__pas_astr_bad_index', 'i32', 'ptr,i64'), s, x, '');
    bad := irICmp('ne', bad, i32c(0), '');
    emitRuntimeCheck(bad, 'string index out of bounds', loc);
  end;
end;

{ --- Bitpacked records and arrays (Free Pascal) --- }

type
  { where a bitpacked component's bits are: from bit bit (an i64, for an
    array's element: hasBit) or constBit (a record's field) of the size
    bytes at base; t is its type }
  TBitRef = record
    base, bit: TV;
    hasBit: boolean;
    constBit, size: int64;
    t: PType;
  end;
  { the bytes holding a component's bits, loaded and stored as one integer
    (ty, at ptr), and where the bits start in it (shift, of type ty) }
  TBitWindow = record
    ptr, shift: TV;
    ty: PIRType;
  end;

procedure bitRef(e: PNode; var r: TBitRef);
var
  idx, offset: TV;
  idxTy, at, rt: PType;
  bd: TOrdinalBounds;
  f: PDecl;
  ignored: PIRType;
begin
  if e^.kind = nkIndex then
  begin
    at := e^.base^.typ;
    r.base := emitAddress(e^.base);
    idx := visitExpr(e^.index);
    idxTy := e^.index^.typ;
    emitRangeCheckMsg(idx, idxTy, at^.indexType, e^.index^.b, 'array index out of bounds');
    bd := getOrdinalBounds(at^.indexType);
    offset := irBin('sub', toI64(idx, idxTy), i64c(bd.lo), 'idx');
    r.bit := irBin('mul', offset, i64c(bitPackedWidth(at^.elementType)), 'bit');
    r.hasBit := true;
    r.constBit := 0;
    r.size := sizeOf_(at);
    r.t := at^.elementType;
    exit;
  end;
  if e^.kind = nkField then
  begin
    f := e^.fieldDecl;
    rt := e^.base^.typ;
    r.base := emitAddress(e^.base);
  end
  else
  begin
    f := e^.refDecl;
    rt := e^.refWith^.rec^.typ;
    r.base := withBase(e^.refWith);
  end;
  ignored := memType(rt); { computes the bit offsets }
  r.hasBit := false;
  r.constBit := f^.cgBitOffset;
  r.size := sizeOf_(rt);
  r.t := f^.fieldType;
end;

procedure bitWindow(const r: TBitRef; var wd: TBitWindow);
var
  w, k: int64;
  byte_, shift, last: TV;
begin
  w := bitPackedWidth(r.t);
  if not r.hasBit then
  begin
    k := (r.constBit mod 8 + w + 7) div 8;
    byte_ := i64c(r.constBit div 8);
    shift := i64c(r.constBit mod 8);
  end
  else if w mod 8 = 0 then { whole bytes }
  begin
    k := w div 8;
    byte_ := irBin('lshr', r.bit, i64c(3), '');
    shift := i64c(0);
  end
  else if 8 mod w = 0 then { never across bytes }
  begin
    k := 1;
    byte_ := irBin('lshr', r.bit, i64c(3), '');
    shift := irBin('and', r.bit, i64c(7), '');
  end
  else
  begin
    k := imin((w + 14) div 8, r.size);
    if k = r.size then
    begin
      byte_ := i64c(0);
      shift := r.bit;
    end
    else
    begin
      byte_ := irBin('lshr', r.bit, i64c(3), '');
      last := i64c(r.size - k);
      byte_ := irSelect(irICmp('ugt', byte_, last, ''), last, byte_, '');
      shift := irBin('sub', r.bit, irBin('shl', byte_, i64c(3), ''), '');
    end;
  end;
  wd.ty := irInt(longint(k * 8));
  if k * 8 < 64 then
    shift := irCast('trunc', shift, wd.ty, '')
  else if k * 8 > 64 then
    shift := irCast('zext', shift, wd.ty, '');
  wd.shift := shift;
  wd.ptr := irGEP1(irI8, r.base, byte_, true, '');
end;

{ The value (register type) of the component r. }
function bitLoadRef(const r: TBitRef): TV;
var
  wd: TBitWindow;
  w: longint;
  v: TV;
  dst: PIRType;
begin
  bitWindow(r, wd);
  w := bitPackedWidth(r.t);
  v := irLoadAlign(wd.ty, wd.ptr, 1, 'bits');
  v := irBin('lshr', v, wd.shift, '');
  if wd.ty^.bits > w then
    v := irCast('trunc', v, irInt(w), '');
  dst := regType(r.t);
  if dst^.bits > w then
  begin
    if bitPackedSigned(r.t) then
      v := irCast('sext', v, dst, '')
    else
      v := irCast('zext', v, dst, '');
  end;
  bitLoadRef := v;
end;

{ The value (register type) of bitpacked component e. }
function bitLoad(e: PNode): TV;
var
  r: TBitRef;
begin
  bitRef(e, r);
  bitLoad := bitLoadRef(r);
end;

{ target := v for bitpacked component target: its bits replaced. }
procedure bitStore(target: PNode; v: TV);
var
  r: TBitRef;
  wd: TBitWindow;
  w, n: longint;
  mask, old, new_: TV;
begin
  bitRef(target, r);
  bitWindow(r, wd);
  w := bitPackedWidth(r.t);
  n := wd.ty^.bits;
  if v.t^.bits > w then
    v := irCast('trunc', v, irInt(w), '');
  if n > w then
    v := irCast('zext', v, wd.ty, '');
  v := irBin('shl', v, wd.shift, '');
  mask := irBin('shl', irLowBits(wd.ty, w), wd.shift, '');
  old := irLoadAlign(wd.ty, wd.ptr, 1, 'bits');
  new_ := irBin('or', irBin('and', old, irBin('xor', mask, irLowBits(wd.ty, n), ''), ''), v, '');
  irStoreAlign(new_, wd.ptr, 1);
end;

function emitAddress(e: PNode): TV;
var
  baseAlign: int64;
  elemAddr: TV;
  d: PDecl;
  base, idx, x, bad, offset, tmp: TV;
  bt, idxTy: PType;
  bd: TOrdinalBounds;
  w: PWithContext;
begin
  if (e^.convOp <> nil) and not e^.convSuspended then
    exit(emitConversion(e)); { converted to a record: a temporary }
  if isBitPackedComponent(e) then { (no address) a copy of its value }
  begin
    tmp := irCreateTemp(memType(e^.typ), 'bits.tmp');
    storeScalar(tmp, bitLoad(e), e^.typ);
    exit(tmp);
  end;
  if (e^.typ <> nil) and (e^.typ^.kind = tyOpenArray) then { its elements }
    exit(irLoad(irPtrT, openDescriptor(e), 'data'));
  case e^.kind of
    nkTuple: emitAddress := emitTuple(e);
    nkAwait, nkSpawn: emitAddress := visitExpr(e); { a structured result: in the task }
    nkCondExpr:
      begin
        { the hidden variable, once set }
        visitStmt(e^.ceLowered);
        emitAddress := emitAddress(e^.ceResult);
      end;
    nkBinary, nkUnary:
      { a record's operator returning a record: a temporary holding it }
      emitAddress := visitExpr(e);
    nkNameRef:
      begin
        d := e^.refDecl;
        case d^.kind of
          dkVar: emitAddress := varAddress(d);
          dkRoutine: emitAddress := visitExpr(e); { a record or string result }
          dkField:
            begin
              w := e^.refWith;
              emitAddress := fieldAddress(withBase(w), w^.rec^.typ, d);
            end;
          dkProperty:
            if d^.propIsClass then
              emitAddress := emitPropertyRead(d, irNull, nil)
            else
            begin
              w := e^.refWith;
              emitAddress := emitPropertyRead(d, withBase(w), w^.rec^.typ);
            end;
        else
          emitAddress := getNameConstant(d^.constValue.strVal);
        end;
      end;
    nkStringLit: emitAddress := getNameConstant(e^.strValue);
    nkParen: emitAddress := emitAddress(e^.inner);
    nkCall, nkIndirectCall, nkInherited:
      if (e^.kind = nkCall) and (e^.methodCast = 2) then
        emitAddress := emitAddress(PNode(listGet(e^.args, 0)))
      else if (e^.kind = nkCall) and (e^.methodCast = 1) then
      begin
        base := irCreateTemp(memType(e^.typ), 'method');
        irStore(visitExpr(PNode(listGet(e^.args, 0))), base);
        emitAddress := base;
      end
      else if (e^.kind = nkCall) and e^.isVarCast then
        emitAddress := emitAddress(PNode(listGet(e^.args, 0))) { T(x) of an untyped x }
      else
        { a record or string function result: a temporary holding it }
        emitAddress := visitExpr(e);
    nkIndex:
      if e^.idxProp <> nil then { a string or record value }
        emitAddress := emitArrayPropertyRead(e)
      else
      begin
        bt := e^.base^.typ;
        idxTy := e^.index^.typ;
        if bt^.kind = tyPointer then
        begin
          { p[i]: (p + i)^, unchecked as in Free Pascal }
          base := visitExpr(e^.base);
          x := toI64(visitExpr(e^.index), idxTy);
          exit(irGEP1(pointeeMemType(bt), base, x, false, ''));
        end;
        if bt^.kind = tyDynArray then
        begin
          base := visitExpr(e^.base);
          x := toI64(visitExpr(e^.index), idxTy);
          if rangeChecks(e^.index^.b) then
          begin
            bad := call2(classRuntime('__pas_dyn_bad_index', 'i32', 'ptr,i64'), base, x, '');
            bad := irICmp('ne', bad, i32c(0), '');
            emitRuntimeCheck(bad, 'array index out of bounds', e^.index^.b);
          end;
          exit(irGEP1(memType(bt^.dynElement), base, x, true, '',
                      indexFlags(e^.index, 0)));
        end;
        if bt^.kind = tyOpenArray then
        begin
          base := emitAddress(e^.base);
          x := toI64(visitExpr(e^.index), idxTy);
          if rangeChecks(e^.index^.b) then
          begin
            bad := irICmp('uge', x, openCount(openDescriptor(e^.base)), '');
            emitRuntimeCheck(bad, 'array index out of bounds', e^.index^.b);
          end;
          exit(irGEP1(memType(bt^.openElement), base, x, true, '',
                      indexFlags(e^.index, 0)));
        end;
        if bt^.kind = tyArrayOfConst then
        begin
          base := visitExpr(e^.base);
          x := toI64(visitExpr(e^.index), idxTy);
          if rangeChecks(e^.index^.b) then
          begin
            bad := call2(classRuntime('__pas_aoc_bad_index', 'i32', 'ptr,i64'), base, x, '');
            bad := irICmp('ne', bad, i32c(0), '');
            emitRuntimeCheck(bad, 'array index out of bounds', e^.index^.b);
          end;
          offset := irBin('add', x, i64c(1), '');
          exit(irGEP1(memType(bt^.aocElement), base, offset, true, ''));
        end;
        if bt^.kind = tyAnsiString then
        begin
          { an address may be written through: the string must be unshared,
            but a const parameter's belongs to the caller }
          base := emitAddress(e^.base);
          x := toI64(visitExpr(e^.index), idxTy);
          if isConstParamRef(e^.base) then
            base := irLoad(irPtrT, base, 's')
          else
            base := call1(classRuntime('__pas_astr_unique', 'ptr', 'ptr'), base, '');
          emitAnsiIndexCheck(base, x, e^.index^.b);
          offset := irBin('sub', x, i64c(1), '');
          exit(irGEP1(irI8, base, offset, true, '', indexFlags(e^.index, 1)));
        end;
        if bt^.kind = tyShortString then
        begin
          base := emitAddress(e^.base);
          idx := visitExpr(e^.index);
          if rangeChecks(e^.index^.b) then
          begin
            x := toI64(idx, idxTy);
            bad := outsideRange(x, 0, bt^.maxLength);
            emitRuntimeCheck(bad, 'string index out of bounds', e^.index^.b);
          end;
          exit(irGEP1(irI8, base, toI64(idx, idxTy), true, '',
                      indexFlags(e^.index, 0)));
        end;
        if bt^.kind = tyStringLit then
        begin
          { a string constant (Turbo/FPC): its characters are 1..N }
          base := emitAddress(e^.base);
          x := toI64(visitExpr(e^.index), idxTy);
          if rangeChecks(e^.index^.b) then
          begin
            bad := outsideRange(x, 1, bt^.strLength);
            emitRuntimeCheck(bad, 'string index out of bounds', e^.index^.b);
          end;
          offset := irBin('sub', x, i64c(1), '');
          exit(irGEP1(irI8, base, offset, true, '', indexFlags(e^.index, 1)));
        end;
        if bt^.kind = tyVector then
        begin
          { vector[N] of T (llvm-23.md LB-6): the 1..N components, laid out
            like an array of T (a GEP into the vector) }
          base := emitAddress(e^.base);
          idx := visitExpr(e^.index);
          x := toI64(idx, idxTy);
          if rangeChecks(e^.index^.b) then
          begin
            bad := outsideRange(x, 1, bt^.vecCount);
            emitRuntimeCheck(bad, 'vector index out of bounds', e^.index^.b);
          end;
          offset := irBin('sub', x, i64c(1), '');
          exit(irGEP2(memType(bt), base, i64c(0), offset, '',
                      indexFlags(e^.index, 1)));
        end;
        base := emitAddress(e^.base);
        idx := visitExpr(e^.index);
        emitRangeCheckMsg(idx, idxTy, bt^.indexType, e^.index^.b,
                          'array index out of bounds');
        bd := getOrdinalBounds(bt^.indexType);
        offset := irBin('sub', toI64(idx, idxTy), i64c(bd.lo), 'idx');
        baseAlign := addrAlign(base, bt);
        elemAddr := irGEP2(memType(bt), base, i64c(0), offset, '',
                           indexFlags(e^.index, bd.lo));
        { (elements are a multiple of their alignment apart) }
        if baseAlign < alignOf(bt^.elementType) then
          setAddrAlign(elemAddr, baseAlign);
        emitAddress := elemAddr;
      end;
    nkField:
      begin
        if e^.constRef <> nil then { TFoo.C: a string constant }
          exit(emitAddress(e^.constRef));
        if e^.fieldDecl^.kind = dkVar then { a class variable (not evaluating the object) }
          exit(PTVBox(e^.fieldDecl^.cgValue)^);
        if e^.base^.typ^.kind = tyClass then
        begin
          if e^.fieldDecl^.kind <> dkField then
            exit(emitMemberValue(e)); { a structured property or result }
          base := emitObject(e^.base);
          exit(fieldAddress(base, e^.base^.typ, e^.fieldDecl));
        end;
        if e^.fieldDecl^.kind <> dkField then { a record's method or property }
          exit(emitMemberValue(e)); { a structured result or property }
        base := emitAddress(e^.base);
        emitAddress := fieldAddress(base, e^.base^.typ, e^.fieldDecl);
      end;
    nkDeref:
      begin
        if e^.inner^.typ^.kind = tyFile then { the buffer variable f^ }
          exit(call1(rtFn('__pas_buffer'), loadFile(emitAddress(e^.inner)), ''));
        base := visitExpr(e^.inner);
        if rangeChecks(e^.inner^.b) then
          emitRuntimeCheck(irICmp('eq', base, irNull, ''), 'dereference of a nil pointer',
                           e^.inner^.b);
        emitAddress := base;
      end;
  else
    emitAddress := irNull; { Sema allows no other designators }
  end;
end;

{ --- Calls --- }

function staticLinkFor(r: PDecl): TV;
begin
  if r^.isClosure and not identValid(r^.lambdaCapture) then { uses no frame }
    exit(irNull);
  if r^.parent <> nil then
    staticLinkFor := framePtrFor(r^.parent)
  else
    staticLinkFor := irNull;
end;

{ the pair (function, static link) for a routine }
function routineEntry(r: PDecl): PIRFunc; forward;

function routinePairOf(r: PDecl): TV;
var
  pair, link, fn: TV;
begin
  r := canon(r);
  { the function a procedural value points to: an external routine, a
    cdecl routine's C entry, a top-level routine's link thunk, or the
    routine itself }
  fn := irFuncRef(routineEntry(r));
  pair := irInsert(irUndef(routinePairT), fn, 0, '');
  link := staticLinkFor(r);
  routinePairOf := irInsert(pair, link, 1, '');
end;

{ The routine value passed for a procedural parameter. }
function emitRoutinePair(e: PNode; t: PType): TV; forward;

function emitRoutineValue(arg: PNode): TV;
var
  d: PDecl;
begin
  if (arg^.kind = nkAddrOf) and (arg^.addrCode <> nil) then { @TFoo.M of a static method }
    exit(routinePairOf(arg^.addrCode));
  if arg^.kind = nkAddrOf then { @f (Turbo/FPC) }
    arg := arg^.inner;
  d := arg^.refDecl;
  if d^.kind = dkVar then { forwarding a parameter }
    exit(irLoad(memType(d^.varType), varAddress(d), ''));
  emitRoutineValue := routinePairOf(d);
end;

{ A string constant passed for a pchar parameter: a NUL-terminated copy. }
function cStringFor(paramTy: PType; arg: PNode; var v: TV): boolean;
var
  d: PDecl;
  c: char;
begin
  cStringFor := false;
  if (paramTy^.kind <> tyPointer) or paramTy^.untyped or (paramTy^.pointee = nil) or
     (paramTy^.pointee^.kind <> tyChar) then
    exit;
  if arg^.kind = nkStringLit then
  begin
    v := globalString(arg^.strValue^.chars, 0, arg^.strValue^.len, '.cstr');
    exit(true);
  end;
  if unwrapConst(arg)^.kind <> nkNameRef then
    exit;
  d := unwrapConst(arg)^.refDecl;
  if (d = nil) or (d^.kind <> dkConst) then
    exit;
  if d^.constValue.kind = ckString then
  begin
    v := globalString(d^.constValue.strVal^.chars, 0, d^.constValue.strVal^.len, '.cstr');
    exit(true);
  end;
  if (d^.constValue.kind = ckOrdinal) and (getBase(d^.constType)^.kind = tyChar) then
  begin
    c := chr(d^.constValue.intVal and 255);
    v := globalString(PCharArray(@c), 0, 1, '.cstr');
    exit(true);
  end;
end;

{ A runtime function used only by programs with classes. }
function classRuntime(const name, ret, params: string): PIRFunc;
var
  f: PIRFunc;
begin
  f := irFindFunction(name);
  if f = nil then
  begin
    declareRT(name, ret, params, 'nounwind');
    f := irFindFunction(name);
  end;
  classRuntime := f;
end;

{ --- Reference counting (COM interfaces, objfpc) --- }

{ The runtime's reference counting entry points (runtime/io.c). }
function intfRuntime(const name: string): PIRFunc;
begin
  if name = '__pas_intf_assign' then
    intfRuntime := classRuntime(name, 'void', 'ptr,ptr,ptr')
  else
    { __pas_intf_addref(obj), __pas_intf_clear(addr), __pas_free_instance,
      __pas_interfaced_construct(ed)(obj) }
    intfRuntime := classRuntime(name, 'void', 'ptr');
end;

procedure intfCall1(const name: string; const x: TV);
var
  ignored: TV;
begin
  ignored := call1(intfRuntime(name), x, '');
end;

{ Releases the COM interfaces in the object of managed type t at addr and
  sets them to nil (addRef false), or takes one more reference to each (a
  copy was made). }
procedure manageValue(const addr: TV; t: PType; addRef: boolean);
var
  i, elem, next, done, obj, desc, data, n, ignored: TV;
  bd: TOrdinalBounds;
  pre, loopB, doneB: PIRBlock;
  phi: PIRInst;
  j: longint;
  fd: PDecl;
  what: string;
begin
  case t^.kind of
    tyOpenArray:
      begin
        if addRef then
          what := 'addref'
        else
          what := 'fini';
        desc := irLoad(irPtrT, addr, 'open');
        data := irLoad(irPtrT, desc, 'data');
        n := openCount(desc);
        pre := irInsertBlock;
        loopB := irNewBlock(what + '.loop');
        doneB := irNewBlock(what + '.done');
        irCondBr(irICmp('eq', n, i64c(0), ''), doneB, loopB, false);
        irSetInsertPoint(loopB);
        i := irPhi(irI64, 'i', phi);
        irAddIncoming(phi, i64c(0), pre);
        elem := irGEP1(memType(t^.openElement), data, i, true, '');
        manageValue(elem, t^.openElement, addRef);
        next := irBin('add', i, i64c(1), '');
        irAddIncoming(phi, next, irInsertBlock);
        done := irICmp('eq', next, n, '');
        irCondBr(done, doneB, loopB, false);
        irSetInsertPoint(doneB);
      end;
    tyAnsiString:
      if addRef then
      begin
        obj := irLoad(irPtrT, addr, 's');
        astrV1('__pas_astr_addref', obj);
      end
      else
        astrV1('__pas_astr_clear', addr);
    tyDynArray:
      if addRef then
      begin
        obj := irLoad(irPtrT, addr, 'a');
        astrV1('__pas_dyn_addref', obj);
      end
      else
        astrV1('__pas_dyn_clear', addr);
    tyInterface:
      if addRef then
      begin
        obj := irLoad(irPtrT, addr, 'obj');
        intfCall1('__pas_intf_addref', obj);
      end
      else
        intfCall1('__pas_intf_clear', addr);
    tyTask, tyToken, tyIterator:
      if addRef then
      begin
        if t^.kind = tyTask then
        begin
          obj := irLoad(irPtrT, addr, 'task');
          ignored := call1(classRuntime('__pas_task_addref', 'void', 'ptr'), obj, '');
        end
        else if t^.kind = tyIterator then
        begin
          obj := irLoad(irPtrT, addr, 'iter');
          ignored := call1(classRuntime('__pas_gen_addref', 'void', 'ptr'), obj, '');
        end
        else
        begin
          obj := irLoad(irPtrT, addr, 'token');
          ignored := call1(classRuntime('__pas_token_addref', 'void', 'ptr'), obj, '');
        end;
      end
      else if t^.kind = tyTask then
        ignored := call1(classRuntime('__pas_task_clear', 'void', 'ptr'), addr, '')
      else if t^.kind = tyIterator then
        ignored := call1(classRuntime('__pas_gen_clear', 'void', 'ptr'), addr, '')
      else
        ignored := call1(classRuntime('__pas_token_clear', 'void', 'ptr'), addr, '');
    tyRoutine: { a closure: its frame }
      if t^.routineReference then
      begin
        if addRef then
        begin
          obj := irLoad(irPtrT, irStructGEP(memType(t), addr, 1, ''), 'frame');
          ignored := call1(classRuntime('__pas_frame_addref', 'void', 'ptr'), obj, '');
        end
        else
          ignored := call1(classRuntime('__pas_closure_clear', 'void', 'ptr'), addr, '');
      end;
    tyArray:
      begin
        if addRef then
          what := 'addref'
        else
          what := 'fini';
        bd := getOrdinalBounds(t^.indexType);
        pre := irInsertBlock;
        loopB := irNewBlock(what + '.loop');
        doneB := irNewBlock(what + '.done');
        irBr(loopB);
        irSetInsertPoint(loopB);
        i := irPhi(irI64, 'i', phi);
        irAddIncoming(phi, i64c(0), pre);
        elem := irGEP2(memType(t), addr, i64c(0), i, '');
        manageValue(elem, t^.elementType, addRef);
        next := irBin('add', i, i64c(1), '');
        irAddIncoming(phi, next, irInsertBlock);
        done := irICmp('eq', next, i64c(bd.hi - bd.lo + 1), '');
        irCondBr(done, doneB, loopB, false);
        irSetInsertPoint(doneB);
      end;
    tyRecord:
      for j := 0 to t^.allFields.count - 1 do
      begin
        fd := PDecl(listGet(t^.allFields, j));
        if isManaged(fd^.fieldType) then
          manageValue(fieldAddress(addr, t, fd), fd^.fieldType, addRef);
      end;
  else
    ;
  end;
end;

procedure finalizeValue(const addr: TV; t: PType);
begin
  manageValue(addr, t, false);
end;

procedure addRefValue(const addr: TV; t: PType);
begin
  manageValue(addr, t, true);
end;

{ dst^ := src^ for a record or array t: counted if it holds COM
  interfaces. }
procedure copyValue(const dst, src: TV; t: PType);
var
  old: TV;
begin
  if isManaged(t) then
  begin
    addRefValue(src, t);
    { the old value is released last: src and dst may be the same object }
    old := irCreateTemp(memType(t), 'old');
    copyAggregate(old, dst, t);
    copyAggregate(dst, src, t);
    finalizeValue(old, t);
  end
  else
    copyAggregate(dst, src, t);
end;

{ addr^ := v for a COM interface. }
procedure storeInterface(const addr, v: TV);
var
  obj, table, ignored: TV;
begin
  obj := irExtract(v, 0, irPtrT, 'obj');
  table := irExtract(v, 1, irPtrT, 'itable');
  ignored := call3(intfRuntime('__pas_intf_assign'), addr, obj, table, '');
end;

{ Stores v (scalar, of type t) at addr: counted for COM interfaces. }
procedure storeValue(const addr, v: TV; t: PType);
var
  fn, frame, ignored: TV;
begin
  if t^.kind = tyAnsiString then
  begin
    { A static string constant has refs < 0: storing it needs no count. }
    if irIsConst(v) then
      astrV2('__pas_astr_take', 'ptr,ptr', addr, v)
    else
      astrV2('__pas_astr_assign', 'ptr,ptr', addr, v);
  end
  else if t^.kind = tyDynArray then
    astrV2('__pas_dyn_assign', 'ptr,ptr', addr, v)
  else if isClosureType(t) then
  begin
    fn := irExtract(v, 0, irPtrT, 'fn');
    frame := irExtract(v, 1, irPtrT, 'frame');
    ignored := call3(classRuntime('__pas_closure_assign', 'void', 'ptr,ptr,ptr'),
                     addr, fn, frame, '');
  end
  else if t^.kind = tyTask then
    ignored := call2(classRuntime('__pas_task_assign', 'void', 'ptr,ptr'), addr, v, '')
  else if t^.kind = tyToken then
    ignored := call2(classRuntime('__pas_token_assign', 'void', 'ptr,ptr'), addr, v, '')
  else if t^.kind = tyIterator then
    ignored := call2(classRuntime('__pas_gen_assign', 'void', 'ptr,ptr'), addr, v, '')
  else if isManaged(t) then
    storeInterface(addr, v)
  else
    storeScalar(addr, v, t);
end;

{ Releases the ansistring or COM interface held in the temporary slot
  (ansistring temporaries are named astr.tmp). }
procedure clearTemp(const slot: TV);
begin
  if slot.agg <> nil then { a record temporary: agg is its type }
    finalizeValue(slot, PType(slot.agg))
  else if copy(slot.v, 1, 9) = '%astr.tmp' then
    astrV1('__pas_astr_clear', slot)
  else
    intfCall1('__pas_intf_clear', slot);
end;

{ A function's result v, a COM interface (a reference the caller now
  holds): kept in a temporary until the statement ends. }
function heldTemp(const v: TV): TV; forward;

function holdResult(const v: TV): TV;
var
  ignored: TV;
begin
  ignored := heldTemp(v);
  holdResult := v;
end;

{ holdResult's temporary: its address. }
function heldTemp(const v: TV): TV;
var
  slot: TV;
begin
  if irSameType(v.t, irPtrT) then
    slot := irCreateTempInit(v.t, 'astr.tmp', irZero(v.t))
  else
    slot := irCreateTempInit(v.t, 'intf.tmp', irZero(v.t));
  clearTemp(slot); { a loop's previous }
  if pinTemps then
    pinInMemory(slot);
  irStore(v, slot);
  listAdd(liveTemps, irBox(slot));
  if curRoutine <> nil then
    listAdd(routineTemps, irBox(slot));
  heldTemp := slot;
end;

{ A dynamic array result v of type t, a reference the caller now holds:
  kept in a temporary until the statement ends. }
function heldDynTemp(const v: TV; t: PType): TV; forward;

function holdDynResult(const v: TV; t: PType): TV;
var
  ignored: TV;
begin
  ignored := heldDynTemp(v, t);
  holdDynResult := v;
end;

{ holdDynResult's temporary: its address. }
function heldDynTemp(const v: TV; t: PType): TV;
var
  slot: TV;
begin
  slot := irCreateTempInit(v.t, 'dyn.tmp', irZero(v.t));
  slot.agg := t;
  clearTemp(slot); { a loop's previous }
  if pinTemps then
    pinInMemory(slot);
  irStore(v, slot);
  listAdd(liveTemps, irBox(slot));
  if curRoutine <> nil then
    listAdd(routineTemps, irBox(slot));
  heldDynTemp := slot;
end;

{ A function's record result v, holding ansistrings the caller now owns:
  kept in a temporary until the statement ends. Returns its address. }
function holdRecordResult(const v: TV; t: PType): TV;
var
  slot: TV;
begin
  slot := irCreateTempInit(v.t, 'rec.tmp', irZero(v.t));
  finalizeValue(slot, t); { a loop's previous }
  if pinTemps then
    pinInMemory(slot);
  irStore(v, slot);
  slot.agg := t;
  listAdd(liveTemps, irBox(slot));
  if curRoutine <> nil then
    listAdd(routineTemps, irBox(slot));
  holdRecordResult := slot;
end;

{ Releases the temporaries held since liveTemps had mark entries. }
procedure releaseTemps(mark: longint);
var
  k: longint;
begin
  for k := mark to liveTemps.count - 1 do
    clearTemp(PTVBox(listGet(liveTemps, k))^);
  while liveTemps.count > mark do
    listPop(liveTemps);
end;

{ Is c TInterfacedObject (the predeclared one) or a descendant? }
function isInterfacedClass(c: PType): boolean;
begin
  while c <> nil do
  begin
    if (c^.classModule = nil) and (nm(c^.declName) = 'TInterfacedObject') then
      exit(true);
    c := c^.classParent;
  end;
  isInterfacedClass := false;
end;

{ Does class c have fields of its own holding COM interfaces? }
function hasManagedFields(c: PType): boolean;
var
  k: longint;
  d: PDecl;
begin
  for k := 0 to c^.classMembers.count - 1 do
  begin
    d := PDecl(listGet(c^.classMembers, k));
    if (d^.kind = dkField) and isManaged(d^.fieldType) then
      exit(true);
  end;
  hasManagedFields := false;
end;

{ A variadic argument of an external routine, with C's promotions:
  ordinals to at least int, reals (doubles) as they are, string constants
  as C strings. }
function emitVarArg(arg: PNode): TV;
var
  t: PType;
  d: PDecl;
  v: TV;
begin
  t := arg^.typ;
  if t^.kind = tyStringLit then
  begin
    if arg^.kind = nkStringLit then
      exit(globalString(arg^.strValue^.chars, 0, arg^.strValue^.len, '.cstr'));
    d := unwrapConst(arg)^.refDecl;
    exit(globalString(d^.constValue.strVal^.chars, 0, d^.constValue.strVal^.len,
                      '.cstr'));
  end;
  v := visitExpr(arg);
  if isOrdinal(t) and (v.t^.kind = ikInt) and (v.t^.bits < 32) then
  begin
    if isSignedOrdinal(t) then
      exit(irCast('sext', v, irI32, ''));
    exit(irCast('zext', v, irI32, ''));
  end;
  emitVarArg := v;
end;

{ Appends the values passed for args (parameters params) to buf. toC: to
  an external (C) routine, which takes callbacks as function pointers, and
  maybe variadic arguments after params. }
procedure emitArgValues(const params, args: TPtrList; var buf: TBuf; toC: boolean);
var
  i: longint;
  p: PDecl;
  arg: PNode;
  v: TV;
  pt: PType;
  released: TPtrList; { of PTVBox, by argument: its address if released }
  slot, tmp: TV;
begin
  { out arguments holding managed values are released first, before any
    other argument is evaluated (another may be the same variable) }
  listInit(released);
  for i := 0 to args.count - 1 do
  begin
    if i >= params.count then
      break;
    p := PDecl(listGet(params, i));
    if p^.isOut and isManaged(p^.varType) then
    begin
      arg := PNode(listGet(args, i));
      if p^.varType^.kind = tyOpenArray then
      begin
        v := emitOpenArrayArg(arg, p^.varType);
        slot := irCreateTemp(irPtrT, 'open.slot');
        irStore(v, slot); { what finalizeValue takes }
        finalizeValue(slot, p^.varType);
      end
      else
      begin
        v := emitAddress(arg);
        finalizeValue(v, p^.varType);
      end;
      listAdd(released, irBox(v));
    end
    else
      listAdd(released, nil);
  end;
  i := -1;
  while (i + 1 < args.count) or (i + 1 < params.count) do
  begin
    i := i + 1;
    { arguments left out: the parameters' default values }
    if i < args.count then
      arg := PNode(listGet(args, i))
    else
      arg := PDecl(listGet(params, i))^.paramDefault;
    if arg = nil then
      break;
    if i >= params.count then
    begin
      irArgsAdd(buf, emitVarArg(arg));
      continue;
    end;
    if (i < released.count) and (listGet(released, i) <> nil) then
    begin
      irArgsAdd(buf, PTVBox(listGet(released, i))^);
      continue;
    end;
    p := PDecl(listGet(params, i));
    pt := p^.varType;
    if isConstRefPtr(p) then
    begin
      if arg^.constRefAddr then { the variable }
        irArgsAdd(buf, emitAddress(arg))
      else
      begin
        { a value (as for const): passed in a temporary }
        if not cStringFor(pt, arg, v) then
        begin
          if pt^.kind = tyAnsiString then
            v := emitAnsi(arg)
          else
            v := convertForAssign(visitExpr(arg), arg^.typ, pt, arg^.b);
        end;
        tmp := irCreateTemp(memType(pt), 'tmp');
        storeScalar(tmp, v, pt);
        irArgsAdd(buf, tmp);
      end;
      continue;
    end;
    if pt^.kind = tyUntyped then { Turbo/FPC: a variable, by reference }
      irArgsAdd(buf, emitAddress(arg))
    else if pt^.kind = tyOpenArray then
      irArgsAdd(buf, emitOpenArrayArg(arg, pt))
    else if pt^.kind = tyArrayOfConst then
      irArgsAdd(buf, emitConstArray(arg))
    else if cStringFor(pt, arg, v) then
      irArgsAdd(buf, v)
    else if (pt^.kind = tyRoutine) and (arg^.kind = nkNil) then
    begin
      if toC then { a default value }
        irArgsAdd(buf, irNull)
      else
        irArgsAdd(buf, emitRoutinePair(arg, pt));
    end
    else if (pt^.kind = tyRoutine) and toC then
      irArgsAdd(buf, irExtract(emitRoutineValue(arg), 0, irPtrT, ''))
    else if (pt^.kind = tyRoutine) and (pt^.routineOfObject or pt^.routineReference) then
    begin
      if p^.varKind = vkVarParam then
        irArgsAdd(buf, emitAddress(arg))
      else
        irArgsAdd(buf, emitRoutinePair(arg, pt));
    end
    else if pt^.kind = tyRoutine then
      irArgsAdd(buf, emitRoutineValue(arg))
    else if (pt^.kind = tyShortString) and (p^.varKind <> vkVarParam) then
      { any string: a const parameter reads it there, a value parameter
        copies what fits into its own variable }
      irArgsAdd(buf, emitPStr(arg))
    else if (p^.varKind = vkVarParam) or isStructured(pt) then
      irArgsAdd(buf, emitAddress(arg))
    else if pt^.kind = tyAnsiString then
      irArgsAdd(buf, emitAnsi(arg))
    else
      irArgsAdd(buf, convertForAssign(visitExpr(arg), arg^.typ, pt, arg^.b));
  end;
end;

function emitCallToSelf(ret: PIRType; const callee: TV; const link: TV;
                        hasLink: boolean; const self_: TV; hasSelf: boolean;
                        const params, args: TPtrList; resTy: PType): TV;
var
  buf: TBuf;
  tmp, res: TV;
  holdAnsi: boolean;
begin
  bufInit(buf);
  if hasLink then { external (C) routines have none }
    irArgsAdd(buf, link);
  if returnsByPointer(resTy) then
  begin
    if hasResultDst then
      tmp := resultDst
    else
      tmp := irCreateTemp(memType(resTy), 'result');
    irArgsAddAttr(buf, tmp, resultCallAttr(resTy));
  end;
  hasResultDst := false; { (only this call's result, not its arguments') }
  if hasSelf then { methods: after the result pointer }
    irArgsAdd(buf, self_);
  { The arguments' results are held as usual; only a direct call's own
    ansistring result may move into its assignment target. }
  holdAnsi := noHoldAnsiResult;
  noHoldAnsiResult := false;
  emitArgValues(params, args, buf, false);
  noHoldAnsiResult := holdAnsi;
  res := irCallPtr(ret, callee, buf, '');
  bufFree(buf);
  if returnsByPointer(resTy) then { expressions use a string's address }
    exit(tmp);
  { a string or record result comes back by value; expressions use its
    address (an interface, (ptr, ptr), is a value) }
  if (ret^.kind = ikArray) or ((ret^.kind = ikStruct) and not irSameType(ret, routinePairT)) then
  begin
    if (resTy <> nil) and (resTy^.kind = tyRecord) and isManaged(resTy) then
      exit(holdRecordResult(res, resTy));
    tmp := irCreateTemp(ret, 'result');
    irStore(res, tmp);
    exit(tmp);
  end;
  { (released by finalizeValue: also a closure's) }
  if (resTy <> nil) and ((resTy^.kind in [tyDynArray, tyTask, tyToken, tyIterator]) or
                         isClosureType(resTy)) then
    exit(holdDynResult(res, resTy));
  if (resTy <> nil) and (resTy^.kind = tyAnsiString) and holdAnsi then
  begin
    tookAnsiResult := true;
    exit(res);
  end;
  if (resTy <> nil) and isManaged(resTy) then
    exit(holdResult(res));
  emitCallToSelf := res;
end;

function emitCallTo(ret: PIRType; const callee: TV; const link: TV; hasLink: boolean;
                    const params, args: TPtrList; resTy: PType): TV;
begin
  emitCallTo := emitCallToSelf(ret, callee, link, hasLink, irNull, false, params, args,
                               resTy);
end;

function resultOf(r: PDecl): PType;
begin
  if r^.isFunction then
    resultOf := r^.resultType
  else
    resultOf := nil;
end;

function emitCall(r: PDecl; const args: TPtrList): TV;
var
  f: PIRFunc;
  buf: TBuf;
  res: TV;
begin
  r := canon(r);
  if r^.asyncType <> nil then
    exit(emitAsyncCall(r, staticLinkFor(r), irNull, args));
  if r^.iterType <> nil then
    exit(emitIteratorCall(r, staticLinkFor(r), irNull, args));
  f := fnOf(r);
  if r^.isExternal then
  begin
    bufInit(buf);
    emitArgValues(r^.params, args, buf, true);
    if r^.isVarArgs then
      res := irCallVarArg(f, irFuncRef(f), buf, '')
    else
      res := irCallPtr(f^.ret, irFuncRef(f), buf, '');
    bufFree(buf);
    exit(res);
  end;
  if needsLink(r) then
    emitCall := emitCallTo(f^.ret, irFuncRef(f), staticLinkFor(r), true, r^.params,
                           args, resultOf(r))
  else
    emitCall := emitCallTo(f^.ret, irFuncRef(f), irNull, false, r^.params, args,
                           resultOf(r));
end;

function retTypeOf(result_: PType): PIRType;
begin
  if (result_ = nil) or returnsByPointer(result_) then
    retTypeOf := irVoidT
  else
    retTypeOf := regType(result_);
end;

{ Calls the routine value pair of type rt; nilLoc: where to report a nil
  procedural value (noLoc: no check). }
function emitCallThrough(const pair: TV; rt: PType; const args: TPtrList; nilLoc: TLoc): TV;
var
  fn, link: TV;
begin
  fn := irExtract(pair, 0, irPtrT, '');
  if (nilLoc <> noLoc) and rangeChecks(nilLoc) then
    emitRuntimeCheck(irICmp('eq', fn, irNull, ''), 'call through a nil procedural value',
                     nilLoc);
  if rt^.routineOfObject then { (code, data): Self comes from the pair }
  begin
    link := irExtract(pair, 1, irPtrT, '');
    exit(emitCallToSelf(retTypeOf(rt^.routineResult), fn, irNull, true, link, true,
                        rt^.routineParams, args, rt^.routineResult));
  end;
  if rt^.routineCDecl then { a C entry (cthunk, external): no static link }
    exit(emitCallTo(retTypeOf(rt^.routineResult), fn, irNull, false,
                    rt^.routineParams, args, rt^.routineResult));
  link := irExtract(pair, 1, irPtrT, '');
  emitCallThrough := emitCallTo(retTypeOf(rt^.routineResult), fn, link, true,
                                rt^.routineParams, args, rt^.routineResult);
end;

{ A call through a procedural parameter or variable. }
function emitIndirectCall(v: PDecl; const args: TPtrList): TV;
var
  pair: TV;
  loc: TLoc;
begin
  pair := irLoad(memType(v^.varType), varAddress(v), '');
  { parameters always hold a routine; variables may be nil }
  loc := v^.loc;
  if isParamDecl(v) then
    loc := noLoc;
  emitIndirectCall := emitCallThrough(pair, v^.varType, args, loc);
end;

{ A procedural value of type t (Turbo/FPC): @routine, nil, or a
  procedural variable, field or element. }
function emitRoutinePair(e: PNode; t: PType): TV;
begin
  if e^.kind = nkNil then
    exit(irZero(memType(t)));
  if t^.routineOfObject then
    exit(visitExpr(e));
  if (e^.kind = nkAddrOf) and (e^.addrCode <> nil) then { @TFoo.M of a static method }
    exit(routinePairOf(e^.addrCode));
  if e^.kind = nkAddrOf then
    exit(routinePairOf(e^.inner^.refDecl));
  if t^.routineReference then { (also a call's result) }
    exit(visitExpr(e));
  emitRoutinePair := irLoad(memType(t), emitAddress(e), '');
end;

{ --- Objects and methods (objfpc) --- }

{ The object a with context stands for (Self for a method body), or the
  address of a with statement's record. }
function withBase(w: PWithContext): TV;
begin
  if w^.isSelf then { load Self; a record's (a type helper's) Self is its address }
  begin
    if (w^.rec^.typ^.kind = tyRecord) or
       ((w^.rec^.kind = nkNameRef) and (w^.rec^.refDecl <> nil) and
        (w^.rec^.refDecl^.kind = dkVar) and (w^.rec^.refDecl^.varKind = vkVarParam)) then
      withBase := emitAddress(w^.rec)
    else
      withBase := visitExpr(w^.rec);
  end
  else
    withBase := PTVBox(w^.cgAddr)^;
end;

{ if obj = nil then a runtime error, under range checks }
procedure emitNilObjectCheck(const obj: TV; loc: TLoc);
begin
  if rangeChecks(loc) then
    emitRuntimeCheck(irICmp('eq', obj, irNull, ''), 'use of a nil object reference', loc);
end;

{ The object an expression of a class type refers to, checked for nil. }
function emitObject(e: PNode): TV;
var
  obj: TV;
begin
  obj := visitExpr(e);
  emitNilObjectCheck(obj, e^.b);
  emitObject := obj;
end;

{ Entry slot of a VMT: 0 the parent's VMT, 1 the class name, 2 the
  instance size, 3 the interface table, 4... the virtual methods. }
function loadVMTSlot(const vmt: TV; slot: longint; const name: string): TV;
var
  p: TV;
begin
  p := irGEP1(irPtrT, vmt, i64c(slot), true, '');
  loadVMTSlot := irLoadInv(irPtrT, p, name);
end;

{ The address of virtual method index in the VMT of object obj. }
function loadVirtual(const obj: TV; index: longint): TV;
var
  vmt: TV;
begin
  vmt := irLoad(irPtrT, obj, 'vmt');
  loadVirtual := loadVMTSlot(vmt, 6 + index, 'method');
end;

{ if vmt = nil then a runtime error, under range checks }
procedure emitNilClassCheck(const vmt: TV; loc: TLoc);
begin
  if rangeChecks(loc) then
    emitRuntimeCheck(irICmp('eq', vmt, irNull, ''), 'use of a nil class reference', loc);
end;

{ The class (VMT) of a method call's receiver: obj itself if selfIsClass,
  else the object's (checked for nil). }
function receiverClass(const obj: TV; selfIsClass: boolean; loc: TLoc): TV;
begin
  if selfIsClass then
    exit(obj);
  emitNilObjectCheck(obj, loc);
  receiverClass := irLoad(irPtrT, obj, 'vmt');
end;

{ The VMT of class c; the predeclared classes' (TObject, Exception,
  EDivByZero and its ancestors) are in the runtime, as __pas_<class>_vmt. }
function vmtOf(c: PType): TV;
var
  g: PTVBox;
  v: TV;
  name: string;
begin
  g := PTVBox(mapGet(vmts, c^.id));
  if g <> nil then
    exit(g^);
  name := '__pas_' + lowerStr(nameStr(c^.declName)) + '_vmt';
  if not irFindGlobal(name, v) then
    v := irGlobal(name, 'external', irArray(6 + c^.classVTable.count, irPtrT),
                  irNull, true, false, true, 0);
  mapPut(vmts, c^.id, irBox(v));
  vmtOf := v;
end;

function methodFnType(r: PDecl): PIRType;
begin
  if r^.isFunction and not returnsByPointer(r^.resultType) then
    methodFnType := regType(r^.resultType)
  else
    methodFnType := irVoidT;
end;

{ --- Exceptions (objfpc) --- }

{ The runtime's exception entry points, declared when first used. }
function excRuntime(const name: string): PIRFunc;
var
  f: PIRFunc;
  ret, params, attrs: string;
begin
  f := irFindFunction(name);
  if f = nil then
  begin
    ret := 'void';
    params := 'ptr';
    attrs := 'nounwind';
    if (name = '__pas_raise_divzero') or (name = '__pas_raise_overflow') then
      attrs := 'cold noreturn nounwind'
    else if (name = '__pas_raise') or (name = '__pas_reraise_free') then
    begin
      params := 'ptr,ptr';
      attrs := 'noreturn nounwind';
    end
    else if name = '__pas_raise_at' then
    begin
      params := 'ptr,ptr,ptr';
      attrs := 'noreturn nounwind';
    end
    else if name = '__pas_reraise' then
      attrs := 'noreturn nounwind';
    { otherwise __pas_exc_pop, __pas_free_object }
    declareRT(name, ret, params, attrs);
    f := irFindFunction(name);
  end;
  excRuntime := f;
end;

procedure excCall1(const name: string; const x: TV);
var
  ignored: TV;
begin
  ignored := call1(excRuntime(name), x, '');
end;

procedure excCall2(const name: string; const x, y: TV);
var
  ignored: TV;
begin
  ignored := call2(excRuntime(name), x, y, '');
end;

{ ExceptAddr: the address of the exception the innermost enclosing handler
  handles (the frame it was raised to), nil outside handlers. }
function exceptAddr: TV;
begin
  if handledFrames.count = 0 then
    exit(irNull);
  exceptAddr := irLoad(irPtrT,
    irStructGEP(excFrameT, PTVBox(listGet(handledFrames, handledFrames.count - 1))^,
                3, ''), 'exceptaddr');
end;

{ Pushes a new PasExcFrame and calls _setjmp on it: an exception raised to
  the frame continues at landing, the protected code in body (the
  insertion point). Returns the frame. }
function emitProtectIn(landing, body: PIRBlock; const bodyName: string): TV;
var
  f, r: TV;
begin
  f := irCreateTemp(excFrameT, 'excframe');
  excCall1('__pas_exc_push', f);
  r := call1(rtFn('_setjmp'), irStructGEP(excFrameT, f, 4, ''), 'jmp');
  if body = nil then
    body := irNewBlock(bodyName);
  irCondBr(irICmp('ne', r, i32c(0), ''), landing, body, false);
  irSetInsertPoint(body);
  emitProtectIn := f;
end;

{ The same, the protected code in a new block named bodyName. }
function emitProtect(landing: PIRBlock; const bodyName: string): TV;
begin
  emitProtect := emitProtectIn(landing, nil, bodyName);
end;


{ "file:line:col" for the runtime's exception functions, or null. }
function whereText(loc: TLoc): TV;
var
  b: TBuf;
  fid, line, col: longint;
begin
  if loc = noLoc then
    exit(irNull);
  bufInit(b);
  smPresumed(loc, fid, line, col);
  bufAdd(b, smFileName(fid));
  bufAdd(b, ':' + intToStr(line) + ':' + intToStr(col));
  whereText := globalString(b.data, 0, b.len, '.where');
  bufFree(b);
end;

{ if cond: integer division by zero: a runtime error, or (objfpc) an
  EDivByZero exception, as in Free Pascal with SysUtils. }
procedure emitDivZeroCheck(const cond: TV; loc: TLoc);
var
  errB, okB: PIRBlock;
begin
  if mode <> lmObjFPC then
  begin
    emitRuntimeCheck(cond, 'division by zero', loc);
    exit;
  end;
  if (cond.kind = vkInt) and irIsNullValue(cond) then
    exit; { provably fine }
  errB := irNewBlock('rt.error');
  okB := irNewBlock('rt.ok');
  irCondBr(cond, errB, okB, true);
  irSetInsertPoint(errB);
  excCall1('__pas_raise_divzero', whereText(loc));
  irUnreachable;
  irSetInsertPoint(okB);
end;

{ Can a virtual call of r be a direct call? Only if no class of the
  whole-program hierarchy that descends from r's overrides it; with
  -fseparate-units only for a class a separately compiled unit cannot see
  (item 11). }
function canDevirtualize(r: PDecl): boolean;
var
  c, d: PType;
  i: longint;
  v: PDecl;
begin
  canDevirtualize := false;
  c := r^.ownerClass;
  if (c = nil) or (r^.vmtIndex < 0) or r^.isAbstract or
     (r^.cgFunc = nil) then
    exit;
  if separate and
     ((c^.classModule = nil) or (mapGet(exportedClasses, c^.id) <> nil)) then
    exit;
  if (r^.vmtIndex >= c^.classVTable.count) or
     (PDecl(listGet(c^.classVTable, r^.vmtIndex)) <> r) then
    exit;
  for i := 0 to allClasses.count - 1 do
  begin
    d := PType(listGet(allClasses, i));
    if (d = c) or not inheritsFrom(d, c) then
      continue;
    if r^.vmtIndex < d^.classVTable.count then
    begin
      v := PDecl(listGet(d^.classVTable, r^.vmtIndex));
      if v <> r then
        exit;
    end;
  end;
  canDevirtualize := true;
end;

function classIsClosed(c: PType): boolean;
begin
  classIsClosed := not separate or
                   ((c^.classModule <> nil) and
                    (mapGet(exportedClasses, c^.id) = nil));
end;

function classDepth(c: PType): longint;
var
  d: longint;
  a: PType;
begin
  d := 0;
  a := c;
  while (a <> nil) and (a^.classParent <> nil) do
  begin
    d := d + 1;
    a := a^.classParent;
  end;
  classDepth := d;
end;

function classHasDescendants(c: PType): boolean;
var
  i: longint;
  d: PType;
begin
  if c^.classModule = nil then { a predeclared class: the runtime's own
                                 descendants are not in allClasses }
    exit(true);
  if not classIsClosed(c) then
    exit(true);
  for i := 0 to allClasses.count - 1 do
  begin
    d := PType(listGet(allClasses, i));
    if (d <> c) and inheritsFrom(d, c) then
      exit(true);
  end;
  classHasDescendants := false;
end;

{ Is vmt the VMT of c or of a descendant? A class with no descendants only
  needs the equality test; otherwise the depth and ancestor display make it
  two loads and two compares (item 12). }
function classIsVMT(const vmt: TV; c: PType): TV;
var
  target, exact, depth, ancestors, slot, ancestor, inside: TV;
  d: longint;
begin
  target := vmtOf(c);
  if not classHasDescendants(c) then
    exit(irICmp('eq', vmt, target, 'is'));
  d := classDepth(c);
  exact := irICmp('eq', vmt, target, 'is.exact');
  depth := irCast('ptrtoint', loadVMTSlot(vmt, 4, 'depth'), irI64, '');
  ancestors := loadVMTSlot(vmt, 5, 'ancestors');
  slot := irGEP1(irPtrT, ancestors, i64c(d), true, '');
  ancestor := irLoad(irPtrT, slot, 'ancestor');
  inside := irBin('and', irICmp('uge', depth, i64c(d), ''),
                  irICmp('eq', ancestor, target, ''), 'is.display');
  classIsVMT := irBin('or', exact, inside, 'is');
end;

{ obj is c: false for nil, else classIsVMT. }
function emitClassIs(const obj: TV; c: PType): TV;
var
  nullB, checkB, endB: PIRBlock;
  vmt, r, res: TV;
  phi: PIRInst;
begin
  nullB := irNewBlock('is.null');
  checkB := irNewBlock('is.check');
  endB := irNewBlock('is.end');
  irCondBr(irICmp('eq', obj, irNull, ''), nullB, checkB, false);
  irSetInsertPoint(nullB);
  irBr(endB);
  irSetInsertPoint(checkB);
  vmt := irLoad(irPtrT, obj, 'vmt');
  r := classIsVMT(vmt, c);
  irBr(endB);
  irSetInsertPoint(endB);
  res := irPhi(irI1, 'is', phi);
  irAddIncoming(phi, irConstBool(false), nullB);
  irAddIncoming(phi, r, checkB);
  emitClassIs := res;
end;

{ obj as c: obj, or a runtime error if it is not a c (item 12). }
function emitClassAs(const obj: TV; c: PType; loc: TLoc): TV;
var
  nullB, checkB, errB, okB: PIRBlock;
  vmt: TV;
begin
  nullB := irNewBlock('as.null');
  checkB := irNewBlock('as.check');
  errB := irNewBlock('as.error');
  okB := irNewBlock('as.ok');
  irCondBr(irICmp('eq', obj, irNull, ''), nullB, checkB, false);
  irSetInsertPoint(nullB);
  irBr(okB);
  irSetInsertPoint(checkB);
  vmt := irLoad(irPtrT, obj, 'vmt');
  irCondBr(classIsVMT(vmt, c), okB, errB, false);
  irSetInsertPoint(errB);
  emitRuntimeError('invalid class typecast', loc);
  irSetInsertPoint(okB);
  emitClassAs := obj;
end;

{ obj.Free: if obj <> nil then obj.Destroy (virtual), then release it. }
procedure emitFree(const obj: TV);
var
  doB, endB: PIRBlock;
  fn, ignored: TV;
  args: TBuf;
begin
  doB := irNewBlock('free.do');
  endB := irNewBlock('free.end');
  irCondBr(irICmp('eq', obj, irNull, ''), endB, doB, false);
  irSetInsertPoint(doB);
  fn := loadVirtual(obj, 0);
  bufInit(args);
  irArgsAdd(args, irNull);
  irArgsAdd(args, obj);
  ignored := irCallPtr(irVoidT, fn, args, '');
  bufFree(args);
  intfCall1('__pas_free_instance', obj);
  irBr(endB);
  irSetInsertPoint(endB);
end;

{ Calls method r on object obj. inherited: a static call of the inherited
  method (no dispatch; a destructor does not release the object);
  otherwise a virtual method is called through the VMT, and a destructor
  releases the object afterwards. The result (an address for strings and
  records), or irNull for a call without a value. }
function emitConstructionThrough(const vmt: TV; ctor: PDecl; const args: TPtrList;
                                 loc: TLoc): TV; forward;

{ selfIsClass: obj is a class (a VMT), as in TFoo.M, c.M (c: class of T)
  and in class methods; then r is a class method or a constructor, which
  creates an object of that class. }
{ --- Advanced records (objfpc) --- }

{ r(args) on record obj (its address): a method, or a class method or
  operator (static: no Self). }
function emitRecordMethodCall(r: PDecl; const obj: TV; const args: TPtrList): TV;
var
  f: PIRFunc;
  res: TV;
begin
  if not hasSelf(r) then
    exit(emitCall(r, args));
  f := fnOf(canon(r));
  res := emitCallToSelf(f^.ret, irFuncRef(f), irNull, true, obj, true, r^.params, args,
                        resultOf(r));
  if r^.isFunction then
    emitRecordMethodCall := res
  else
    emitRecordMethodCall := irNull;
end;

{ The method of type helper member d (a property: its getter), or nil. }
function helperRoutine(d: PDecl): PDecl;
begin
  helperRoutine := nil;
  if d^.kind = dkProperty then
    d := d^.readDecl;
  if (d <> nil) and (d^.kind = dkRoutine) and (d^.ownerClass <> nil) and
     (d^.ownerClass^.classHelperFor <> nil) then
    helperRoutine := d;
end;

{ The Self of a type helper's member used through e: the address of its
  base, or of a temporary holding the base's value. }
function helperSelf(e: PNode): TV;
var
  base: PNode;
  t: PType;
  v, tmp: TV;
begin
  base := e^.base;
  if not e^.helperValue then
    exit(emitAddress(base));
  t := helperRoutine(e^.fieldDecl)^.ownerClass^.classHelperFor;
  if t^.kind = tyAnsiString then
  begin
    v := emitAnsi(base);
    astrV1('__pas_astr_addref', v);
    exit(heldTemp(v));
  end;
  if t^.kind = tyDynArray then
  begin
    v := visitExpr(base);
    astrV1('__pas_dyn_addref', v);
    exit(heldDynTemp(v, t));
  end;
  if t^.kind = tyShortString then
  begin
    tmp := newStringTemp(t^.maxLength);
    emitStringAssign(tmp, t, base);
    exit(tmp);
  end;
  tmp := irCreateTemp(memType(t), 'self.tmp');
  if isStructured(t) then
    copyAggregate(tmp, visitExpr(base), t)
  else
    storeScalar(tmp, visitExpr(base), t);
  helperSelf := tmp;
end;

{ TR.Create(args): a new record (zeroed, in a temporary) initialized by the
  constructor ctor. }
function emitRecordConstruction(rt: PType; ctor: PDecl; const args: TPtrList): TV;
var
  ty: PIRType;
  tmp, ignored: TV;
  f: PIRFunc;
begin
  ty := memType(rt);
  tmp := irCreateTemp(ty, 'rec');
  emitMemSet(tmp, i8c(0), i64c(ty^.size), ty^.align);
  f := fnOf(canon(ctor));
  ignored := emitCallToSelf(f^.ret, irFuncRef(f), irNull, true, tmp, true,
                            ctor^.params, args, nil);
  emitRecordConstruction := tmp;
end;

{ The object whose member base.M is used: base's object (checked for nil),
  or a record's address. }
function memberObject(base: PNode): TV;
begin
  if base^.typ^.kind = tyRecord then
    memberObject := emitAddress(base)
  else
    memberObject := emitObject(base);
end;

function emitMethodCallC(r: PDecl; const obj: TV; const args: TPtrList;
                         inherited_: boolean; loc: TLoc; selfIsClass: boolean): TV;
var
  vmt, fn, res, self_, arg, inh: TV;
  f: PIRFunc;
  impl, direct, msgField: PDecl;
begin
  emitMethodCallC := irNull;
  if r^.asyncType <> nil then
  begin
    if r^.isClassMethod then
      exit(emitAsyncCall(r, irNull, receiverClass(obj, selfIsClass, loc), args));
    exit(emitAsyncCall(r, irNull, obj, args));
  end;
  if r^.iterType <> nil then
  begin
    if r^.isClassMethod then
      exit(emitIteratorCall(r, irNull, receiverClass(obj, selfIsClass, loc), args));
    exit(emitIteratorCall(r, irNull, obj, args));
  end;
  if (r^.ownerClass <> nil) and ((r^.ownerClass^.classRecord <> nil) or
                                 (r^.ownerClass^.classHelperFor <> nil)) then
    exit(emitRecordMethodCall(r, obj, args));
  if (r^.methodKind = mtConstructor) and selfIsClass and not inherited_ then
    exit(emitConstructionThrough(obj, r, args, loc));
  case r^.objMethod of
    obCreate: exit; { TObject's constructor does nothing }
    obClassType: exit(irLoad(irPtrT, obj, 'vmt'));
    obClassParent: exit(loadVMTSlot(receiverClass(obj, selfIsClass, loc), 0, 'parent'));
    obInstanceSize:
      exit(irCast('ptrtoint', loadVMTSlot(receiverClass(obj, selfIsClass, loc), 2, 'size'),
                  irI64, 'instancesize'));
    obInheritsFrom:
      begin
        vmt := receiverClass(obj, selfIsClass, loc);
        arg := visitExpr(PNode(listGet(args, 0)));
        f := classRuntime('__pas_class_inherits', 'i32', 'ptr,ptr');
        inh := call2(f, vmt, arg, '');
        exit(irICmp('ne', inh, i32c(0), 'inherits'));
      end;
    obExceptionCreate: { FMessage := msg }
      begin
        msgField := findOwnMember(r^.ownerClass, intern('fmessage'));
        emitStringAssign(fieldAddress(obj, r^.ownerClass, msgField),
                         msgField^.fieldType, PNode(listGet(args, 0)));
        exit;
      end;
    obExceptionCreateFmt: { FMessage := Format(msg, args) }
      begin
        msgField := findOwnMember(r^.ownerClass, intern('fmessage'));
        arg := emitAnsi(PNode(listGet(args, 0)));
        inh := emitConstArray(PNode(listGet(args, 1)));
        astrV3('__pas_format_pstr', 'ptr,ptr,ptr', fieldAddress(obj, r^.ownerClass, msgField),
               arg, inh);
        exit;
      end;
    obFree:
      begin
        emitFree(obj);
        exit;
      end;
    obClassName:
      begin
        if selfIsClass then
          vmt := obj
        else
          vmt := irLoad(irPtrT, obj, 'vmt');
        exit(loadVMTSlot(vmt, 1, 'classname'));
      end;
    obDestroy:
      if inherited_ then { TObject's destructor does nothing }
        exit;
  else
    ;
  end;
  if r^.isClassMethod then { Self: the class }
  begin
    self_ := receiverClass(obj, selfIsClass, loc);
    if not inherited_ and (r^.vmtIndex >= 0) and not canDevirtualize(r) then
    begin
      emitNilClassCheck(self_, loc);
      fn := loadVMTSlot(self_, 6 + r^.vmtIndex, 'method');
      res := emitCallToSelf(methodFnType(r), fn, irNull, true, self_, true, r^.params, args,
                            resultOf(r));
    end
    else
    begin
      f := fnOf(r);
      res := emitCallToSelf(f^.ret, irFuncRef(f), irNull, true, self_, true, r^.params, args,
                            resultOf(r));
    end;
    if r^.isFunction then
      emitMethodCallC := res;
    exit;
  end;
  impl := nil;
  if (not inherited_) and (r^.vmtIndex >= 0) and (obj.knownClass <> nil) then
    if r^.vmtIndex < PType(obj.knownClass)^.classVTable.count then
    begin
      impl := PDecl(listGet(PType(obj.knownClass)^.classVTable, r^.vmtIndex));
      if (impl = nil) or impl^.isAbstract or (impl^.cgFunc = nil) then
        impl := nil;
    end;
  if not inherited_ and (r^.vmtIndex >= 0) and (impl = nil) and
     not canDevirtualize(r) then
  begin
    emitNilObjectCheck(obj, loc);
    fn := loadVirtual(obj, r^.vmtIndex);
    res := emitCallToSelf(methodFnType(r), fn, irNull, true, obj, true, r^.params, args,
                          resultOf(r));
  end
  else
  begin
    if impl <> nil then
      direct := impl
    else
      direct := r;
    f := fnOf(direct);
    res := emitCallToSelf(f^.ret, irFuncRef(f), irNull, true, obj, true, r^.params, args,
                          resultOf(direct));
  end;
  if (r^.methodKind = mtDestructor) and not inherited_ then
    intfCall1('__pas_free_instance', obj);
  if r^.isFunction then
    emitMethodCallC := res;
end;

function emitMethodCall(r: PDecl; const obj: TV; const args: TPtrList;
                        inherited_: boolean; loc: TLoc): TV;
begin
  emitMethodCall := emitMethodCallC(r, obj, args, inherited_, loc, false);
end;

{ --- Interfaces (objfpc, CORBA style) --- }

{ The iid of interface i (the predeclared IUnknown's is in the runtime). }
function iidOf(i: PType): TV;
var
  g: PTVBox;
  v: TV;
  name: string;
begin
  g := PTVBox(mapGet(iids, i^.id));
  if g <> nil then
    exit(g^);
  name := '__pas_' + lowerStr(nm(i^.declName)) + '_iid';
  if not irFindGlobal(name, v) then
    v := irGlobal(name, 'external', irI8, irNull, true, false, true, 0);
  mapPut(iids, i^.id, irBox(v));
  iidOf := v;
end;

function makeInterface(const obj, table: TV): TV;
var
  v: TV;
begin
  v := irInsert(irUndef(routinePairT), obj, 0, '');
  makeInterface := irInsert(v, table, 1, '');
end;

{ The interface i value of object obj: (obj, its class's table for i). }
function toInterface(const obj: TV; i: PType): TV;
var
  f: PIRFunc;
  table: TV;
begin
  f := classRuntime('__pas_intf_lookup', 'ptr', 'ptr,ptr');
  table := call2(f, obj, iidOf(i), 'itable');
  toInterface := makeInterface(obj, table);
end;

{ The object of an interface value (nil: itself). }
function interfaceObject(const v: TV): TV;
begin
  if v.t^.kind = ikStruct then
    interfaceObject := irExtract(v, 0, irPtrT, 'obj')
  else
    interfaceObject := v;
end;

{ Is w's object a class (Self in a class method)? }
function withIsClass(w: PWithContext): boolean;
begin
  withIsClass := w^.rec^.typ^.kind = tyClassRef;
end;

{ TFoo.Create(args): a new object of class c, initialized by ctor. The
  arguments are evaluated first. An exception leaving a constructor
  written in Pascal destroys and releases the new object (as in Free
  Pascal), so the call runs protected. }
function emitConstruction(c: PType; ctor: PDecl; const args: TPtrList; loc: TLoc): TV;
var
  obj, ignored, f: TV;
  argBuf, buf: TBuf;
  failed, done: PIRBlock;
  counted: boolean;
begin
  if ctor^.objMethod <> obNone then
  begin
    obj := call1(rtFn('__pas_new'), i64c(instanceSize(c)), 'obj');
    irStore(vmtOf(c), obj);
    obj.knownClass := c;
    ignored := emitMethodCall(ctor, obj, args, true, loc);
    exit(obj);
  end;
  bufInit(argBuf);
  emitArgValues(ctor^.params, args, argBuf, false);
  obj := call1(rtFn('__pas_new'), i64c(instanceSize(c)), 'obj');
  irStore(vmtOf(c), obj);
  counted := isInterfacedClass(ctor^.ownerClass);
  if counted then
    intfCall1('__pas_interfaced_construct', obj);
  if irNoRaise(fnOf(ctor)) then { nothing to protect against }
  begin
    bufInit(buf);
    irArgsAdd(buf, irNull);
    irArgsAdd(buf, obj);
    if argBuf.len > 0 then
    begin
      bufAdd(buf, ', ');
      bufAddChars(buf, argBuf.data, 0, argBuf.len);
    end;
    ignored := irCall(fnOf(ctor), buf, '');
    bufFree(buf);
    bufFree(argBuf);
    if counted then
      intfCall1('__pas_interfaced_constructed', obj);
    obj.knownClass := c;
    exit(obj);
  end;
  failed := irNewBlock('ctor.raise');
  f := emitProtect(failed, 'ctor.call');
  bufInit(buf);
  irArgsAdd(buf, irNull);
  irArgsAdd(buf, obj);
  if argBuf.len > 0 then
  begin
    bufAdd(buf, ', ');
    bufAddChars(buf, argBuf.data, 0, argBuf.len);
  end;
  ignored := irCall(fnOf(ctor), buf, '');
  bufFree(buf);
  bufFree(argBuf);
  excCall1('__pas_exc_pop', f);
  if counted then
    intfCall1('__pas_interfaced_constructed', obj);
  done := irNewBlock('ctor.done');
  irBr(done);
  irSetInsertPoint(failed);
  excCall2('__pas_reraise_free', f, obj);
  irUnreachable;
  irSetInsertPoint(done);
  obj.knownClass := c;
  emitConstruction := obj;
end;

{ The new object of class vmt, of its size (the VMT's slot 2). }
function allocateThrough(const vmt: TV): TV;
var
  size, obj: TV;
begin
  size := irCast('ptrtoint', loadVMTSlot(vmt, 2, 'size'), irI64, '');
  obj := call1(rtFn('__pas_new'), size, 'obj');
  irStore(vmt, obj);
  allocateThrough := obj;
end;

{ c.Create(args) with c a class (a VMT; also Self in a class method): a new
  object of the class c refers to, of its size, initialized by ctor
  (virtual: c's). Protected like emitConstruction. }
function emitConstructionThrough(const vmt: TV; ctor: PDecl; const args: TPtrList;
                                 loc: TLoc): TV;
var
  obj, ignored, f, fn: TV;
  argBuf, buf: TBuf;
  failed, done: PIRBlock;
  fun: PIRFunc;
  counted: boolean;
begin
  emitNilClassCheck(vmt, loc);
  if ctor^.objMethod <> obNone then
  begin
    obj := allocateThrough(vmt);
    ignored := emitMethodCall(ctor, obj, args, true, loc);
    exit(obj);
  end;
  bufInit(argBuf);
  emitArgValues(ctor^.params, args, argBuf, false);
  obj := allocateThrough(vmt);
  counted := isInterfacedClass(ctor^.ownerClass);
  if counted then
    intfCall1('__pas_interfaced_construct', obj);
  failed := irNewBlock('ctor.raise');
  f := emitProtect(failed, 'ctor.call');
  bufInit(buf);
  irArgsAdd(buf, irNull);
  irArgsAdd(buf, obj);
  if argBuf.len > 0 then
  begin
    bufAdd(buf, ', ');
    bufAddChars(buf, argBuf.data, 0, argBuf.len);
  end;
  if ctor^.vmtIndex >= 0 then
  begin
    fn := loadVMTSlot(vmt, 6 + ctor^.vmtIndex, 'ctor');
    ignored := irCallPtr(irVoidT, fn, buf, '');
  end
  else
  begin
    fun := fnOf(ctor);
    ignored := irCall(fun, buf, '');
  end;
  bufFree(buf);
  bufFree(argBuf);
  excCall1('__pas_exc_pop', f);
  if counted then
    intfCall1('__pas_interfaced_constructed', obj);
  done := irNewBlock('ctor.done');
  irBr(done);
  irSetInsertPoint(failed);
  excCall2('__pas_reraise_free', f, obj);
  irUnreachable;
  irSetInsertPoint(done);
  emitConstructionThrough := obj;
end;

{ obj.M(args) or TFoo.Create(args) through e (an nkField). }
{ base.r(args), base an interface value: r through its method table. }
function emitInterfaceCall(r: PDecl; base: PNode; loc: TLoc; const args: TPtrList): TV;
var
  obj, v, table, slot, fn, res: TV;
begin
  v := visitExpr(base);
  obj := irExtract(v, 0, irPtrT, 'obj');
  table := irExtract(v, 1, irPtrT, 'itable');
  if rangeChecks(loc) then
    emitRuntimeCheck(irICmp('eq', obj, irNull, ''), 'use of a nil interface reference',
                     loc);
  slot := irGEP1(irPtrT, table, i64c(r^.vmtIndex), true, '');
  fn := irLoad(irPtrT, slot, 'method');
  res := emitCallToSelf(methodFnType(r), fn, irNull, true, obj, true, r^.params, args,
                        resultOf(r));
  if r^.isFunction then
    exit(res);
  emitInterfaceCall := irNull;
end;

function emitMemberCall(e: PNode; const args: TPtrList): TV;
var
  r: PDecl;
  obj: TV;
begin
  r := e^.fieldDecl;
  if (r^.ownerClass <> nil) and (r^.ownerClass^.classHelperFor <> nil) then
  begin { a type helper's }
    if hasSelf(r) then
      exit(emitRecordMethodCall(r, helperSelf(e), args));
    exit(emitCall(r, args));
  end;
  if r^.isStatic and (e^.base^.typ^.kind <> tyRecord) then
    exit(emitCall(r, args)); { a class's static method: no Self }
  if e^.base^.typ^.kind = tyRecord then
  begin
    if namesType(e^.base) then
    begin { TR.Create, TR.M }
      if r^.methodKind = mtConstructor then
        exit(emitRecordConstruction(e^.base^.typ, r, args));
      exit(emitCall(r, args));
    end;
    if not hasSelf(r) then
      exit(emitCall(r, args));
    exit(emitRecordMethodCall(r, emitAddress(e^.base), args));
  end;
  if e^.base^.typ^.kind = tyInterface then { through the method table }
    exit(emitInterfaceCall(r, e^.base, e^.fieldName.loc, args));
  if namesType(e^.base) then { TFoo.Create, TOuter.TInner.Create }
  begin
    if r^.methodKind = mtConstructor then
      exit(emitConstruction(e^.base^.typ, r, args, e^.b));
    exit(emitMethodCallC(r, vmtOf(e^.base^.typ), args, false, e^.fieldName.loc, true));
  end;
  if e^.base^.typ^.kind = tyClassRef then { c.M, c: class of T }
  begin
    obj := visitExpr(e^.base);
    exit(emitMethodCallC(r, obj, args, false, e^.fieldName.loc, true));
  end;
  { not checked for nil: Free accepts nil, and a static method may be
    called on nil (as in Free Pascal); virtual calls are checked }
  obj := visitExpr(e^.base);
  emitMemberCall := emitMethodCall(r, obj, args, false, e^.fieldName.loc);
end;

{ inherited [M] [(args)]: on the enclosing method's Self. }
function emitInheritedCall(e: PNode; const args: TPtrList): TV;
var
  self_: TV;
begin
  if e^.inhMethod = nil then { a bare inherited with no inherited method }
    exit(irNull);
  self_ := irLoad(irPtrT, varAddress(e^.inhEnclosing^.selfVar), 'self');
  if not identValid(e^.inhName) and (args.count = 0) then
    emitInheritedCall := emitMethodCallC(e^.inhMethod, self_, e^.inhArgs, true, e^.b,
                                         e^.inhEnclosing^.isClassMethod)
  else
    emitInheritedCall := emitMethodCallC(e^.inhMethod, self_, args, true, e^.b,
                                         e^.inhEnclosing^.isClassMethod);
end;

{ The value of property p of object obj (static class c): an address for
  strings and records. }
{ The arguments of a property's accessor method: the indexes of an array
  property, the index constant, the value written (if any). }
procedure accessorArgs(p: PDecl; const indexes: TPtrList; value: PNode;
                       var args: TPtrList);
var
  i: longint;
begin
  listInit(args);
  for i := 0 to indexes.count - 1 do
    listAdd(args, listGet(indexes, i));
  if p^.propIndex <> nil then
    listAdd(args, p^.propIndex);
  if value <> nil then
    listAdd(args, value);
end;

{ indexes: an array property's }
function emitPropertyReadArgs(p: PDecl; const obj: TV; c: PType;
                              const indexes: TPtrList): TV;
var
  t: PType;
  addr: TV;
  args: TPtrList;
begin
  t := p^.propType;
  if p^.propIsClass then { a class variable or a static method }
  begin
    if p^.readDecl^.kind = dkVar then
    begin
      addr := PTVBox(p^.readDecl^.cgValue)^;
      if isStructured(t) then
        exit(addr);
      exit(loadScalar(addr, t, nm(p^.ident.name)));
    end;
    accessorArgs(p, indexes, nil, args);
    exit(emitCall(p^.readDecl, args));
  end;
  if p^.readDecl^.kind = dkField then
  begin
    addr := fieldAddress(obj, c, p^.readDecl);
    if isStructured(t) then
      exit(addr);
    exit(loadScalar(addr, t, nm(p^.ident.name)));
  end;
  accessorArgs(p, indexes, nil, args);
  emitPropertyReadArgs := emitMethodCall(p^.readDecl, obj, args, false, p^.loc);
end;

function emitPropertyRead(p: PDecl; const obj: TV; c: PType): TV;
var
  noArgs: TPtrList;
begin
  listInit(noArgs);
  emitPropertyRead := emitPropertyReadArgs(p, obj, c, noArgs);
end;

{ The object of an array property access e (obj.P[i], P[i], obj[i]), and
  its static class c. }
function propertyObject(e: PNode; var c: PType): TV;
var
  root: PNode;
begin
  root := e^.idxRoot;
  if (root^.kind = nkField) and (root^.fieldDecl = e^.idxProp) then
  begin
    c := root^.base^.typ;
    exit(memberObject(root^.base));
  end;
  if (root^.kind = nkNameRef) and (root^.refDecl = e^.idxProp) then
  begin
    c := root^.refWith^.rec^.typ;
    exit(withBase(root^.refWith));
  end;
  c := root^.typ; { the default property }
  propertyObject := emitObject(root);
end;

{ The value of array property access e (an address for strings and
  records). }
function emitArrayPropertyRead(e: PNode): TV;
var
  c: PType;
  obj: TV;
begin
  if e^.idxProp^.propIsClass then
    exit(emitPropertyReadArgs(e^.idxProp, irNull, nil, e^.idxArgs));
  obj := propertyObject(e, c);
  emitArrayPropertyRead := emitPropertyReadArgs(e^.idxProp, obj, c, e^.idxArgs);
end;

{ A member of a class used as a value through e (an nkField): a field, a
  property, or a call of a method without arguments. }
function emitMemberValue(e: PNode): TV;
var
  d: PDecl;
  c: PType;
  obj, addr: TV;
  noArgs: TPtrList;
begin
  d := e^.fieldDecl;
  if d^.kind = dkRoutine then
  begin
    listInit(noArgs);
    exit(emitMemberCall(e, noArgs));
  end;
  if d^.kind = dkVar then { a class variable }
  begin
    if d^.varType^.kind = tyShortString then
      exit(PTVBox(d^.cgValue)^);
    exit(loadScalar(PTVBox(d^.cgValue)^, d^.varType, nm(d^.ident.name)));
  end;
  c := e^.base^.typ;
  if (d^.kind = dkProperty) and d^.propIsClass then
    exit(emitPropertyRead(d, irNull, nil)); { (no object) }
  if (d^.kind = dkProperty) and (helperRoutine(d) <> nil) then { a type helper's }
    exit(emitPropertyRead(d, helperSelf(e), c));
  if (d^.kind = dkProperty) and (c^.kind = tyInterface) then { (its getter) }
  begin
    listInit(noArgs);
    exit(emitInterfaceCall(d^.readDecl, e^.base, e^.fieldName.loc, noArgs));
  end;
  obj := memberObject(e^.base);
  if d^.kind = dkProperty then
    exit(emitPropertyRead(d, obj, c));
  addr := fieldAddress(obj, c, d);
  if e^.typ^.kind = tyShortString then
    exit(addr);
  emitMemberValue := loadScalar(addr, e^.typ, nm(e^.fieldName.name));
end;

{ obj is TFoo / obj as TFoo: the runtime walks obj's chain of VMTs. }
function emitIsAs(e: PNode): TV;
var
  obj, r, msgG: TV;
  b: TBuf;
begin
  obj := visitExpr(e^.lhs);
  if e^.lhs^.typ^.kind = tyInterface then { a COM interface's object }
    obj := irExtract(obj, 0, irPtrT, 'obj');
  if e^.rhs^.typ^.kind = tyInterface then
  begin
    if e^.bop = boIs then
    begin
      r := call2(classRuntime('__pas_intf_lookup', 'ptr', 'ptr,ptr'), obj,
                 iidOf(e^.rhs^.typ), '');
      exit(irICmp('ne', r, irNull, 'is'));
    end;
    bufInit(b);
    bufAdd(b, 'invalid interface typecast');
    bufAddWhere(b, e^.opLoc);
    msgG := globalString(b.data, 0, b.len, '.rterr');
    bufFree(b);
    r := call3(classRuntime('__pas_intf_as', 'ptr', 'ptr,ptr,ptr'), obj,
               iidOf(e^.rhs^.typ), msgG, 'itable');
    exit(makeInterface(obj, r));
  end;
  if e^.bop = boIs then
    exit(emitClassIs(obj, e^.rhs^.typ));
  emitIsAs := emitClassAs(obj, e^.rhs^.typ, e^.opLoc);
end;

{ --- Operators --- }

function emitShortCircuit(e: PNode): TV;
var
  isAnd: boolean;
  l, r, res: TV;
  lhsEnd, rhsEnd, rhsB, endB: PIRBlock;
  phi: PIRInst;
begin
  isAnd := e^.bop = boAnd;
  l := visitExpr(e^.lhs);
  lhsEnd := irInsertBlock;
  if isAnd then
  begin
    rhsB := irNewBlock('and.rhs');
    endB := irNewBlock('and.end');
    irCondBr(l, rhsB, endB, false);
  end
  else
  begin
    rhsB := irNewBlock('or.rhs');
    endB := irNewBlock('or.end');
    irCondBr(l, endB, rhsB, false);
  end;
  irSetInsertPoint(rhsB);
  r := visitExpr(e^.rhs);
  rhsEnd := irInsertBlock;
  irBr(endB);
  irSetInsertPoint(endB);
  if isAnd then
    res := irPhi(irI1, 'and', phi)
  else
    res := irPhi(irI1, 'or', phi);
  irAddIncoming(phi, irConstBool(not isAnd), lhsEnd);
  irAddIncoming(phi, r, rhsEnd);
  emitShortCircuit := res;
end;

function signedMin(t: PIRType): TV;
var
  l: TLimbs;
begin
  limbsOfInt(0, l);
  l[(t^.bits - 1) div 32] := int64(1) shl ((t^.bits - 1) mod 32);
  signedMin := irConstLimbs(t, l);
end;

{ v's range, lo..hi, unless v is a constant (whose range is exact). }
procedure noteRange(var v: TV; lo, hi: int64);
begin
  if irIsConst(v) then
    exit;
  v.rKnown := true;
  v.rLo := lo;
  v.rHi := hi;
end;

{ The range of the signed x and y: with a non-negative operand, 0 up to it
  (optimization3 item 4). }
procedure andRange(var v: TV; const x, y: TV);
begin
  if x.rKnown and (x.rLo >= 0) and y.rKnown and (y.rLo >= 0) then
  begin
    if x.rHi < y.rHi then
      noteRange(v, 0, x.rHi)
    else
      noteRange(v, 0, y.rHi);
  end
  else if x.rKnown and (x.rLo >= 0) then
    noteRange(v, 0, x.rHi)
  else if y.rKnown and (y.rLo >= 0) then
    noteRange(v, 0, y.rHi);
end;

{ The range of the signed l div c, for a constant c > 0: l's range (or its
  type's) divided by c; division truncates, which is monotonic. }
procedure divRange(var v: TV; const l, r: TV);
var
  c, lo, hi: int64;
begin
  if (r.t^.bits > 64) or not irIsConstInt(r, c) or (c <= 0) then
    exit;
  if l.rKnown then
  begin
    lo := l.rLo;
    hi := l.rHi;
  end
  else if not signedTypeRange(l.t^.bits, lo, hi) then
    exit;
  noteRange(v, lo div c, hi div c);
end;

{ The range of the signed l mod c, for a constant c > 0: 0 .. c-1 for a
  non-negative l (and in ISO mode, where the result is never negative),
  else -(c-1) .. c-1 (Turbo/FPC: the sign of l). Within l's own range when
  that is smaller. }
procedure modRange(var v: TV; const l, r: TV);
var
  c, m, lo, hi: int64;
begin
  if (r.t^.bits > 64) or not irIsConstInt(r, c) or (c <= 0) then
    exit;
  m := c - 1;
  if mode = lmISO then
    noteRange(v, 0, m)
  else if l.rKnown and (l.rLo >= 0) then
  begin
    if l.rHi < m then
      noteRange(v, 0, l.rHi)
    else
      noteRange(v, 0, m);
  end
  else if l.rKnown then
  begin
    lo := l.rLo;
    if lo > 0 then lo := 0;
    if lo < -m then lo := -m;
    hi := l.rHi;
    if hi < 0 then hi := 0;
    if hi > m then hi := m;
    noteRange(v, lo, hi);
  end
  else
    noteRange(v, -m, m);
end;

function emitDiv(const l, r: TV; signed: boolean; loc: TLoc): TV;
var
  c: int64;
  isConst: boolean;
  a, b: TV;
begin
  isConst := irIsConstInt(r, c);
  if not isConst or (c = 0) then
    emitDivZeroCheck(irICmp('eq', r, irConstInt(r.t, 0), ''), loc);
  if not signed then
    exit(irBin('udiv', l, r, 'div'));
  { the minimum div -1 overflows (and traps on x86) }
  if not isConst or (c = -1) then
  begin
    a := irICmp('eq', l, signedMin(l.t), '');
    b := irICmp('eq', r, irConstInt(r.t, -1), '');
    emitOverflowCheck(irBin('and', a, b, ''), 'integer overflow in ''div''', loc);
  end;
  emitDiv := irBin('sdiv', l, r, 'div');
end;

function emitMod(const l, r: TV; signed: boolean; loc: TLoc): TV;
var
  c: int64;
  isConst: boolean;
  rem, fixed, neg, isMinus, safeR: TV;
begin
  isConst := irIsConstInt(r, c);
  if mode = lmISO then
  begin
    { ISO 7185 6.7.2.2: an error if j <= 0; the result is i - k*j with
      0 <= result < j }
    if not isConst or (c <= 0) then
      emitRuntimeCheck(irICmp('sle', r, irConstInt(r.t, 0), ''),
                       '''mod'' by zero or a negative number', loc);
    rem := irBin('srem', l, r, '');
    fixed := irBin('add', rem, r, '');
    neg := irICmp('slt', rem, irConstInt(rem.t, 0), '');
    exit(irSelect(neg, fixed, rem, 'mod'));
  end;
  { Turbo/FPC: the sign follows the dividend (like C %) }
  if not isConst or (c = 0) then
    emitDivZeroCheck(irICmp('eq', r, irConstInt(r.t, 0), ''), loc);
  if not signed then
    exit(irBin('urem', l, r, 'mod'));
  { x mod -1 = 0, but the minimum srem -1 traps on x86: divide by 1 }
  isMinus := irICmp('eq', r, irConstInt(r.t, -1), '');
  safeR := irSelect(isMinus, irConstInt(r.t, 1), r, '');
  emitMod := irBin('srem', l, safeR, 'mod');
end;

function compareInts(op: TBinaryOp; const l, r: TV; signed: boolean): TV;
var
  p: string;
begin
  case op of
    boEq: p := 'eq';
    boNe: p := 'ne';
    boLt: if signed then p := 'slt' else p := 'ult';
    boLe: if signed then p := 'sle' else p := 'ule';
    boGt: if signed then p := 'sgt' else p := 'ugt';
  else
    if signed then p := 'sge' else p := 'uge';
  end;
  compareInts := irICmp(p, l, r, 'cmp');
end;

function emitCompare(e: PNode): TV;
var
  op: TBinaryOp;
  lt, rt: PType;
  l, r, cmp, notX, masked: TV;
  wide: PType;
  setTy: PIRType;
  p: string;
begin
  op := e^.bop;
  lt := e^.lhs^.typ;
  rt := e^.rhs^.typ;
  if op = boIn then
    exit(emitIn(e));
  if lt^.kind = tySet then
  begin
    { compared in the larger of the two representations }
    if setStorageBits(rt) > setStorageBits(lt) then
      wide := rt
    else
      wide := lt;
    setTy := irInt(setStorageBits(wide));
    l := visitExpr(e^.lhs);
    l := convertSet(l, lt, wide);
    r := visitExpr(e^.rhs);
    r := convertSet(r, rt, wide);
    case op of
      boEq: exit(irICmp('eq', l, r, 'cmp'));
      boNe: exit(irICmp('ne', l, r, 'cmp'));
      boLe:
        begin
          { l is a subset of r }
          notX := irBin('xor', r, irConstInt(setTy, -1), '');
          masked := irBin('and', l, notX, '');
          exit(irICmp('eq', masked, irZero(setTy), 'subset'));
        end;
    else
      begin
        { l is a superset of r }
        notX := irBin('xor', l, irConstInt(setTy, -1), '');
        masked := irBin('and', r, notX, '');
        exit(irICmp('eq', masked, irZero(setTy), 'superset'));
      end;
    end;
  end;
  if needsStringCompare(lt, rt) and
     ((lt^.kind = tyAnsiString) or (rt^.kind = tyAnsiString)) then
  begin
    l := emitAnsi(e^.lhs);
    r := emitAnsi(e^.rhs);
    cmp := call2(classRuntime('__pas_astr_compare', 'i32', 'ptr,ptr'), l, r, '');
    exit(compareInts(op, cmp, i32c(0), true));
  end;
  if needsStringCompare(lt, rt) then
  begin
    { Turbo/FPC string comparison }
    l := emitPStr(e^.lhs);
    r := emitPStr(e^.rhs);
    cmp := call2(rtFn('__pas_str_compare'), l, r, '');
    exit(compareInts(op, cmp, i32c(0), true));
  end;
  if isMethodPointer(lt) or isMethodPointer(rt) then
  begin
    { the code only, as in Free Pascal }
    if e^.lhs^.kind <> nkNil then
      l := visitExpr(e^.lhs);
    if e^.rhs^.kind <> nkNil then
      r := visitExpr(e^.rhs);
    if e^.lhs^.kind = nkNil then
      l := irNull
    else
      l := irExtract(l, 0, irPtrT, '');
    if e^.rhs^.kind = nkNil then
      r := irNull
    else
      r := irExtract(r, 0, irPtrT, '');
    if op = boEq then
      exit(irICmp('eq', l, r, 'cmp'));
    exit(irICmp('ne', l, r, 'cmp'));
  end;
  if lt^.kind in [tyPointer, tyNil, tyClass, tyClassRef, tyInterface, tyDynArray] then
  begin { interfaces: their objects }
    l := interfaceObject(visitExpr(e^.lhs));
    r := interfaceObject(visitExpr(e^.rhs));
    case op of
      boEq: exit(irICmp('eq', l, r, 'cmp'));
      boNe: exit(irICmp('ne', l, r, 'cmp'));
      { pointers, by address (Turbo/FPC) }
      boLt: exit(irICmp('ult', l, r, 'cmp'));
      boLe: exit(irICmp('ule', l, r, 'cmp'));
      boGt: exit(irICmp('ugt', l, r, 'cmp'));
    else
      exit(irICmp('uge', l, r, 'cmp'));
    end;
  end;
  if isStringType(lt) then
  begin
    { equal-length strings compare lexicographically (unsigned chars) }
    l := emitAddress(e^.lhs);
    r := emitAddress(e^.rhs);
    cmp := call3(rtFn('memcmp'), l, r, i64c(getStringLength(lt)), '');
    exit(compareInts(op, cmp, i32c(0), true));
  end;
  if (lt^.kind = tyReal) or (rt^.kind = tyReal) then
  begin
    l := toDouble(visitExpr(e^.lhs), lt);
    r := toDouble(visitExpr(e^.rhs), rt);
    case op of
      boEq: p := 'oeq';
      boNe: p := 'une';
      boLt: p := 'olt';
      boLe: p := 'ole';
      boGt: p := 'ogt';
    else
      p := 'oge';
    end;
    exit(irFCmp(p, l, r, 'cmp'));
  end;
  l := visitExpr(e^.lhs);
  r := visitExpr(e^.rhs);
  if e^.operandType <> nil then
  begin
    { integers of possibly different types }
    l := convertOrdinal(l, lt, e^.operandType);
    r := convertOrdinal(r, rt, e^.operandType);
    exit(compareInts(op, l, r, isSignedOrdinal(e^.operandType)));
  end;
  emitCompare := compareInts(op, l, r, isSignedOrdinal(lt));
end;

{ --- Built-in functions --- }

function emitFileQuery(k: TBuiltinKind; fileArg: PNode): TV;
var
  f, r: TV;
begin
  if fileArg <> nil then
    f := loadFile(emitAddress(fileArg))
  else
    f := inputFile;
  if k = bkEof then
  begin
    r := call1(rtFn('__pas_eof'), f, '');
    exit(irICmp('ne', r, i32c(0), 'eof'));
  end;
  r := call1(rtFn('__pas_eoln'), f, '');
  emitFileQuery := irICmp('ne', r, i32c(0), 'eoln');
end;

{ T(x): ordinal conversions keep the low bits (extending by the source);
  pointer casts change only the type. }
function emitTypecast(e: PNode): TV;
var
  arg: PNode;
  v: TV;
begin
  arg := PNode(listGet(e^.args, 0));
  if isMethodPointer(e^.typ) and (arg^.typ^.kind = tyRecord) then { TEv(m) }
    exit(irLoad(memType(e^.typ), emitAddress(arg), ''));
  if (e^.typ^.kind = tyAnsiString) and (arg^.typ^.kind = tyPointer) and arg^.typ^.untyped then
    exit(visitExpr(arg)); { the pointer is an ansistring: borrowed }
  if e^.typ^.kind = tyAnsiString then
    exit(emitAnsi(arg));
  if e^.typ^.kind = tyShortString then
    exit(emitPStr(arg));
  v := visitExpr(arg);
  if arg^.typ^.kind = tyAnsiString then
  begin
    { pchar(s) is never nil }
    if not e^.typ^.untyped and (e^.typ^.pointee^.kind = tyChar) then
      exit(call1(classRuntime('__pas_astr_pchar', 'ptr', 'ptr'), v, ''));
    exit(v);
  end;
  if isOrdinal(e^.typ) then
    exit(convertOrdinal(v, arg^.typ, e^.typ));
  if e^.typ^.kind = tyInterface then
    exit(convertForAssign(v, arg^.typ, e^.typ, e^.b));
  emitTypecast := v;
end;

function emitRoundTrunc(e: PNode; const arg: TV; at: PType; isRound: boolean): TV;
var
  x, low, high, bad: TV;
  resT: PIRType;
  msg: TBuf;
begin
  if at^.kind <> tyReal then
    exit(convertOrdinal(arg, at, e^.typ));
  { ISO: round() rounds halves away from zero, like llvm.round; Turbo/FPC
    round halves to even }
  x := arg;
  if isRound then
  begin
    if mode = lmISO then
      x := unaryIntrinsic('round', arg, '')
    else
      x := unaryIntrinsic('roundeven', arg, '');
  end;
  resT := regType(e^.typ);
  if rangeChecks(e^.b) then
  begin
    { the result must lie in the integer (or int64) range; the bounds are
      exact doubles. NaN fails too (unordered comparisons are true). }
    if resT^.bits = 64 then
      low := irFCmp('ult', x, irConstDouble(-9223372036854775808.0), '')
    else
      low := irFCmp('ule', x, irConstDouble(-2147483649.0), '');
    if resT^.bits = 64 then
      high := irFCmp('uge', x, irConstDouble(9223372036854775808.0), '')
    else
      high := irFCmp('uge', x, irConstDouble(2147483648.0), '');
    bad := irBin('or', low, high, '');
    bufInit(msg);
    if isRound then
      bufAdd(msg, '''round''')
    else
      bufAdd(msg, '''trunc''');
    bufAdd(msg, ' result does not fit in ');
    bufAddTypeName(msg, e^.typ);
    emitRuntimeCheckB(bad, msg, e^.b);
    bufFree(msg);
  end;
  if isRound then
    emitRoundTrunc := irCast('fptosi', x, resT, 'round')
  else
    emitRoundTrunc := irCast('fptosi', x, resT, 'trunc');
end;

function emitBuiltinCall(e: PNode; k: TBuiltinKind): TV;
var
  opName: string;
  pairFields: TPtrList;
  argE: PNode;
  at, t: PType;
  arg, tmp, p, x, one, bad, addrV: TV;
  v: int64;
  bd: TOrdinalBounds;
  loc: TLoc;
  isSucc: boolean;
  r: PIRType;
  f: PIRFunc;
begin
  case k of
    bkWait:
      exit(emitAwait(visitExpr(PNode(listGet(e^.args, 0))),
                     PNode(listGet(e^.args, 0))^.typ));
    bkWhenAll, bkWhenAny:
      exit(emitWhen(e, k = bkWhenAll));
    bkIteratorNext:
      exit(emitIteratorNext(visitExpr(PNode(listGet(e^.args, 0)))));
    bkIteratorValue: { (read as a variable's value) }
      begin
        argE := PNode(listGet(e^.args, 0));
        addrV := iteratorValueAddr(visitExpr(argE));
        if isStructured(e^.typ) then
          exit(addrV);
        exit(loadScalar(addrV, e^.typ, 'value'));
      end;
    bkAddChecked, bkSubChecked, bkMulChecked: { the tuple (a op b, no overflow) }
      begin
        r := memType(e^.typ);
        t := PDecl(listGet(e^.typ^.allFields, 0))^.fieldType;
        argE := PNode(listGet(e^.args, 0));
        arg := convertOrdinal(visitExpr(argE), argE^.typ, t);
        argE := PNode(listGet(e^.args, 1));
        x := convertOrdinal(visitExpr(argE), argE^.typ, t);
        if k = bkAddChecked then
          opName := 'add'
        else if k = bkSubChecked then
          opName := 'sub'
        else
          opName := 'mul';
        if isSignedOrdinal(t) then
          opName := 'llvm.s' + opName + '.with.overflow.' + arg.t^.text
        else
          opName := 'llvm.u' + opName + '.with.overflow.' + arg.t^.text;
        listInit(pairFields);
        listAdd(pairFields, arg.t);
        listAdd(pairFields, irI1);
        f := intrinsic(opName, irStructLit(pairFields), arg.t, arg.t, speculatable, '');
        listFree(pairFields);
        tmp := call2(f, arg, x, '');
        p := irCreateTemp(r, 'checked');
        storeScalar(fieldAddress(p, e^.typ, PDecl(listGet(e^.typ^.allFields, 0))),
                    irExtract(tmp, 0, arg.t, 'value'), t);
        storeScalar(fieldAddress(p, e^.typ, PDecl(listGet(e^.typ^.allFields, 1))),
                    irBin('xor', irExtract(tmp, 1, irI1, ''), irConstBool(true), 'ok'),
                    PDecl(listGet(e^.typ^.allFields, 1))^.fieldType);
        exit(p);
      end;
    bkSizeOf: { the argument is not evaluated }
      begin
        t := PNode(listGet(e^.args, 0))^.typ;
        if t^.kind = tyOpenArray then
          exit(irBin('mul', openCount(openDescriptor(PNode(listGet(e^.args, 0)))),
                     i64c(sizeOf_(t^.openElement)), ''));
        exit(i64c(sizeOf_(t)));
      end;
    bkLow, bkHigh:
      begin
        t := PNode(listGet(e^.args, 0))^.typ;
        if t^.kind = tyDynArray then
        begin
          if k = bkLow then
            exit(i32c(0));
          exit(irBin('sub', irCast('trunc', dynLength(visitExpr(PNode(listGet(e^.args, 0)))),
                                   irI32, ''), i32c(1), ''));
        end;
        if t^.kind = tyOpenArray then
        begin
          if k = bkLow then
            exit(i32c(0));
          exit(irBin('sub', irCast('trunc', openCount(openDescriptor(PNode(listGet(e^.args, 0)))),
                                   irI32, ''), i32c(1), ''));
        end;
        if t^.kind = tyArrayOfConst then
        begin
          if k = bkLow then
            exit(i32c(0));
          exit(irBin('sub', irCast('trunc', irLoad(irI64, visitExpr(PNode(listGet(e^.args, 0))), ''),
                                   irI32, ''), i32c(1), ''));
        end;
        if t^.kind = tyShortString then
        begin
          if k = bkHigh then
            v := t^.maxLength
          else
            v := 0;
        end
        else
        begin
          if t^.kind = tyArray then
            t := t^.indexType;
          bd := getOrdinalBounds(t);
          if (k = bkHigh) and isQWord(t) then
            v := -1 { 2^64-1 }
          else if k = bkHigh then
            v := bd.hi
          else
            v := bd.lo;
        end;
        exit(irConstInt(regType(e^.typ), v));
      end;
    bkAssigned:
      begin
        argE := PNode(listGet(e^.args, 0));
        if isMethodPointer(argE^.typ) or isClosureType(argE^.typ) then
          p := irExtract(visitExpr(argE), 0, irPtrT, '')
        else if argE^.typ^.kind = tyRoutine then
        begin
          tmp := irLoad(memType(argE^.typ), emitAddress(argE), '');
          p := irExtract(tmp, 0, irPtrT, '');
        end
        else
          p := interfaceObject(visitExpr(argE));
        exit(irICmp('ne', p, irNull, 'assigned'));
      end;
    bkParamCount:
      exit(call0(rtFn('__pas_paramcount'), 'paramcount'));
    bkExceptAddr:
      exit(exceptAddr);
    bkParamStr:
      begin
        tmp := newStringTemp(255);
        x := emitI32(PNode(listGet(e^.args, 0)));
        rtv2('__pas_paramstr', tmp, x);
        exit(tmp);
      end;
    bkLength, bkCopy, bkPos, bkConcat, bkUpCase:
      exit(emitStringFunction(e, k));
    bkEof, bkEoln:
      exit(emitFileQuery(k, PNode(listGet(e^.args, 0))));
    bkFilePos:
        exit(call1(classRuntime('__pas_filepos', 'i64', 'ptr'),
                   loadFile(emitAddress(PNode(listGet(e^.args, 0)))), 'filepos'));
    bkFileSize:
        exit(call1(classRuntime('__pas_filesize', 'i64', 'ptr'),
                   loadFile(emitAddress(PNode(listGet(e^.args, 0)))), 'filesize'));
    bkGetFileHandle:
        exit(call1(classRuntime('__pas_file_handle', 'i32', 'ptr'),
                   loadFile(emitAddress(PNode(listGet(e^.args, 0)))), 'handle'));
  else
    ;
  end;

  argE := PNode(listGet(e^.args, 0));
  at := argE^.typ;
  arg := visitExpr(argE);
  loc := e^.b;
  case k of
    bkAbs:
      begin
        if at^.kind = tyReal then
          exit(unaryIntrinsic('fabs', arg, 'abs'));
        arg := convertOrdinal(arg, at, e^.typ);
        if not isSignedOrdinal(e^.typ) then
          exit(arg);
        if overflowChecks(loc) then
          emitOverflowCheck(irICmp('eq', arg, signedMin(arg.t), ''),
                            'integer overflow in ''abs''', loc);
        r := arg.t;
        f := intrinsic('llvm.abs.' + r^.text, r, r, irI1, speculatablePlain,
                       r^.text + ', i1 immarg');
        exit(call2(f, arg, irConstBool(false), 'abs'));
      end;
    bkSqr:
      begin
        if at^.kind = tyReal then
          exit(irBin('fmul', arg, arg, 'sqr'));
        arg := convertOrdinal(arg, at, e^.typ);
        exit(emitCheckedArith('mul', isSignedOrdinal(e^.typ), arg, arg, loc, 'sqr'));
      end;
    bkOdd:
      exit(irCast('trunc', arg, irI1, 'odd'));
    bkOrd:
      exit(convertOrdinal(arg, at, e^.typ));
    bkChr:
      begin
        emitRangeCheckMsg(arg, at, e^.typ, loc, '''chr'' argument out of range');
        exit(irCast('trunc', arg, irI8, 'chr'));
      end;
    bkSucc, bkPred:
      begin
        isSucc := k = bkSucc;
        t := getBase(at);
        bd := getOrdinalBounds(t);
        if isQWord(t) then
          bd.hi := -1; { 2^64-1 }
        if rangeChecks(loc) then
        begin
          x := toI64(arg, t);
          if isSucc then
          begin
            bad := irICmp('eq', x, i64c(bd.hi), '');
            emitRuntimeCheck(bad, '''succ'' of the last value of its type', loc);
          end
          else
          begin
            bad := irICmp('eq', x, i64c(bd.lo), '');
            emitRuntimeCheck(bad, '''pred'' of the first value of its type', loc);
          end;
        end;
        one := irConstInt(arg.t, 1);
        if isSucc then
          exit(irBin('add', arg, one, 'succ'));
        exit(irBin('sub', arg, one, 'pred'));
      end;
    bkSin: exit(unaryIntrinsic('sin', toDouble(arg, at), ''));
    bkCos: exit(unaryIntrinsic('cos', toDouble(arg, at), ''));
    bkExp: exit(unaryIntrinsic('exp', toDouble(arg, at), ''));
    bkArctan: exit(call1(rtFn('atan'), toDouble(arg, at), ''));
    bkLn:
      begin
        x := toDouble(arg, at);
        if rangeChecks(loc) then
          emitRuntimeCheck(irFCmp('ule', x, irConstDouble(0.0), ''),
                           '''ln'' of zero or a negative number', loc);
        exit(unaryIntrinsic('log', x, ''));
      end;
    bkSqrt:
      begin
        x := toDouble(arg, at);
        if rangeChecks(loc) then
          emitRuntimeCheck(irFCmp('ult', x, irConstDouble(0.0), ''),
                           '''sqrt'' of a negative number', loc);
        exit(unaryIntrinsic('sqrt', x, ''));
      end;
    bkTrunc, bkRound:
      exit(emitRoundTrunc(e, arg, at, k = bkRound));
  else
    emitBuiltinCall := arg;
  end;
end;

{ --- Expressions (scalar values in register representation) --- }

function visitNameRef(e: PNode): TV;
var
  d: PDecl;
  v: TV;
  noArgs: TPtrList;
begin
  d := e^.refDecl;
  if (d^.kind = dkProperty) and d^.propIsClass then
    exit(emitPropertyRead(d, irNull, nil));
  if d^.kind = dkProperty then
    exit(emitPropertyRead(d, withBase(e^.refWith), e^.refWith^.rec^.typ));
  if (d^.kind = dkRoutine) and d^.isStatic then
  begin
    listInit(noArgs);
    exit(emitCall(d, noArgs)); { a record's static method }
  end;
  if (d^.kind = dkRoutine) and (d^.methodKind <> mtNone) then
  begin
    listInit(noArgs);
    exit(emitMethodCallC(d, withBase(e^.refWith), noArgs, false, e^.b,
                         withIsClass(e^.refWith)));
  end;
  if d^.kind = dkType then { a class as a value }
    exit(vmtOf(d^.declType));
  if (e^.typ^.kind = tyShortString) and (d^.kind in [dkVar, dkField]) then
    exit(emitAddress(e));
  if (d^.kind = dkVar) and (d^.varType <> nil) and (d^.varType^.kind = tyRoutine) then
  begin
    if e^.typ^.kind = tyRoutine then { the value itself (Turbo/FPC) }
      exit(irLoad(memType(d^.varType), varAddress(d), ''));
    listInit(noArgs);
    exit(emitIndirectCall(d, noArgs)); { a parameterless functional parameter }
  end;
  if isBitPackedComponent(e) then
    exit(bitLoad(e));
  if d^.kind in [dkVar, dkField] then
  begin
    v := loadScalar(emitAddress(e), e^.typ, nm(e^.ref.name));
    if d^.cgRangeKnown then
    begin
      v.rKnown := true;
      v.rLo := d^.cgRangeLo;
      v.rHi := d^.cgRangeHi;
    end;
    visitNameRef := v;
    exit;
  end;
  if d^.kind = dkRoutine then
  begin
    listInit(noArgs);
    exit(emitCall(d, noArgs)); { a parameterless function }
  end;
  if d^.kind = dkBuiltin then
  begin
    if d^.builtin = bkParamCount then
      exit(call0(rtFn('__pas_paramcount'), 'paramcount'));
    if d^.builtin = bkExceptAddr then
      exit(exceptAddr);
    exit(emitFileQuery(d^.builtin, nil)); { eof / eoln }
  end;
  if d^.constValue.kind = ckReal then
    exit(irConstDouble(d^.constValue.realVal));
  visitNameRef := irConstInt(regType(d^.constType), d^.constValue.intVal);
end;

function visitUnary(e: PNode): TV;
var
  v: TV;
  t: PType;
  args: TPtrList;
begin
  if e^.uopDecl <> nil then { a record's class operator }
  begin
    listInit(args);
    listAdd(args, e^.operand);
    visitUnary := emitCall(e^.uopDecl, args);
    listFree(args);
    exit;
  end;
  v := visitExpr(e^.operand);
  t := e^.typ;
  if isIntegerLike(t) then { a promoted operand (byte -> integer, ...) }
    v := convertOrdinal(v, e^.operand^.typ, t);
  case e^.uop of
    uoPlus: visitUnary := v;
    uoMinus:
      if t^.kind = tyReal then
        visitUnary := irFNeg(v, 'neg')
      else
        visitUnary := emitCheckedArith('sub', isSignedOrdinal(t), irConstInt(v.t, 0),
                                       v, e^.b, 'neg');
  else
    visitUnary := irBin('xor', v, irConstInt(v.t, -1), 'not');
  end;
end;

{ The elementwise operators of vectors (LB-6): a scalar operand is
  broadcast to the lanes (LLVM's CreateVectorSplat, whose constant
  splats fold to the operand); the lanes' arithmetic carries no checks. }
function vectorBinary(e: PNode; op: TBinaryOp; const l: TV; lt: PType;
                      const r: TV; rt: PType): TV;
var
  vec, lane: PIRType;
  l2, r2: TV;
  laneReal: boolean;

  { A splat in the form LLVM's IRBuilder gives: constants fold (the
    operand), values get the two instructions, named splat. }
  function broadcast(const v: TV; t: PType): TV;
  var
    sv, inserted: TV;
  begin
    if t^.kind = tyVector then
      exit(v);
    if laneReal then
      sv := toDouble(v, t)
    else
      sv := convertOrdinal(v, t, e^.typ^.vecElement);
    if irIsConst(sv) then
      broadcast := irV(lane, 'splat (' + sv.t^.text + ' ' + sv.v + ')')
    else
    begin
      inserted := irSplatInsert(vec, sv, 'splat.splatinsert');
      broadcast := irSplatShuffle(vec, inserted, 'splat.splat');
    end;
  end;

begin
  vec := memType(e^.typ);
  lane := vec^.elem;
  laneReal := e^.typ^.vecElement^.kind = tyReal;
  l2 := broadcast(l, lt);
  r2 := broadcast(r, rt);
  case op of
    boAdd:
      if laneReal then
        vectorBinary := irBin('fadd', l2, r2, 've.add')
      else
        vectorBinary := irBin('add', l2, r2, 've.add');
    boSub:
      if laneReal then
        vectorBinary := irBin('fsub', l2, r2, 've.sub')
      else
        vectorBinary := irBin('sub', l2, r2, 've.sub');
  else
    if laneReal then
      vectorBinary := irBin('fmul', l2, r2, 've.mul')
    else
      vectorBinary := irBin('mul', l2, r2, 've.mul');
  end;
end;

function visitBinary(e: PNode): TV;
var
  op: TBinaryOp;
  lt, rt, opTy: PType;
  lhs: PNode;
  l, r, m, n32, w, tmp, notR, masked: TV;
  signed: boolean;
  loc: TLoc;
  args: TPtrList;
  v, res: TV;
begin
  op := e^.bop;
  if e^.opDecl <> nil then { a record's class operator }
  begin
    listInit(args);
    listAdd(args, e^.lhs);
    listAdd(args, e^.rhs);
    v := emitCall(e^.opDecl, args);
    listFree(args);
    if e^.opNegated then
      v := irBin('xor', v, irConstInt(v.t, -1), 'ne');
    exit(v);
  end;
  if op in [boIs, boAs] then
    exit(emitIsAs(e));
  if (op in [boAnd, boOr]) and (e^.typ^.kind = tyBoolean) then
    exit(emitShortCircuit(e));
  if e^.typ^.kind = tyAnsiString then
  begin
    { (a + b) + c, the characters of all three joined in one allocation
      (item 7 of optimization3.md): __pas_astr_join reads its pieces
      before the fresh buffer is written, so no piece can be it. The
      middle piece may be a character: an own byte, instead of a fresh
      string. }
    lhs := e^.lhs;
    if (lhs^.kind = nkBinary) and (lhs^.bop = boAdd) and
       ((lhs^.typ^.kind = tyAnsiString) and
        ((lhs^.rhs^.typ^.kind = tyAnsiString) or isCharLike(lhs^.rhs^.typ))) then
    begin
      l := emitAnsi(lhs^.lhs);
      if lhs^.rhs^.typ^.kind = tyAnsiString then
      begin
        m := emitAnsi(lhs^.rhs);
        n32 := call1(classRuntime('__pas_astr_length', 'i32', 'ptr'), m, '');
        w := irCast('zext', n32, irI64, '');
      end
      else
      begin
        m := emitCharPiece(lhs^.rhs);
        w := i64c(1);
      end;
      r := emitAnsi(e^.rhs);
      exit(holdResult(call4(classRuntime('__pas_astr_join', 'ptr',
                                          'ptr,ptr,i64,ptr'),
                            l, m, w, r, '')));
    end;
    { c + s and s + c, with the character on its own byte and the string's
      or the empty end's characters as the nil side of the join (item 7) }
    if isCharLike(e^.lhs^.typ) and (e^.rhs^.typ^.kind = tyAnsiString) then
    begin
      l := emitCharPiece(e^.lhs);
      r := emitAnsi(e^.rhs);
      exit(holdResult(call4(classRuntime('__pas_astr_join', 'ptr',
                                          'ptr,ptr,i64,ptr'),
                            irZero(irPtrT), l, i64c(1), r, '')));
    end;
    if (e^.lhs^.typ^.kind = tyAnsiString) and isCharLike(e^.rhs^.typ) then
    begin
      l := emitAnsi(e^.lhs);
      r := emitCharPiece(e^.rhs);
      exit(holdResult(call4(classRuntime('__pas_astr_join', 'ptr',
                                          'ptr,ptr,i64,ptr'),
                            l, r, i64c(1), irZero(irPtrT), '')));
    end;
    l := emitAnsi(e^.lhs);
    r := emitAnsi(e^.rhs);
    exit(holdResult(call2(classRuntime('__pas_astr_concat', 'ptr', 'ptr,ptr'), l, r, '')));
  end;
  if e^.typ^.kind = tyShortString then
  begin
    { Turbo/FPC concatenation }
    tmp := newStringTemp(255);
    l := emitPStr(e^.lhs);
    r := emitPStr(e^.rhs);
    rtv3('__pas_str_concat', tmp, l, r);
    exit(tmp);
  end;
  if isRelational(op) then
    exit(emitCompare(e));

  lt := e^.lhs^.typ;
  rt := e^.rhs^.typ;
  l := visitExpr(e^.lhs);
  r := visitExpr(e^.rhs);

  { Turbo/FPC pointer arithmetic, in units of what is pointed to }
  if e^.typ^.kind = tyPointer then
  begin
    if lt^.kind <> tyPointer then { n + p }
    begin
      tmp := toI64(l, lt);
      exit(irGEP1(pointeeMemType(e^.typ), r, tmp, false, 'ptr.add'));
    end;
    tmp := toI64(r, rt);
    if op = boSub then
      tmp := irBin('sub', i64c(0), tmp, '');
    exit(irGEP1(pointeeMemType(e^.typ), l, tmp, false, 'ptr.add'));
  end;
  if lt^.kind = tyPointer then { p - q }
  begin
    l := irCast('ptrtoint', l, irI64, '');
    r := irCast('ptrtoint', r, irI64, '');
    v := irBin('sub', l, r, 'ptr.diff');
    if pointeeMemType(lt)^.size > 1 then
      v := irBin('sdiv', v, i64c(pointeeMemType(lt)^.size), 'ptr.count');
    exit(v);
  end;

  if e^.typ^.kind = tySet then
  begin
    { (the result has the larger of the two representations) }
    l := convertSet(l, lt, e^.typ);
    r := convertSet(r, rt, e^.typ);
    case op of
      boAdd: exit(irBin('or', l, r, 'union'));
      boMul: exit(irBin('and', l, r, 'intersection'));
    else
      begin
        notR := irBin('xor', r, irConstInt(irInt(setStorageBits(e^.typ)), -1), '');
        exit(irBin('and', l, notR, 'difference'));
      end;
    end;
  end;

  if e^.typ^.kind = tyReal then
  begin
    l := toDouble(l, lt);
    r := toDouble(r, rt);
    case op of
      boAdd: exit(irBin('fadd', l, r, 'add'));
      boSub: exit(irBin('fsub', l, r, 'sub'));
      boMul: exit(irBin('fmul', l, r, 'mul'));
    else
      begin
        if rangeChecks(e^.opLoc) then
          emitRuntimeCheck(irFCmp('oeq', r, irConstDouble(0.0), ''),
                           'division by zero', e^.opLoc);
        exit(irBin('fdiv', l, r, 'div'));
      end;
    end;
  end;

  { vector[N] of T (llvm-23.md LB-6): elementwise, without the per-lane
    overflow checks (LLVM's vectors have no checked add/sub/mul) }
  if e^.typ^.kind = tyVector then
    exit(vectorBinary(e, op, l, lt, r, rt));

  loc := e^.opLoc;
  { integer operators: both operands in the operand type chosen by Sema }
  signed := true;
  opTy := e^.operandType;
  if opTy <> nil then
  begin
    l := convertOrdinal(l, lt, opTy);
    r := convertOrdinal(r, rt, opTy);
    signed := isSignedOrdinal(opTy);
  end;
  case op of
    { Turbo/FPC bitwise operators (booleans: xor only) }
    boAnd:
      begin
        res := irBin('and', l, r, 'and');
        if signed then
          andRange(res, l, r);
        visitBinary := res;
      end;
    boOr: visitBinary := irBin('or', l, r, 'or');
    boXor: visitBinary := irBin('xor', l, r, 'xor');
    boShl:
      begin
        masked := irBin('and', r, irConstInt(r.t, l.t^.bits - 1), '');
        visitBinary := irBin('shl', l, masked, 'shl');
      end;
    boShr:
      begin
        { logical, like Turbo/FPC }
        masked := irBin('and', r, irConstInt(r.t, l.t^.bits - 1), '');
        visitBinary := irBin('lshr', l, masked, 'shr');
      end;
    boAdd: visitBinary := emitCheckedArith('add', signed, l, r, loc, 'add');
    boSub: visitBinary := emitCheckedArith('sub', signed, l, r, loc, 'sub');
    boMul: visitBinary := emitCheckedArith('mul', signed, l, r, loc, 'mul');
    boIntDiv:
      begin
        res := emitDiv(l, r, signed, loc);
        if signed then
          divRange(res, l, r);
        visitBinary := res;
      end;
    boMod:
      begin
        res := emitMod(l, r, signed, loc);
        if signed then
          modRange(res, l, r);
        visitBinary := res;
      end;
  else
    visitBinary := l;
  end;
end;

function visitCall(e: PNode): TV;
var
  d: PDecl;
begin
  if e^.methodCast <> 0 then
    exit(emitAddress(e));
  if e^.callValue <> nil then { a procedural field or property }
    exit(emitCallThrough(visitExpr(e^.callValue), e^.callValue^.typ, e^.args, e^.b));
  if e^.isVarCast then { T(x) of an untyped parameter x }
  begin
    if e^.typ^.kind = tyShortString then
      exit(emitAddress(e));
    exit(loadScalar(emitAddress(e), e^.typ, ''));
  end;
  d := e^.callDecl;
  case d^.kind of
    dkType: visitCall := emitTypecast(e);
    dkRoutine:
      if d^.isStatic then { a record's static method }
        visitCall := emitCall(d, e^.args)
      else if d^.methodKind <> mtNone then
        visitCall := emitMethodCallC(d, withBase(e^.callWith), e^.args, false, e^.b,
                                     withIsClass(e^.callWith))
      else
        visitCall := emitCall(d, e^.args);
    dkVar: visitCall := emitIndirectCall(d, e^.args);
  else
    visitCall := emitBuiltinCall(e, d^.builtin);
  end;
end;

function visitIndirectCall(e: PNode): TV;
var
  callee: PNode;
  pair: TV;
begin
  callee := e^.calleeExpr;
  if callee^.kind = nkInherited then
    exit(emitInheritedCall(callee, e^.args));
  if (callee^.kind = nkField) and (callee^.fieldDecl <> nil) and
     (callee^.fieldDecl^.kind = dkRoutine) then
    exit(emitMemberCall(callee, e^.args));
  if callee^.typ^.routineOfObject or callee^.typ^.routineReference then
    pair := visitExpr(callee) { (a closure: also a call's result) }
  else
    pair := irLoad(memType(callee^.typ), emitAddress(callee), '');
  visitIndirectCall := emitCallThrough(pair, callee^.typ, e^.args, callee^.b);
end;

{ s[i] of an ansistring, read: no copy }
function emitAnsiIndexRead(e: PNode): TV;
var
  s, x, offset: TV;
begin
  s := visitExpr(e^.base);
  x := toI64(visitExpr(e^.index), e^.index^.typ);
  emitAnsiIndexCheck(s, x, e^.index^.b);
  offset := irBin('sub', x, i64c(1), '');
  emitAnsiIndexRead := loadScalar(irGEP1(irI8, s, offset, true, '',
                            indexFlags(e^.index, 1)), e^.typ, '');
end;

{ A method-pointer lambda's value: (code, object), the object the Self
  where it is made. }
function emitMethodLambda(l: PDecl): TV;
var
  pair, obj: TV;
begin
  pair := irInsert(irUndef(routinePairT), irFuncRef(fnOf(l)), 0, '');
  if l^.lambdaSelf <> nil then
    obj := loadScalar(varAddress(l^.lambdaSelf), l^.lambdaSelf^.varType, 'obj')
  else
    obj := irNull;
  emitMethodLambda := irInsert(pair, obj, 1, '');
end;

{ @obj.M, @TFoo.M, @M: (method's code, object or class). }
function emitMethodPointer(a: PNode): TV;
var
  r: PDecl;
  op, base: PNode;
  obj, code, pair: TV;
  selfIsClass: boolean;
  loc: TLoc;
begin
  r := a^.addrMethod;
  loc := a^.b;
  op := a^.inner;
  selfIsClass := false;
  if op^.kind = nkField then
  begin
    base := op^.base;
    if base^.typ^.kind = tyRecord then
      obj := emitAddress(base)
    else if namesType(base) then
    begin
      obj := vmtOf(base^.typ);
      selfIsClass := true;
    end
    else
    begin
      obj := visitExpr(base);
      selfIsClass := base^.typ^.kind = tyClassRef;
    end;
  end
  else
  begin
    obj := withBase(op^.refWith);
    selfIsClass := withIsClass(op^.refWith);
  end;
  code := irFuncRef(fnOf(r));
  if r^.isClassMethod then
  begin
    obj := receiverClass(obj, selfIsClass, loc);
    if r^.vmtIndex >= 0 then
      code := loadVMTSlot(obj, 6 + r^.vmtIndex, 'method');
  end
  else if r^.vmtIndex >= 0 then
  begin
    emitNilObjectCheck(obj, loc);
    code := loadVirtual(obj, r^.vmtIndex);
  end;
  pair := irInsert(irUndef(routinePairT), code, 0, '');
  emitMethodPointer := irInsert(pair, obj, 1, '');
end;

{ [a, b, c] as a new dynamic array of type t. }
function emitDynArrayCtor(s: PNode; t: PType): TV;
var
  i: longint;
  elemT: PType;
  info, data, slot, cs: TV;
  e: PNode;
begin
  if s^.setElems.count = 0 then
    exit(irNull);
  elemT := t^.dynElement;
  info := dynInfoOf(t);
  data := holdDynResult(call2(classRuntime('__pas_dyn_new', 'ptr', 'ptr,i64'), info,
                              i64c(s^.setElems.count), ''), t);
  for i := 0 to s^.setElems.count - 1 do
  begin
    e := PSetElement(listGet(s^.setElems, i))^.lo;
    slot := irGEP1(memType(elemT), data, i64c(i), true, '');
    if elemT^.kind = tyShortString then
      emitStringAssign(slot, elemT, e)
    else if elemT^.kind = tyAnsiString then
      storeValue(slot, emitAnsi(e), elemT)
    else if isStructured(elemT) then
      copyValue(slot, emitAddress(e), elemT)
    else if cStringFor(elemT, e, cs) then
      irStore(cs, slot)
    else
      storeValue(slot, convertForAssign(visitExpr(e), e^.typ, elemT, e^.b), elemT);
  end;
  emitDynArrayCtor := data;
end;

{ move p: p's object; p is nil afterwards. }
function emitMove(e: PNode): TV;
var
  slot, obj: TV;
begin
  slot := emitAddress(e^.inner);
  obj := irLoad(irPtrT, slot, 'moved');
  irStore(irNull, slot);
  if e^.typ^.kind = tyDynArray then { (its reference, held to the statement's end) }
    exit(holdDynResult(obj, e^.typ));
  emitMove := obj;
end;

{ The text part p of a template string as an ansistring constant; null
  when it is empty. }
function templateText(p: PNode): TV;
begin
  if p^.strValue^.len = 0 then
    templateText := irNull
  else
    templateText := getAnsiConstant(p^.strValue^.chars, 0, p^.strValue^.len);
end;

{ value, assignable to type t, stored at addr (a variable of type t). }
procedure storeExpr(const addr: TV; t: PType; value: PNode);
var
  pair, v: TV;
begin
  if t^.kind = tyShortString then
  begin
    emitStringAssign(addr, t, value);
    exit;
  end;
  if t^.kind = tyAnsiString then
  begin
    storeValue(addr, emitAnsi(value), t);
    exit;
  end;
  if isStructured(t) then
  begin
    copyValue(addr, emitAddress(value), t);
    exit;
  end;
  if t^.kind = tyRoutine then
  begin
    pair := emitRoutinePair(value, t);
    storeValue(addr, pair, t); { (a closure: counted) }
    exit;
  end;
  v := convertForAssign(visitExpr(value), value^.typ, t, value^.b);
  storeValue(addr, v, t);
end;

{ (a, b): its hidden variable, each element assigned its value. }
function emitTuple(e: PNode): TV;
var
  addr, fa: TV;
  r: PType;
  f: PDecl;
  i: longint;
begin
  r := e^.typ;
  addr := varAddress(e^.tupTemp);
  for i := 0 to e^.tupElems.count - 1 do
  begin
    f := PDecl(listGet(r^.allFields, i));
    fa := fieldAddress(addr, r, f);
    storeExpr(fa, f^.fieldType, PNode(listGet(e^.tupElems, i)));
  end;
  emitTuple := addr;
end;

{ A template string: each value is written, as write writes it, to the
  runtime's text file that collects characters (__pas_tmpl_sink);
  __pas_tmpl_add then makes the string so far, what was collected and the
  text that follows into one string. (Nothing but the write itself runs
  between the two, so a template inside a value is done by then.) }
function emitTemplate(e: PNode): TV;
var
  r, f, t: TV;
  i: longint;
begin
  r := templateText(PNode(listGet(e^.tmplParts, 0)));
  i := 1;
  while i < e^.tmplParts.count do
  begin
    f := call0(classRuntime('__pas_tmpl_sink', 'ptr', ''), '');
    emitWriteArg(f, PNode(listGet(e^.tmplParts, i)));
    t := templateText(PNode(listGet(e^.tmplParts, i + 1)));
    r := holdResult(call2(classRuntime('__pas_tmpl_add', 'ptr', 'ptr,ptr'), r, t, ''));
    i := i + 2;
  end;
  emitTemplate := r;
end;

{ --- Tasks (objfpc) --- }

{ The parameters of r from its task's arguments: their slot index. }
function firstParamSlot(r: PDecl): longint;
begin
  if hasSelf(r) then
    firstParamSlot := 2
  else
    firstParamSlot := 1;
end;

{ An async routine's task: its arguments' record [link, Self, params]
  (copies), its entry R.task(task) (calls R with them, stores the result
  in the task) and R.taskfini(args) (finalizes them). name: r's
  function's; external: r is exported or imported (declared: imported),
  and so are its task's functions. }
procedure declareAsync(r: PDecl; const name: string; external_, declared: boolean);
var
  slots, params: TPtrList;
  counted: boolean;
  i: longint;
  p: PDecl;
  linkage, entryName, finiName, argName: string;
  f: PIRFunc;
begin
  listInit(slots);
  listAdd(slots, irPtrT); { the link }
  if hasSelf(r) then
    listAdd(slots, irPtrT);
  counted := false;
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    listAdd(slots, memType(p^.varType));
    if isManaged(p^.varType) then
      counted := true;
  end;
  r^.cgArgsTy := irStructNamed('args.' + copy(name, 5, 255), slots);
  listFree(slots);
  if external_ then
    linkage := ''
  else
    linkage := 'internal';
  listInit(params);
  listAdd(params, irPtrT);
  if r^.iterType <> nil then
  begin
    entryName := name + '.gen';
    finiName := name + '.genfini';
    argName := 'gen';
  end
  else
  begin
    entryName := name + '.task';
    finiName := name + '.taskfini';
    argName := 'task';
  end;
  if declared then
    f := irDeclareFunction(entryName, irVoidT, params, '', '')
  else
    f := irCreateFunction(entryName, linkage, irVoidT, params);
  irSetArgName(f, 0, argName);
  r^.cgTaskEntry := f;
  r^.cgTaskFini := nil;
  if counted then
  begin
    if declared then
      f := irDeclareFunction(finiName, irVoidT, params, '', '')
    else
      f := irCreateFunction(finiName, linkage, irVoidT, params);
    irSetArgName(f, 0, 'args');
    r^.cgTaskFini := f;
  end;
  listFree(params);
end;

procedure emitAsyncParts(r: PDecl);
var
  argsTy: PIRType;
  entry, fini: PIRFunc;
  task, args, resultSlot, addr, res: TV;
  buf: TBuf;
  byPointer: boolean;
  slot, i: longint;
  p: PDecl;
  t: PType;
begin
  argsTy := PIRType(r^.cgArgsTy);
  entry := PIRFunc(r^.cgTaskEntry);
  fini := PIRFunc(r^.cgTaskFini);
  { R.task(task): R(args...), its result into the task's }
  curFn := entry;
  resetAddrAligns;
  curRoutine := nil;
  curFrame := irNull;
  pinTemps := true;
  curSP := nil;
  irBeginBody(entry);
  irSetDebugLoc(nil);
  irSetInsertPoint(irNewBlock('entry'));
  task := irArg(entry, 0);
  if r^.iterType <> nil then
    args := call1(classRuntime('__pas_gen_args', 'ptr', 'ptr'), task, 'args')
  else
    args := call1(classRuntime('__pas_task_args', 'ptr', 'ptr'), task, 'args');
  bufInit(buf);
  irArgsAdd(buf, irLoad(irPtrT, irStructGEP(argsTy, args, 0, ''), 'link'));
  resultSlot := irNull;
  if r^.isFunction then
    resultSlot := call1(classRuntime('__pas_task_result', 'ptr', 'ptr'), task, 'result');
  byPointer := r^.isFunction and returnsByPointer(r^.resultType);
  if byPointer then
    irArgsAddAttr(buf, resultSlot, resultCallAttr(r^.resultType));
  if hasSelf(r) then
    irArgsAdd(buf, irLoad(irPtrT, irStructGEP(argsTy, args, 1, ''), 'self'));
  slot := firstParamSlot(r);
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    addr := irStructGEP(argsTy, args, slot, '');
    slot := slot + 1;
    t := p^.varType;
    if isStructured(t) then { (the callee copies a value parameter) }
      irArgsAdd(buf, addr)
    else
      irArgsAdd(buf, loadScalar(addr, t, nm(p^.ident.name)));
  end;
  res := irCall(fnOf(r), buf, '');
  bufFree(buf);
  if r^.isFunction and not byPointer then
  begin
    t := r^.resultType;
    if (t^.kind = tyBoolean) or isOrdinal(t) or (t^.kind = tyReal) then
      storeScalar(resultSlot, res, t)
    else { (a counted value moves into the task) }
      irStore(res, resultSlot);
  end;
  irRetVoid;
  if fini = nil then
    exit;
  { R.taskfini(args): the counted arguments released }
  curFn := fini;
  resetAddrAligns;
  irBeginBody(fini);
  irSetInsertPoint(irNewBlock('entry'));
  slot := firstParamSlot(r);
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    addr := irStructGEP(argsTy, irArg(fini, 0), slot, '');
    slot := slot + 1;
    if isManaged(p^.varType) then
      finalizeValue(addr, p^.varType);
  end;
  irRetVoid;
end;

{ A call of async routine r (link; self_: Self's value or null): its task,
  made with copies of the arguments and started (here: it runs until it
  waits or ends). }
function emitAsyncCall(r: PDecl; const link, self_: TV; const args: TPtrList): TV;
var
  mode, i, slot: longint;
  argsTy: PIRType;
  info, finiV, task, argsAddr: TV;
  p: PDecl;
  arg: PNode;
  start: string;
  buf: TBuf;
begin
  mode := nextStart;
  nextStart := 0;
  argsTy := PIRType(r^.cgArgsTy);
  if r^.isFunction then
    info := typeInfoOf(r^.resultType)
  else
    info := irNull;
  if r^.cgTaskFini <> nil then
    finiV := irFuncRef(PIRFunc(r^.cgTaskFini))
  else
    finiV := irNull;
  bufInit(buf);
  irArgsAdd(buf, info);
  irArgsAdd(buf, i64c(argsTy^.size));
  irArgsAdd(buf, irFuncRef(PIRFunc(r^.cgTaskEntry)));
  irArgsAdd(buf, finiV);
  task := irCall(classRuntime('__pas_task_new', 'ptr', 'ptr,i64,ptr,ptr'), buf, 'task');
  bufFree(buf);
  argsAddr := call1(classRuntime('__pas_task_args', 'ptr', 'ptr'), task, 'args');
  irStore(link, irStructGEP(argsTy, argsAddr, 0, ''));
  if hasSelf(r) then
    irStore(self_, irStructGEP(argsTy, argsAddr, 1, ''));
  slot := firstParamSlot(r);
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    if i < args.count then
      arg := PNode(listGet(args, i))
    else
      arg := p^.paramDefault;
    storeExpr(irStructGEP(argsTy, argsAddr, slot, ''), p^.varType, arg);
    slot := slot + 1;
  end;
  if mode = 1 then
    start := '__pas_task_spawn'
  else if mode = 2 then
    start := '__pas_task_run_pool'
  else
    start := '__pas_task_start';
  ignoredW := call1(classRuntime(start, 'void', 'ptr'), task, '');
  emitAsyncCall := holdDynResult(task, r^.asyncType);
end;

{ A call of iterator routine r (link; self_: Self's value or null): its
  iterator, made with copies of the arguments; its body runs when its
  values are asked for. }
function emitIteratorCall(r: PDecl; const link, self_: TV; const args: TPtrList): TV;
var
  i, slot: longint;
  argsTy: PIRType;
  finiV, gen, argsAddr: TV;
  p: PDecl;
  arg: PNode;
  buf: TBuf;
begin
  argsTy := PIRType(r^.cgArgsTy);
  if r^.cgTaskFini <> nil then
    finiV := irFuncRef(PIRFunc(r^.cgTaskFini))
  else
    finiV := irNull;
  bufInit(buf);
  irArgsAdd(buf, typeInfoOf(r^.iterType^.iterElement));
  irArgsAdd(buf, i64c(argsTy^.size));
  irArgsAdd(buf, irFuncRef(PIRFunc(r^.cgTaskEntry)));
  irArgsAdd(buf, finiV);
  gen := irCall(classRuntime('__pas_gen_new', 'ptr', 'ptr,i64,ptr,ptr'), buf, 'iter');
  bufFree(buf);
  argsAddr := call1(classRuntime('__pas_gen_args', 'ptr', 'ptr'), gen, 'args');
  irStore(link, irStructGEP(argsTy, argsAddr, 0, ''));
  if hasSelf(r) then
    irStore(self_, irStructGEP(argsTy, argsAddr, 1, ''));
  slot := firstParamSlot(r);
  for i := 0 to r^.params.count - 1 do
  begin
    p := PDecl(listGet(r^.params, i));
    if i < args.count then
      arg := PNode(listGet(args, i))
    else
      arg := p^.paramDefault;
    storeExpr(irStructGEP(argsTy, argsAddr, slot, ''), p^.varType, arg);
    slot := slot + 1;
  end;
  emitIteratorCall := holdDynResult(gen, r^.iterType);
end;

{ IteratorNext(it): runs it to its next value; an i1. }
function emitIteratorNext(const it: TV): TV;
var
  r: TV;
begin
  r := call1(classRuntime('__pas_gen_next', 'i32', 'ptr'), it, 'more');
  emitIteratorNext := irICmp('ne', r, i32c(0), 'more');
end;

{ The address of iterator it's current value. }
function iteratorValueAddr(const it: TV): TV;
begin
  iteratorValueAddr := call1(classRuntime('__pas_gen_value', 'ptr', 'ptr'), it, 'value');
end;

{ await / Wait: waits for task t (of type tt); its result (an address for
  a structured type), or null without one. }
function emitAwait(const t: TV; tt: PType): TV;
var
  rt: PType;
  res: TV;
begin
  ignoredW := call1(classRuntime('__pas_task_await', 'void', 'ptr'), t, '');
  rt := tt^.taskResult;
  if rt = nil then
    exit(irNull);
  res := call1(classRuntime('__pas_task_result', 'ptr', 'ptr'), t, 'result');
  if isStructured(rt) then
    exit(res);
  emitAwait := loadScalar(res, rt, 'awaited');
end;

{ WhenAll(tasks) / WhenAny(tasks): the tasks' data and count. }
function emitWhen(e: PNode; all: boolean): TV;
var
  a: PNode;
  at, tt, rt: PType;
  data, n, desc, task, resInfo, elemInfo: TV;
  bd: TOrdinalBounds;
  buf: TBuf;
begin
  a := PNode(listGet(e^.args, 0));
  at := a^.typ;
  if at^.kind = tyArray then
  begin
    data := emitAddress(a);
    bd := getOrdinalBounds(at^.indexType);
    n := i64c(bd.hi - bd.lo + 1);
    tt := at^.elementType;
  end
  else if at^.kind = tyDynArray then
  begin
    data := visitExpr(a);
    n := dynLength(data);
    tt := at^.dynElement;
  end
  else
  begin
    desc := openDescriptor(a);
    data := irLoad(irPtrT, desc, 'data');
    n := openCount(desc);
    tt := at^.openElement;
  end;
  rt := e^.typ;
  bufInit(buf);
  irArgsAdd(buf, data);
  irArgsAdd(buf, n);
  if all then
  begin
    if rt^.taskResult <> nil then
      resInfo := typeInfoOf(rt^.taskResult)
    else
      resInfo := irNull;
    if tt^.taskResult <> nil then
      elemInfo := typeInfoOf(tt^.taskResult)
    else
      elemInfo := irNull;
    irArgsAdd(buf, resInfo);
    irArgsAdd(buf, elemInfo);
    task := irCall(classRuntime('__pas_when_all', 'ptr', 'ptr,i64,ptr,ptr'), buf, 'whenall');
  end
  else
  begin
    irArgsAdd(buf, typeInfoOf(rt^.taskResult));
    task := irCall(classRuntime('__pas_when_any', 'ptr', 'ptr,i64,ptr'), buf, 'whenany');
  end;
  bufFree(buf);
  emitWhen := holdDynResult(task, rt);
end;

{ Is the reference of type t at addr nil? (an i1) }
function isNilAt(const addr: TV; t: PType): TV;
var
  p: TV;
begin
  if t^.kind = tyInterface then { object, table }
    p := irLoad(irPtrT, addr, 'obj')
  else
    p := irLoad(irPtrT, addr, 'ref');
  isNilAt := irICmp('eq', p, irNull, 'isnil');
end;

{ The default value of type t (0, false, '', nil), as visitExpr gives it. }
function zeroValue(t: PType): TV;
begin
  if isBoolBase(t) then
    zeroValue := irConstBool(false)
  else
    zeroValue := irZero(memType(t));
end;

{ x?.m: x's value in a hidden variable; where it is not nil, m (in
  another, which is the value; else the default value) or the call. }
function visitSafeNav(e: PNode): TV;
var
  bt, rt: PType;
  tmp, res: TV;
  thenB, endB: PIRBlock;
begin
  bt := e^.navBase^.typ;
  tmp := varAddress(e^.navTmp);
  storeExpr(tmp, bt, e^.navBase);
  rt := nil;
  res := irNull;
  if e^.navRes <> nil then
  begin
    res := varAddress(e^.navRes);
    rt := e^.navRes^.varType;
    storeValue(res, zeroValue(rt), rt);
  end;
  thenB := irNewBlock('nav.then');
  endB := irNewBlock('nav.end');
  irCondBr(isNilAt(tmp, bt), endB, thenB, false);
  irSetInsertPoint(thenB);
  if e^.navStatement then
    visitStmt(e^.navCall)
  else
  begin
    storeExpr(res, rt, e^.navAccess);
    if e^.navOk <> nil then
      storeScalar(varAddress(e^.navOk), irConstBool(true), e^.navOk^.varType);
  end;
  irBr(endB);
  irSetInsertPoint(endB);
  if e^.navStatement then
    visitSafeNav := irNull
  else
    visitSafeNav := visitExpr(e^.navResRef);
end;

{ a ?? b: a in a hidden variable, replaced by b where a is nil (or where
  a's chain of ?. stopped). }
function visitCoalesce(e: PNode): TV;
var
  l, nav: PNode;
  ok: PDecl;
  t: PType;
  res, missing: TV;
  thenB, endB: PIRBlock;
begin
  l := e^.coLeft;
  while l^.kind = nkParen do
    l := l^.inner;
  ok := nil;
  if l^.kind = nkSafeNav then
  begin
    nav := l;
    while nav^.navAccess^.kind = nkSafeNav do
      nav := nav^.navAccess;
    ok := nav^.navOk;
  end;
  if ok <> nil then
    storeScalar(varAddress(ok), irConstBool(false), ok^.varType);
  t := e^.typ;
  res := varAddress(e^.coRes);
  storeExpr(res, t, e^.coLeft);
  if ok <> nil then
    missing := irBin('xor', loadScalar(varAddress(ok), ok^.varType, ''),
                     irConstBool(true), 'missing')
  else
    missing := isNilAt(res, t);
  thenB := irNewBlock('coalesce.else');
  endB := irNewBlock('coalesce.end');
  irCondBr(missing, thenB, endB, false);
  irSetInsertPoint(thenB);
  storeExpr(res, t, e^.coRight);
  irBr(endB);
  irSetInsertPoint(endB);
  visitCoalesce := visitExpr(e^.coResRef);
end;

function visitExprKind(e: PNode): TV;
var
  noArgs: TPtrList;
begin
  case e^.kind of
    nkIntegerLit: visitExprKind := irConstInt(regType(e^.typ), e^.intValue);
    nkRealLit: visitExprKind := irConstDouble(e^.realValue);
    nkStringLit:
      { a character; longer strings are handled by address }
      if e^.strWide <> 0 then
        visitExprKind := irConstInt(irI16, e^.strWide)
      else
        visitExprKind := i8c(ord(e^.strValue^.chars^[0]));
    nkNameRef: visitExprKind := visitNameRef(e);
    nkParen: visitExprKind := visitExpr(e^.inner);
    nkMove: visitExprKind := emitMove(e);
    nkTemplate: visitExprKind := emitTemplate(e);
    nkTuple: visitExprKind := emitTuple(e);
    nkAwait: visitExprKind := emitAwait(visitExpr(e^.awaitOperand), e^.awaitOperand^.typ);
    nkSpawn:
      begin
        if e^.spawnPool then
          nextStart := 2
        else
          nextStart := 1;
        visitExprKind := visitExpr(e^.spawnCall);
        nextStart := 0;
      end;
    nkSafeNav: visitExprKind := visitSafeNav(e);
    nkCoalesce: visitExprKind := visitCoalesce(e);
    nkCondExpr:
      begin
        { the if statement Sema made of it sets a hidden variable, which
          is the value }
        visitStmt(e^.ceLowered);
        if isStructured(e^.typ) then
          visitExprKind := emitAddress(e^.ceResult)
        else
          visitExprKind := visitExpr(e^.ceResult);
      end;
    nkUnary: visitExprKind := visitUnary(e);
    nkBinary: visitExprKind := visitBinary(e);
    nkCall: visitExprKind := visitCall(e);
    nkIndirectCall: visitExprKind := visitIndirectCall(e);
    nkAddrOf:
      if e^.addrMethod <> nil then
        visitExprKind := emitMethodPointer(e)
      else if (e^.addrLambda <> nil) and e^.addrLambda^.isMethodLambda then
        visitExprKind := emitMethodLambda(e^.addrLambda)
      else if e^.addrCode <> nil then
        visitExprKind := irFuncRef(fnOf(e^.addrCode))
      else
        visitExprKind := emitAddress(e^.inner);
    nkNil: visitExprKind := irNull;
    nkSetCtor:
      if (e^.typ <> nil) and (e^.typ^.kind = tyDynArray) then
        visitExprKind := emitDynArrayCtor(e, e^.typ)
      else
        visitExprKind := emitSetCtor(e);
    nkDeref, nkIndex:
      if (e^.kind = nkIndex) and (e^.idxProp <> nil) then
        visitExprKind := emitArrayPropertyRead(e)
      else if (e^.kind = nkIndex) and isBitPackedComponent(e) then
        visitExprKind := bitLoad(e)
      else if e^.typ^.kind = tyShortString then
        visitExprKind := emitAddress(e)
      else if (e^.kind = nkIndex) and (e^.base^.typ^.kind = tyAnsiString) then
        visitExprKind := emitAnsiIndexRead(e)
      else
        visitExprKind := loadScalar(emitAddress(e), e^.typ, '');
    nkField:
      if e^.constRef <> nil then { TFoo.C }
        visitExprKind := visitExpr(e^.constRef)
      else if isBitPackedComponent(e) then
        visitExprKind := bitLoad(e)
      else if (e^.fieldDecl^.kind <> dkField) or (e^.base^.typ^.kind = tyClass) then
        visitExprKind := emitMemberValue(e) { objfpc: a member of an object }
      else if e^.typ^.kind = tyShortString then
        visitExprKind := emitAddress(e)
      else
        visitExprKind := loadScalar(emitAddress(e), e^.typ, nm(e^.fieldName.name));
    nkInherited:
      begin
        listInit(noArgs);
        visitExprKind := emitInheritedCall(e, noArgs);
      end;
  else
    { formatted expressions and aggregates are handled by their users }
    visitExprKind := irZero(irI32);
  end;
end;

{ The value of e, a user-defined conversion applied (its operator called
  with e's unconverted value); an address for a record. }
function visitExpr(e: PNode): TV;
begin
  if (e^.convOp <> nil) and not e^.convSuspended then
    visitExpr := emitConversion(e)
  else
    visitExpr := visitExprKind(e);
end;

function emitConversion(e: PNode): TV;
var
  op: PDecl;
  t: PType;
  args: TPtrList;
begin
  op := e^.convOp;
  { while computing the value to convert: no conversion, the unconverted
    type }
  t := e^.typ;
  e^.typ := e^.convFrom;
  e^.convFrom := t;
  e^.convSuspended := true;
  listInit(args);
  listAdd(args, e);
  emitConversion := emitCall(op, args);
  listFree(args);
  t := e^.typ;
  e^.typ := e^.convFrom;
  e^.convFrom := t;
  e^.convSuspended := false;
end;

{$I pcgenstmt.inc}

end.
