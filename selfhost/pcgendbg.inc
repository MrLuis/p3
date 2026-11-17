{ pcgen, continued: debug information (-g), the port of CodeGen.cpp's
  DIBuilder code. The metadata nodes are built with the texts and operand
  orders LLVM 19 gives them, so the module prints as the C++ pcc's. Nodes
  whose text is known only at the end (the compile unit's lists, a
  subprogram's retainedNodes) are placeholders filled by finalizeDebugInfo,
  as DIBuilder::finalize does. }

type
  PSPInfo = ^TSPInfo;
  TSPInfo = record
    sp, file_, typ: PMeta;
    name: string;
    line: longint;
    tracked: boolean;    { a variable was created in it: retainedNodes }
  end;

var
  dbgOn: boolean;
  dbgOptimized: boolean;
  cuMeta, unitFile: PMeta;
  curSP: PSPInfo;
  diTypes: TIntMap;      { type id -> PMeta }
  diFiles: TIntMap;      { file name id -> PMeta }
  fileScopes: TIntMap;   { name id of 'sp/file' -> PMeta }
  allEnums, allGVs, allSPs: TPtrList;

function hexChar(v: longint): char;
begin
  if v < 10 then
    hexChar := chr(ord('0') + v)
  else
    hexChar := chr(ord('A') + v - 10);
end;

{ an MDString: quoted, escaped as LLVM's printEscapedString }
function mdStr(const s: string): string;
var
  r: string;
  i: longint;
  c: char;
begin
  r := '"';
  for i := 1 to length(s) do
  begin
    c := s[i];
    if c = '\' then
      r := r + '\\'
    else if (c >= ' ') and (c <= '~') and (c <> '"') then
      r := r + c
    else
      r := r + '\' + hexChar(ord(c) shr 4) + hexChar(ord(c) and 15);
  end;
  mdStr := r + '"';
end;

function ref(m: PMeta): string;
begin
  if m = nil then
    ref := 'null'
  else
    ref := irMetaRef(m);
end;

{ a node from a text and up to four operands (nil ones are skipped) }
function md(distinct: boolean; const text: string; o1, o2, o3, o4: PMeta): PMeta;
var
  b: TBuf;
  ops: TPtrList;
begin
  bufInit(b);
  bufAdd(b, text);
  listInit(ops);
  if o1 <> nil then
    listAdd(ops, o1);
  if o2 <> nil then
    listAdd(ops, o2);
  if o3 <> nil then
    listAdd(ops, o3);
  if o4 <> nil then
    listAdd(ops, o4);
  md := irMeta(distinct, b, ops);
  listFree(ops);
  bufFree(b);
end;

{ a tuple of the nodes (nil prints null) }
function mdTuple(const elems: TPtrList): PMeta;
var
  b: TBuf;
  ops: TPtrList;
  i: longint;
  e: PMeta;
begin
  bufInit(b);
  listInit(ops);
  bufAdd(b, '!{');
  for i := 0 to elems.count - 1 do
  begin
    e := PMeta(listGet(elems, i));
    if i > 0 then
      bufAdd(b, ', ');
    bufAdd(b, ref(e));
    if e <> nil then
      listAdd(ops, e);
  end;
  bufAddChar(b, '}');
  mdTuple := irMeta(false, b, ops);
  listFree(ops);
  bufFree(b);
end;

function sizeField(name: string; v: int64): string;
begin
  if v = 0 then
    sizeField := ''
  else
    sizeField := ', ' + name + ': ' + intToStr(v);
end;

function nameField(n: PName): string;
begin
  if (n = nil) or (n^.len = 0) then
    nameField := ''
  else
    nameField := 'name: ' + mdStr(nm(n)) + ', ';
end;

{ llvm::sys::fs::make_absolute, then the file name and its directory }
procedure splitAbsolute(const path: string; var fname, dir: string);
var
  abs_: string;
  i: longint;
begin
  abs_ := path;
  if (abs_ = '') or (abs_[1] <> '/') then
    abs_ := sysGetCwd + '/' + abs_;
  i := length(abs_);
  while (i > 0) and (abs_[i] <> '/') do
    i := i - 1;
  fname := copy(abs_, i + 1, 255);
  dir := copy(abs_, 1, i - 1);
  if (i = 1) then
    dir := '/';
end;

function createFile(const path: string): PMeta;
var
  fname, dir: string;
begin
  splitAbsolute(path, fname, dir);
  createFile := md(false, '!DIFile(filename: ' + mdStr(fname) + ', directory: ' +
                   mdStr(dir) + ')', nil, nil, nil, nil);
end;

function diFileFor(loc: TLoc): PMeta;
var
  fid, line, col: longint;
  key: PName;
  f: PMeta;
begin
  if loc = noLoc then
    exit(unitFile);
  smPresumed(loc, fid, line, col);
  key := intern(smFileName(fid));
  f := PMeta(mapGet(diFiles, key^.id));
  if f = nil then
  begin
    f := createFile(smFileName(fid));
    mapPut(diFiles, key^.id, f);
  end;
  diFileFor := f;
end;

function lineOf(loc: TLoc): longint;
var
  fid, line, col: longint;
begin
  if loc = noLoc then
    exit(0);
  smPresumed(loc, fid, line, col);
  lineOf := line;
end;

procedure initDebugInfo(const sourceName: string);
var
  list: TPtrList;
begin
  mapInit(diTypes);
  mapInit(diFiles);
  mapInit(fileScopes);
  listInit(allEnums);
  listInit(allGVs);
  listInit(allSPs);
  curSP := nil;
  unitFile := createFile(sourceName);
  cuMeta := irMetaPlaceholder;
  listInit(list);
  listAdd(list, cuMeta);
  irNamedMeta('llvm.dbg.cu', list);
  listClear(list);
  { DWARF 4: GNU ld mis-merges DWARF 5 string-offset tables when these
    objects are linked with clang-compiled ones (libpascalrt) }
  listAdd(list, md(false, '!{i32 2, !"Debug Info Version", i32 3}', nil, nil, nil, nil));
  listAdd(list, md(false, '!{i32 2, !"Dwarf Version", i32 4}', nil, nil, nil, nil));
  irNamedMeta('llvm.module.flags', list);
  listFree(list);
end;

function basicType(const name: string; bits: longint; const enc: string): PMeta;
begin
  basicType := md(false, '!DIBasicType(name: ' + mdStr(name) + sizeField('size', bits) +
                  ', encoding: ' + enc + ')', nil, nil, nil, nil);
end;

function unspecifiedType(const name: string): PMeta;
begin
  unspecifiedType := md(false, '!DIBasicType(tag: DW_TAG_unspecified_type, name: ' +
                        mdStr(name) + ')', nil, nil, nil, nil);
end;

function pointerType(base: PMeta; tag: string): PMeta;
begin
  pointerType := md(false, '!DIDerivedType(tag: ' + tag + ', baseType: ' + ref(base) +
                    ', size: 64)', base, nil, nil, nil);
end;

function subrange(lo, count: int64): PMeta;
begin
  subrange := md(false, '!DISubrange(count: ' + intToStr(count) + ', lowerBound: ' +
                 intToStr(lo) + ')', nil, nil, nil, nil);
end;

function arrayType(sizeBits: int64; elem, sub: PMeta): PMeta;
var
  elems: TPtrList;
  t: PMeta;
begin
  listInit(elems);
  listAdd(elems, sub);
  t := mdTuple(elems);
  listFree(elems);
  arrayType := md(false, '!DICompositeType(tag: DW_TAG_array_type, baseType: ' + ref(elem) +
                  sizeField('size', sizeBits) + ', elements: ' + ref(t) + ')',
                  elem, t, nil, nil);
end;

function debugType(t: PType): PMeta; forward;

function buildDebugType(t: PType): PMeta;
var
  elems: TPtrList;
  i: longint;
  c, f: PDecl;
  intT, elemDI, tup, def, member, ptr: PMeta;
  lo, count: int64;
  bd: TOrdinalBounds;
  b: TBuf;
  ignored: PIRType;
  s: string;
begin
  case t^.kind of
    tyInteger:
      if t^.isSigned then
        buildDebugType := basicType(nm(t^.intName), t^.bits, 'DW_ATE_signed')
      else
        buildDebugType := basicType(nm(t^.intName), t^.bits, 'DW_ATE_unsigned');
    tyReal: buildDebugType := basicType('real', 64, 'DW_ATE_float');
    tyBoolean: buildDebugType := basicType('boolean', 8, 'DW_ATE_boolean');
    tyChar: buildDebugType := basicType('char', 8, 'DW_ATE_unsigned_char');
    tyWideChar: buildDebugType := basicType('widechar', 16, 'DW_ATE_unsigned_char');
    tySubrange: buildDebugType := debugType(t^.host);
    tyDynArray, tyOpenArray, tyArrayOfConst, tyTask, tyToken:
      buildDebugType := pointerType(nil, 'DW_TAG_pointer_type');
    tyAnsiString:
      buildDebugType := pointerType(basicType('char', 8, 'DW_ATE_unsigned_char'),
                                    'DW_TAG_pointer_type');
    tyEnum:
      begin
        listInit(elems);
        for i := 0 to t^.enumValues.count - 1 do
        begin
          c := PDecl(listGet(t^.enumValues, i));
          listAdd(elems, md(false, '!DIEnumerator(name: ' + mdStr(nm(c^.ident.spelling)) +
                                   ', value: ' + intToStr(c^.constValue.intVal) + ')',
                            nil, nil, nil, nil));
        end;
        tup := mdTuple(elems);
        listFree(elems);
        intT := basicType('integer', 32, 'DW_ATE_signed');
        def := md(false, '!DICompositeType(tag: DW_TAG_enumeration_type, ' +
                                    nameField(t^.declName) + 'file: ' + ref(unitFile) +
                                    ', baseType: ' + ref(intT) +
                                    ', size: 32, align: 32, elements: ' + ref(tup) + ')',
                             unitFile, intT, tup, nil);
        listAdd(allEnums, def);
        buildDebugType := def;
      end;
    tyStringLit, tyArray:
      begin
        if hasBitElements(t) then { (its bytes) }
        begin
          elemDI := basicType('byte', 8, 'DW_ATE_unsigned');
          exit(arrayType(sizeOf_(t) * 8, elemDI, subrange(0, sizeOf_(t))));
        end;
        if t^.kind = tyStringLit then
        begin
          elemDI := basicType('char', 8, 'DW_ATE_unsigned_char');
          lo := 1;
          count := t^.strLength;
        end
        else
        begin
          elemDI := debugType(t^.elementType);
          bd := getOrdinalBounds(t^.indexType);
          lo := bd.lo;
          count := bd.hi - bd.lo + 1;
        end;
        buildDebugType := arrayType(sizeOf_(t) * 8, elemDI, subrange(lo, count));
      end;
    tyRecord:
      begin
        ignored := memType(t); { computes the field offsets }
        { the record first: fields may point back to it }
        def := irMetaPlaceholder;
        mapPut(diTypes, t^.id, def);
        listInit(elems);
        for i := 0 to t^.allFields.count - 1 do
        begin
          f := PDecl(listGet(t^.allFields, i));
          s := '!DIDerivedType(tag: DW_TAG_member, name: ' + mdStr(nm(f^.ident.spelling)) +
               ', scope: ' + ref(def) + ', file: ' + ref(unitFile) +
               sizeField('line', lineOf(f^.loc));
          member := debugType(f^.fieldType);
          if t^.isBitPackedRecord and isOrdinal(f^.fieldType) then { a bit field }
            s := s + ', baseType: ' + ref(member) +
                 sizeField('size', bitPackedWidth(f^.fieldType)) +
                 sizeField('offset', f^.cgBitOffset) +
                 ', flags: DIFlagBitField, extraData: i64 0)'
          else
            s := s + ', baseType: ' + ref(member) +
                 sizeField('size', sizeOf_(f^.fieldType) * 8) +
                 sizeField('offset', f^.cgOffset * 8) + ')';
          listAdd(elems, md(false, s, unitFile, def, member, nil));
        end;
        tup := mdTuple(elems);
        listFree(elems);
        bufInit(b);
        bufAdd(b, '!DICompositeType(tag: DW_TAG_structure_type, ' + nameField(t^.declName) +
                  'file: ' + ref(unitFile) + sizeField('size', sizeOf_(t) * 8) +
                  ', elements: ' + ref(tup) + ')');
        listInit(elems);
        listAdd(elems, unitFile);
        listAdd(elems, tup);
        irMetaFill(def, b, elems);
        listFree(elems);
        bufFree(b);
        buildDebugType := def;
      end;
    tyPointer:
      if t^.pointee <> nil then
        buildDebugType := pointerType(debugType(t^.pointee), 'DW_TAG_pointer_type')
      else
        buildDebugType := pointerType(nil, 'DW_TAG_pointer_type');
    tySet: { its bytes }
      buildDebugType := arrayType(setStorageBits(t),
                                  basicType('byte', 8, 'DW_ATE_unsigned'),
                                  subrange(0, setStorageBits(t) div 8));
    tyFile:
      buildDebugType := pointerType(unspecifiedType('PasFile'), 'DW_TAG_pointer_type');
    tyClass:
      begin
        { a pointer to a structure holding the fields (after the VMT pointer) }
        ignored := classLayout(t);
        def := irMetaPlaceholder;
        ptr := pointerType(def, 'DW_TAG_pointer_type');
        mapPut(diTypes, t^.id, ptr); { fields may refer to the class }
        listInit(elems);
        for i := 0 to t^.classAllFields.count - 1 do
        begin
          f := PDecl(listGet(t^.classAllFields, i));
          s := '!DIDerivedType(tag: DW_TAG_member, name: ' + mdStr(nm(f^.ident.spelling)) +
               ', scope: ' + ref(def) + ', file: ' + ref(unitFile) +
               sizeField('line', lineOf(f^.loc));
          member := debugType(f^.fieldType);
          s := s + ', baseType: ' + ref(member) + sizeField('size', sizeOf_(f^.fieldType) * 8) +
               sizeField('offset', f^.cgOffset * 8) + ')';
          listAdd(elems, md(false, s, unitFile, def, member, nil));
        end;
        tup := mdTuple(elems);
        listFree(elems);
        bufInit(b);
        bufAdd(b, '!DICompositeType(tag: DW_TAG_structure_type, ' + nameField(t^.declName) +
                  'file: ' + ref(unitFile) + sizeField('size', instanceSize(t) * 8) +
                  ', elements: ' + ref(tup) + ')');
        listInit(elems);
        listAdd(elems, unitFile);
        listAdd(elems, tup);
        irMetaFill(def, b, elems);
        listFree(elems);
        bufFree(b);
        buildDebugType := ptr;
      end;
  else
    begin
      { procedural values, nil }
      bufInit(b);
      bufAddTypeName(b, t);
      s := '';
      for i := 0 to longint(b.len) - 1 do
        s := s + bufCharAt(b, i);
      bufFree(b);
      buildDebugType := unspecifiedType(s);
    end;
  end;
end;

function debugType(t: PType): PMeta;
var
  d: PMeta;
begin
  d := PMeta(mapGet(diTypes, t^.id));
  if d <> nil then
    exit(d);
  d := buildDebugType(t);
  { named arrays, subranges, pointers ...: a typedef carries the name }
  if (t^.declName <> nil) and (t^.kind <> tyRecord) and (t^.kind <> tyEnum) and
     (t^.kind <> tyClass) and not isBuiltinType(t) then
    d := md(false, '!DIDerivedType(tag: DW_TAG_typedef, name: ' + mdStr(nm(t^.declName)) +
                   ', file: ' + ref(unitFile) + ', baseType: ' + ref(d) + ')',
            unitFile, d, nil, nil);
  mapPut(diTypes, t^.id, d);
  debugType := d;
end;

{ DIBuilder::createFunction for a definition (filled in at the end) }
function createSubprogram(f: PIRFunc; const name: string; scopeFile: PMeta; line: longint;
                          result_: PType; const params: TPtrList; file_: PMeta;
                          self_: PDecl): PSPInfo;
var
  sig: TPtrList;
  i: longint;
  p: PDecl;
  info: PSPInfo;
  types: PMeta;
begin
  listInit(sig);
  if result_ <> nil then
    listAdd(sig, debugType(result_))
  else
    listAdd(sig, nil);
  if self_ <> nil then { methods: Self first (a record's by reference) }
  begin
    if isByRef(self_) then
      listAdd(sig, pointerType(debugType(self_^.varType), 'DW_TAG_reference_type'))
    else
      listAdd(sig, debugType(self_^.varType));
  end;
  for i := 0 to params.count - 1 do
  begin
    p := PDecl(listGet(params, i));
    if isByRef(p) then
      listAdd(sig, pointerType(debugType(p^.varType), 'DW_TAG_reference_type'))
    else
      listAdd(sig, debugType(p^.varType));
  end;
  types := mdTuple(sig);
  listFree(sig);
  info := PSPInfo(arenaAlloc(sizeof(TSPInfo)));
  info^.typ := md(false, '!DISubroutineType(types: ' + ref(types) + ')', types, nil, nil, nil);
  info^.sp := irMetaPlaceholder;
  info^.file_ := file_;
  if file_ = nil then
    info^.file_ := unitFile;
  info^.name := name;
  info^.line := line;
  info^.tracked := false;
  if scopeFile = nil then
    scopeFile := unitFile;
  listAdd(allSPs, info);
  irSetFuncDbg(f, info^.sp);
  createSubprogram := info;
end;

procedure setDebugLoc(loc: TLoc);
var
  fid, line, col: longint;
  scope, f: PMeta;
  key: PName;
  s: string;
begin
  if not dbgOn or (curSP = nil) or (loc = noLoc) then
    exit;
  smPresumed(loc, fid, line, col);
  { code from another file (a unit's initialization in main) needs a scope
    naming that file }
  scope := curSP^.sp;
  f := diFileFor(loc);
  if f <> curSP^.file_ then
  begin
    key := intern(intToStr(curSP^.sp^.id) + '/' + intToStr(f^.id));
    scope := PMeta(mapGet(fileScopes, key^.id));
    if scope = nil then
    begin
      scope := md(false, '!DILexicalBlockFile(scope: ' + ref(curSP^.sp) + ', file: ' +
                         ref(f) + ', discriminator: 0)', f, curSP^.sp, nil, nil);
      mapPut(fileScopes, key^.id, scope);
    end;
  end;
  s := '!DILocation(line: ' + intToStr(line);
  if col <> 0 then
    s := s + ', column: ' + intToStr(col);
  irSetDebugLoc(md(false, s + ', scope: ' + ref(scope) + ')', scope, nil, nil, nil));
end;

{ Describes the variables of r (parameters, locals, result) to the
  debugger: offsets into the frame, or their own allocas. }
procedure declareFrameVariables(r: PDecl);
var
  frameT: PIRType;
  argNo, i: longint;
  v: PDecl;

  procedure declare(x: PDecl; arg: longint);
  var
    ty, dv, loc: PMeta;
    line: longint;
    s, expr: string;
  begin
    ty := debugType(x^.varType);
    { Self has no location of its own: the method's }
    if x^.loc = noLoc then
      line := lineOf(bodyLoc(r))
    else
      line := lineOf(x^.loc);
    s := '!DILocalVariable(name: ' + mdStr(nm(x^.ident.spelling));
    if arg > 0 then
      s := s + ', arg: ' + intToStr(arg);
    s := s + ', scope: ' + ref(curSP^.sp) + ', file: ' + ref(unitFile) +
         sizeField('line', line) + ', type: ' + ref(ty) + ')';
    dv := md(false, s, curSP^.sp, unitFile, ty, nil);
    curSP^.tracked := true;
    expr := '!DIExpression(';
    if x^.cgInFrame then
    begin
      expr := expr + 'DW_OP_plus_uconst, ' + intToStr(irFieldOffset(frameT, x^.cgSlot));
      if isByRef(x) then
        expr := expr + ', DW_OP_deref';
    end
    else if isByRef(x) then
      expr := expr + 'DW_OP_deref';
    expr := expr + ')';
    s := '!DILocation(line: ' + intToStr(line) + ', scope: ' + ref(curSP^.sp) + ')';
    loc := md(false, s, curSP^.sp, nil, nil, nil);
    if x^.cgInFrame then
      irInsertDeclare(curFrame, dv, expr, loc)
    else
      irInsertDeclare(PTVBox(x^.cgLocal)^, dv, expr, loc);
  end;

begin
  frameT := frameType(r);
  argNo := 1;
  if hasSelf(r) then
  begin
    declare(r^.selfVar, argNo);
    argNo := argNo + 1;
  end;
  for i := 0 to r^.params.count - 1 do
  begin
    declare(PDecl(listGet(r^.params, i)), argNo);
    argNo := argNo + 1;
  end;
  for i := 0 to r^.body^.decls.count - 1 do
  begin
    v := PDecl(listGet(r^.body^.decls, i));
    if (v^.kind = dkVar) and ((v^.init = nil) or v^.isInitVar) and (v^.absTarget = nil) then
      declare(v, 0);
  end;
  if r^.isFunction then
    declare(r^.resultVar, 0);
end;

procedure declareGlobalDebugInfo(v: PDecl; const g: TV);
var
  ty, gv, gve: PMeta;
  s: string;
begin
  ty := debugType(v^.varType);
  { no linkage name: debuggers would show "pas.x" instead of "x" }
  s := '!DIGlobalVariable(name: ' + mdStr(nm(v^.ident.spelling)) + ', scope: ' + ref(cuMeta) +
       ', file: ' + ref(unitFile) + sizeField('line', lineOf(v^.loc)) + ', type: ' + ref(ty) +
       ', isLocal: true, isDefinition: true)';
  gv := md(true, s, cuMeta, unitFile, ty, nil);
  gve := md(false, '!DIGlobalVariableExpression(var: ' + ref(gv) +
                   ', expr: !DIExpression())', gv, nil, nil, nil);
  listAdd(allGVs, gve);
  irSetGlobalDbg(g, gve);
end;

{ DIBuilder::finalize: the compile unit's lists, subprograms' retained
  nodes }
procedure finalizeDebugInfo;
var
  b: TBuf;
  ops, empty: TPtrList;
  enums, gvs, retained: PMeta;
  i: longint;
  info: PSPInfo;
begin
  bufInit(b);
  listInit(ops);
  enums := nil;
  gvs := nil;
  if allEnums.count > 0 then
    enums := mdTuple(allEnums);
  if allGVs.count > 0 then
    gvs := mdTuple(allGVs);
  bufAdd(b, 'distinct !DICompileUnit(language: DW_LANG_Pascal83, file: ' + ref(unitFile) +
            ', producer: "pcc", isOptimized: ');
  if dbgOptimized then
    bufAdd(b, 'true')
  else
    bufAdd(b, 'false');
  bufAdd(b, ', runtimeVersion: 0, emissionKind: FullDebug');
  listAdd(ops, unitFile);
  if enums <> nil then
  begin
    bufAdd(b, ', enums: ' + ref(enums));
    listAdd(ops, enums);
  end;
  if gvs <> nil then
  begin
    bufAdd(b, ', globals: ' + ref(gvs));
    listAdd(ops, gvs);
  end;
  bufAddChar(b, ')');
  irMetaFill(cuMeta, b, ops);

  listInit(empty);
  for i := 0 to allSPs.count - 1 do
  begin
    info := PSPInfo(listGet(allSPs, i));
    bufClear(b);
    listClear(ops);
    bufAdd(b, 'distinct !DISubprogram(name: ' + mdStr(info^.name) + ', scope: ' +
              ref(info^.file_) + ', file: ' + ref(info^.file_) + sizeField('line', info^.line) +
              ', type: ' + ref(info^.typ) + sizeField('scopeLine', info^.line) +
              ', flags: DIFlagPrototyped, spFlags: DISPFlagDefinition');
    if dbgOptimized then
      bufAdd(b, ' | DISPFlagOptimized');
    bufAdd(b, ', unit: ' + ref(cuMeta));
    listAdd(ops, info^.file_);
    listAdd(ops, info^.typ);
    listAdd(ops, cuMeta);
    if info^.tracked then
    begin
      retained := mdTuple(empty);
      bufAdd(b, ', retainedNodes: ' + ref(retained));
      listAdd(ops, retained);
    end;
    bufAddChar(b, ')');
    irMetaFill(info^.sp, b, ops);
  end;
  bufFree(b);
end;
