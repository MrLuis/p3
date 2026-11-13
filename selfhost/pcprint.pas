unit pcprint;
{ The AST dump of the self-hosted compiler; a port of src/ast/ASTPrinter.cpp.
  One node per line, indented by depth:

    BinaryExpr '+' : integer <3:10>

  Two forms: --emit-ast (S6) shows the program with Sema's results (types,
  constant values, declaration kinds); --emit-parse-tree (S5) shows the
  tree as parsed, with type specs, typed-constant values, uses clauses and
  units. }

interface

uses pcsys, pcutil, pcdiag, pcast, pctypes;

{ Appends the parse tree of the units (in load order) and the program. }
procedure printParseTree(var outText: TBuf; prog: PDecl; const units: TPtrList);
{ Appends the AST of the program (with types once Sema has run). }
procedure printAST(var outText: TBuf; prog: PDecl);

implementation

var
  outBuf: TBuf;
  indent: longint;
  parseTree: boolean; { --emit-parse-tree rather than --emit-ast }

procedure line(const nodeName: string);
var
  i: longint;
begin
  for i := 1 to indent do
    bufAddChar(outBuf, ' ');
  bufAdd(outBuf, nodeName);
end;

{ line(prefix + 'name' + suffix) for a possibly long name }
procedure lineName(const prefix: string; n: PName);
begin
  line(prefix);
  if n <> nil then
    bufAddName(outBuf, n);
  bufAddChar(outBuf, '''');
end;

procedure loc(l: TLoc);
var
  fid, ln, col: longint;
begin
  smPresumed(l, fid, ln, col);
  if ln <> 0 then
    bufAdd(outBuf, ' <' + intToStr(ln) + ':' + intToStr(col) + '>')
  else
    bufAdd(outBuf, ' <invalid>');
  bufAddChar(outBuf, #10);
end;

procedure quoted(n: PName);
begin
  bufAddChar(outBuf, '"');
  bufAddEscaped(outBuf, n^.chars, 0, n^.len);
  bufAddChar(outBuf, '"');
end;

{ " : type" once Sema has set one }
procedure typeSuffix(t: PType);
begin
  if t <> nil then
  begin
    bufAdd(outBuf, ' : ');
    bufAddTypeName(outBuf, t);
  end;
end;

{ the type and the location of an expression }
procedure endExpr(e: PNode);
begin
  typeSuffix(e^.typ);
  if e^.convOp <> nil then { a record's := / Explicit }
  begin
    bufAdd(outBuf, ' converted by ''');
    bufAddName(outBuf, e^.convOp^.ident.name);
    bufAddChar(outBuf, '''');
    if e^.convOp^.overloadIndex <> 0 then
      bufAdd(outBuf, ' #' + intToStr(e^.convOp^.overloadIndex));
  end;
  loc(e^.b);
end;

function declKindName(d: PDecl): string;
begin
  case d^.kind of
    dkProgram: declKindName := 'program';
    dkVar:
      case d^.varKind of
        vkVariable: declKindName := 'variable';
        vkResult: declKindName := 'function result';
      else
        declKindName := 'parameter';
      end;
    dkConst: declKindName := 'constant';
    dkType: declKindName := 'type';
    dkField: declKindName := 'field';
    dkLabel: declKindName := 'label';
    dkUnit: declKindName := 'unit';
    dkProperty: declKindName := 'property';
    dkRoutine:
      if d^.methodKind = mtConstructor then
        declKindName := 'constructor'
      else if d^.methodKind = mtDestructor then
        declKindName := 'destructor'
      else if d^.methodKind = mtOperator then
        declKindName := 'operator'
      else if d^.isFunction then
        declKindName := 'function'
      else
        declKindName := 'procedure';
  else
    if d^.isProcedure then
      declKindName := 'procedure'
    else
      declKindName := 'function';
  end;
end;

{ Like the type name, but spells out a named type's structure. }
procedure addDescribedType(t: PType);
var
  i: longint;
  f, c: PDecl;
  open_: boolean;
begin
  if t^.declName = nil then
  begin
    bufAddTypeName(outBuf, t);
    exit;
  end;
  case t^.kind of
    tyArray:
      begin
        if t^.isBitPackedArray then
          bufAdd(outBuf, 'bitpacked ')
        else if t^.isPackedArray then
          bufAdd(outBuf, 'packed ');
        bufAdd(outBuf, 'array [');
        bufAddTypeName(outBuf, t^.indexType);
        bufAdd(outBuf, '] of ');
        bufAddTypeName(outBuf, t^.elementType);
      end;
    tyRecord:
      begin
        if t^.isBitPackedRecord then
          bufAdd(outBuf, 'bitpacked record')
        else
          bufAdd(outBuf, 'record');
        for i := 0 to t^.allFields.count - 1 do
        begin
          f := PDecl(listGet(t^.allFields, i));
          bufAddChar(outBuf, ' ');
          bufAddName(outBuf, f^.ident.name);
          bufAdd(outBuf, ': ');
          if f^.fieldType <> nil then
            bufAddTypeName(outBuf, f^.fieldType)
          else
            bufAddChar(outBuf, '?');
          bufAddChar(outBuf, ';');
        end;
        bufAdd(outBuf, ' end');
      end;
    tySubrange:
      begin
        bufAddTypeName(outBuf, t^.host);
        bufAdd(outBuf, ' ' + intToStr(t^.lo) + '..' + intToStr(t^.hi));
      end;
    tyEnum:
      begin
        bufAddChar(outBuf, '(');
        for i := 0 to t^.enumValues.count - 1 do
        begin
          c := PDecl(listGet(t^.enumValues, i));
          if i > 0 then
            bufAdd(outBuf, ', ');
          bufAddName(outBuf, c^.ident.name);
        end;
        bufAddChar(outBuf, ')');
      end;
    tyClass:
      begin
        bufAdd(outBuf, 'class');
        open_ := false;
        if t^.classParent <> nil then
        begin
          bufAddChar(outBuf, '(');
          bufAddTypeName(outBuf, t^.classParent);
          open_ := true;
        end;
        for i := 0 to t^.classInterfaces.count - 1 do
        begin
          if open_ then
            bufAdd(outBuf, ', ')
          else
            bufAddChar(outBuf, '(');
          open_ := true;
          bufAddTypeName(outBuf, PInterfaceImpl(listGet(t^.classInterfaces, i))^.intf);
        end;
        if open_ then
          bufAddChar(outBuf, ')');
      end;
    tyInterface:
      if t^.intfParent <> nil then
      begin
        bufAdd(outBuf, 'interface(');
        bufAddTypeName(outBuf, t^.intfParent);
        bufAddChar(outBuf, ')');
      end
      else
        bufAdd(outBuf, 'interface');
  else
    bufAddTypeName(outBuf, t);
  end;
end;

{ " private" etc. after a class member }
{ " #N": which routine of an overloaded name (objfpc) is meant }
procedure overloadNumber(d: PDecl);
begin
  if (d <> nil) and (d^.kind = dkRoutine) and (d^.overloadIndex <> 0) then
    bufAdd(outBuf, ' #' + intToStr(d^.overloadIndex));
end;

{ a record's class operator implementing an operation (Sema) }
procedure operatorOf(op: PDecl; negated: boolean);
begin
  if op = nil then
    exit;
  if negated then
    bufAdd(outBuf, ' not by ''')
  else
    bufAdd(outBuf, ' by ''');
  bufAddName(outBuf, op^.ident.name);
  bufAddChar(outBuf, '''');
  overloadNumber(op);
end;

{ hint directives (deprecated 'message', experimental, ...) }
procedure hintsSuffix(d: PDecl);
begin
  if (d^.hints and hintDeprecated) <> 0 then
  begin
    bufAdd(outBuf, ' deprecated');
    if d^.hintMessage <> nil then
    begin
      bufAddChar(outBuf, ' ');
      quoted(d^.hintMessage);
    end;
  end;
  if (d^.hints and hintExperimental) <> 0 then
    bufAdd(outBuf, ' experimental');
  if (d^.hints and hintPlatform) <> 0 then
    bufAdd(outBuf, ' platform');
  if (d^.hints and hintLibrary) <> 0 then
    bufAdd(outBuf, ' library');
  if (d^.hints and hintUnimplemented) <> 0 then
    bufAdd(outBuf, ' unimplemented');
end;

procedure visibilitySuffix(d: PDecl);
begin
  hintsSuffix(d);
  case d^.vis of
    vsPublished: bufAdd(outBuf, ' published');
    vsProtected: bufAdd(outBuf, ' protected');
    vsPrivate: bufAdd(outBuf, ' private');
    vsStrictProtected: bufAdd(outBuf, ' strict protected');
    vsStrictPrivate: bufAdd(outBuf, ' strict private');
  else
    ;
  end;
end;

procedure printConstValue(const v: TConstValue);
begin
  case v.kind of
    ckOrdinal: bufAdd(outBuf, ' = ' + intToStr(v.intVal));
    ckReal: bufAdd(outBuf, ' = ' + formatG(v.realVal, 6));
    ckString:
      begin
        bufAdd(outBuf, ' = ');
        quoted(v.strVal);
      end;
  else
    ;
  end;
end;

procedure visitExpr(e: PNode); forward;
procedure visitStmt(s: PNode); forward;
procedure printDecl(d: PDecl); forward;
procedure printBlock(b: PBlock); forward;

procedure printTypeSpec(ts: PTypeSpec); forward;

procedure printFieldList(fl: PFieldList);
var
  i, j: longint;
  f: PDecl;
  vp: PVariantPart;
  v: PVariant;
  l: PCaseLabel;
begin
  for i := 0 to fl^.fixed.count - 1 do
  begin
    f := PDecl(listGet(fl^.fixed, i));
    lineName('FieldDecl ''', f^.ident.name);
    visibilitySuffix(f);
    loc(f^.loc);
    indent := indent + 2;
    printTypeSpec(f^.fieldSpec);
    indent := indent - 2;
  end;
  vp := fl^.variant;
  if vp = nil then
    exit;
  line('VariantPart');
  loc(vp^.caseLoc);
  indent := indent + 2;
  if vp^.tag <> nil then
  begin
    lineName('TagField ''', vp^.tag^.ident.name);
    loc(vp^.tag^.loc);
  end;
  printTypeSpec(vp^.tagSpec);
  for i := 0 to vp^.variants.count - 1 do
  begin
    v := PVariant(listGet(vp^.variants, i));
    line('Variant');
    if v^.labels.count = 0 then
      loc(vp^.caseLoc)
    else
      loc(PCaseLabel(listGet(v^.labels, 0))^.lo^.b);
    indent := indent + 2;
    for j := 0 to v^.labels.count - 1 do
    begin
      l := PCaseLabel(listGet(v^.labels, j));
      line('CaseLabel');
      loc(l^.lo^.b);
      indent := indent + 2;
      visitExpr(l^.lo);
      indent := indent - 2;
    end;
    printFieldList(v^.fields);
    indent := indent - 2;
  end;
  indent := indent - 2;
end;

procedure printTypeSpec(ts: PTypeSpec);
var
  i: longint;
  id: PIdent;
begin
  if ts = nil then
    exit;
  case ts^.kind of
    tsNamed:
      begin
        lineName('NamedTypeSpec ''', ts^.tname.name);
        loc(ts^.b);
      end;
    tsDynArray:
      begin
        line('DynArrayTypeSpec');
        loc(ts^.b);
        indent := indent + 2;
        printTypeSpec(ts^.dynSpec);
        indent := indent - 2;
      end;
    tsOpenArray:
      begin
        line('OpenArrayTypeSpec');
        loc(ts^.b);
        indent := indent + 2;
        printTypeSpec(ts^.openSpec);
        indent := indent - 2;
      end;
    tsArrayOfConst:
      begin
        line('ArrayOfConstTypeSpec');
        loc(ts^.b);
      end;
    tsUntyped:
      begin
        line('UntypedTypeSpec');
        loc(ts^.b);
      end;
    tsEnum:
      begin
        if ts^.enumScoped then
          line('EnumTypeSpec scoped')
        else
          line('EnumTypeSpec');
        loc(ts^.b);
        indent := indent + 2;
        for i := 0 to ts^.enumNames.count - 1 do
        begin
          id := PIdent(listGet(ts^.enumNames, i));
          lineName('EnumValue ''', id^.name);
          loc(id^.loc);
        end;
        indent := indent - 2;
      end;
    tsSubrange:
      begin
        line('SubrangeTypeSpec');
        loc(ts^.b);
        indent := indent + 2;
        visitExpr(ts^.lo);
        visitExpr(ts^.hi);
        indent := indent - 2;
      end;
    tsArray:
      begin
        if ts^.arrBitPacked then
          line('ArrayTypeSpec bitpacked')
        else if ts^.arrPacked then
          line('ArrayTypeSpec packed')
        else
          line('ArrayTypeSpec');
        loc(ts^.b);
        indent := indent + 2;
        for i := 0 to ts^.indexes.count - 1 do
          printTypeSpec(PTypeSpec(listGet(ts^.indexes, i)));
        printTypeSpec(ts^.element);
        indent := indent - 2;
      end;
    tsRecord:
      begin
        if ts^.recBitPacked then
          line('RecordTypeSpec bitpacked')
        else if ts^.recPacked then
          line('RecordTypeSpec packed')
        else
          line('RecordTypeSpec');
        if ts^.recPackRecords <> 0 then
          bufAdd(outBuf, ' packrecords ' + intToStr(ts^.recPackRecords));
        loc(ts^.b);
        indent := indent + 2;
        printFieldList(ts^.fields);
        for i := 0 to ts^.recMembers.count - 1 do { an advanced record's }
          printDecl(PDecl(listGet(ts^.recMembers, i)));
        indent := indent - 2;
      end;
    tsPointer:
      begin
        lineName('PointerTypeSpec ''', ts^.tname.name);
        loc(ts^.b);
      end;
    tsSet, tsFile:
      begin
        if ts^.kind = tsSet then
          line('SetTypeSpec')
        else if ts^.elemSpec = nil then
          line('FileTypeSpec untyped')
        else
          line('FileTypeSpec');
        loc(ts^.b);
        indent := indent + 2;
        if ts^.elemSpec <> nil then
          printTypeSpec(ts^.elemSpec);
        indent := indent - 2;
      end;
    tsRoutine:
      begin
        if ts^.rtIsFunction then
          line('RoutineTypeSpec function')
        else
          line('RoutineTypeSpec procedure');
        if ts^.rtOfObject then
          bufAdd(outBuf, ' of object');
        if ts^.rtCallConv <> ccDefault then
          bufAdd(outBuf, ' ' + callConvName(ts^.rtCallConv));
        loc(ts^.b);
        indent := indent + 2;
        for i := 0 to ts^.rtParams.count - 1 do
          printDecl(PDecl(listGet(ts^.rtParams, i)));
        if ts^.rtResult <> nil then
        begin
          line('Result');
          loc(ts^.rtResult^.b);
          indent := indent + 2;
          printTypeSpec(ts^.rtResult);
          indent := indent - 2;
        end;
        indent := indent - 2;
      end;
    tsClassRef:
      begin
        lineName('ClassRefTypeSpec ''', ts^.tname.name);
        loc(ts^.b);
      end;
    tsString:
      begin
        if ts^.isLong then
          line('StringTypeSpec long')
        else
          line('StringTypeSpec');
        loc(ts^.b);
        if ts^.maxLen <> nil then
        begin
          indent := indent + 2;
          visitExpr(ts^.maxLen);
          indent := indent - 2;
        end;
      end;
    tsClass:
      begin
        if ts^.isForwardClass then
          line('ClassTypeSpec forward')
        else
          line('ClassTypeSpec');
        if identValid(ts^.parentName) then
        begin
          bufAdd(outBuf, ' ''');
          bufAddName(outBuf, ts^.parentName.name);
          bufAddChar(outBuf, '''');
        end;
        for i := 0 to ts^.interfaceNames.count - 1 do
        begin
          bufAdd(outBuf, ', ''');
          bufAddName(outBuf, PIdent(listGet(ts^.interfaceNames, i))^.name);
          bufAddChar(outBuf, '''');
        end;
        loc(ts^.b);
        indent := indent + 2;
        for i := 0 to ts^.memberList.count - 1 do
          printDecl(PDecl(listGet(ts^.memberList, i)));
        indent := indent - 2;
      end;
    tsInterface:
      begin
        if ts^.isForwardIntf then
          line('InterfaceTypeSpec forward')
        else
          line('InterfaceTypeSpec');
        if not ts^.isCorba then
          bufAdd(outBuf, ' com');
        if identValid(ts^.intfParentName) then
        begin
          bufAdd(outBuf, ' ''');
          bufAddName(outBuf, ts^.intfParentName.name);
          bufAddChar(outBuf, '''');
        end;
        if (ts^.guid <> nil) and (ts^.guid^.len > 0) then
        begin
          bufAddChar(outBuf, ' ');
          quoted(ts^.guid);
        end;
        loc(ts^.b);
        indent := indent + 2;
        for i := 0 to ts^.intfMemberList.count - 1 do
          printDecl(PDecl(listGet(ts^.intfMemberList, i)));
        indent := indent - 2;
      end;
    tsHelper:
      begin
        if ts^.helperIsRecord then
          line('RecordHelperTypeSpec')
        else
          line('HelperTypeSpec');
        if identValid(ts^.helperParent) then
        begin
          bufAdd(outBuf, ' ''');
          bufAddName(outBuf, ts^.helperParent.name);
          bufAddChar(outBuf, '''');
        end;
        loc(ts^.b);
        indent := indent + 2;
        line('For');
        indent := indent + 2;
        printTypeSpec(ts^.helperFor);
        indent := indent - 2;
        for i := 0 to ts^.helperMembers.count - 1 do
          printDecl(PDecl(listGet(ts^.helperMembers, i)));
        indent := indent - 2;
      end;
  end;
end;

procedure printUses(const usesList: TPtrList);
var
  i: longint;
  id: PIdent;
begin
  for i := 0 to usesList.count - 1 do
  begin
    id := PIdent(listGet(usesList, i));
    lineName('Uses ''', id^.name);
    loc(id^.loc);
  end;
end;

{ " : T" for a declaration's type: the resolved type, else the name of a
  named type spec }
procedure declTypeSuffix(ts: PTypeSpec; t: PType);
begin
  if t <> nil then
    typeSuffix(t)
  else if (ts <> nil) and (ts^.kind = tsNamed) then
  begin
    bufAdd(outBuf, ' : ''');
    bufAddName(outBuf, ts^.tname.spelling);
    bufAddChar(outBuf, '''');
  end;
end;

procedure printDecl(d: PDecl);
var
  i: longint;
  kindPrefix: string;
begin
  case d^.kind of
    dkConst:
      begin
        lineName('ConstDecl ''', d^.ident.name);
        typeSuffix(d^.constType);
        printConstValue(d^.constValue);
        hintsSuffix(d);
        loc(d^.loc);
        { before Sema the constant has no value: show its expression }
        if (d^.constType = nil) and (d^.constInit <> nil) then
        begin
          indent := indent + 2;
          visitExpr(d^.constInit);
          indent := indent - 2;
        end;
      end;
    dkType:
      begin
        lineName('TypeDecl ''', d^.ident.name);
        if d^.declType <> nil then
        begin
          bufAdd(outBuf, ' = ');
          addDescribedType(d^.declType);
        end;
        hintsSuffix(d);
        loc(d^.loc);
        if parseTree then
        begin
          indent := indent + 2;
          printTypeSpec(d^.typeSpec);
          indent := indent - 2;
        end;
      end;
    dkVar:
      begin
        case d^.varKind of
          vkVarParam:
            if d^.isOut then
              lineName('ParamDecl out ''', d^.ident.name)
            else
              lineName('ParamDecl var ''', d^.ident.name);
          vkConstParam:
            if d^.isConstRef then
              lineName('ParamDecl constref ''', d^.ident.name)
            else
              lineName('ParamDecl const ''', d^.ident.name);
          vkValueParam: lineName('ParamDecl ''', d^.ident.name);
        else
          lineName('VarDecl ''', d^.ident.name);
        end;
        if not parseTree then
        begin
          if d^.varType <> nil then
            typeSuffix(d^.varType)
          else if (d^.varSpec <> nil) and (d^.varSpec^.kind = tsNamed) then
          begin
            bufAdd(outBuf, ' : ''');
            bufAddName(outBuf, d^.varSpec^.tname.spelling);
            bufAddChar(outBuf, '''');
          end;
          if d^.isClassVar then
            bufAdd(outBuf, ' class');
          if d^.isInitVar then
            bufAdd(outBuf, ' initialized');
          if d^.absTarget <> nil then
            bufAdd(outBuf, ' absolute');
          if d^.paramDefault <> nil then
            bufAdd(outBuf, ' default');
          visibilitySuffix(d);
          loc(d^.loc);
          exit;
        end;
        if d^.isClassVar then
          bufAdd(outBuf, ' class');
        if d^.isInitVar then
          bufAdd(outBuf, ' initialized');
        if d^.absTarget <> nil then
          bufAdd(outBuf, ' absolute');
        if d^.paramDefault <> nil then
          bufAdd(outBuf, ' default');
        visibilitySuffix(d);
        loc(d^.loc);
        indent := indent + 2;
        printTypeSpec(d^.varSpec);
        if d^.paramDefault <> nil then
        begin
          line('Default');
          loc(d^.paramDefault^.b);
          indent := indent + 2;
          visitExpr(d^.paramDefault);
          indent := indent - 2;
        end;
        if d^.absTarget <> nil then
        begin
          line('Absolute');
          loc(d^.absTarget^.b);
          indent := indent + 2;
          visitExpr(d^.absTarget);
          indent := indent - 2;
        end;
        if d^.init <> nil then
        begin
          line('Init');
          loc(d^.init^.b);
          indent := indent + 2;
          visitExpr(d^.init);
          indent := indent - 2;
        end;
        indent := indent - 2;
      end;
    dkRoutine:
      begin
        if d^.classInit = ciConstructor then
          kindPrefix := 'ClassConstructorDecl '''
        else if d^.classInit = ciDestructor then
          kindPrefix := 'ClassDestructorDecl '''
        else if d^.methodKind = mtConstructor then
          kindPrefix := 'ConstructorDecl '''
        else if d^.methodKind = mtDestructor then
          kindPrefix := 'DestructorDecl '''
        else if d^.methodKind = mtOperator then
          kindPrefix := 'OperatorDecl '''
        else if d^.isFunction then
          kindPrefix := 'FunctionDecl '''
        else
          kindPrefix := 'ProcedureDecl ''';
        line(kindPrefix);
        for i := 0 to d^.outerClassNames.count - 1 do
        begin
          bufAddName(outBuf, PIdent(listGet(d^.outerClassNames, i))^.name);
          bufAddChar(outBuf, '.');
        end;
        if identValid(d^.methodClassName) then
        begin
          bufAddName(outBuf, d^.methodClassName.name);
          bufAddChar(outBuf, '.');
        end;
        bufAddName(outBuf, d^.ident.name);
        bufAddChar(outBuf, '''');
        typeSuffix(d^.resultType);
        if d^.isClassMethod and (d^.classInit = ciNone) then
          bufAdd(outBuf, ' class');
        visibilitySuffix(d);
        if d^.isVirtual then
          bufAdd(outBuf, ' virtual');
        if d^.isOverride then
          bufAdd(outBuf, ' override');
        if d^.isAbstract then
          bufAdd(outBuf, ' abstract');
        if d^.hasOverloadDirective then
          bufAdd(outBuf, ' overload');
        if d^.isInline then
          bufAdd(outBuf, ' inline');
        if d^.callConv <> ccDefault then
          bufAdd(outBuf, ' ' + callConvName(d^.callConv));
        overloadNumber(d);
        if d^.isForward then
          bufAdd(outBuf, ' forward');
        if d^.isExternal then
        begin
          bufAdd(outBuf, ' external');
          { (a symbol name given as a constant: known after Sema) }
          if (d^.externalName <> nil) and (d^.externalName^.len > 0) then
          begin
            bufAdd(outBuf, ' ''');
            bufAddName(outBuf, d^.externalName);
            bufAddChar(outBuf, '''');
          end
          else if d^.nameExpr = nil then
          begin
            bufAdd(outBuf, ' ''');
            bufAddName(outBuf, d^.ident.spelling);
            bufAddChar(outBuf, '''');
          end;
          if (d^.libraryName <> nil) and (d^.libraryName^.len > 0) then
          begin
            bufAdd(outBuf, ' in ''');
            bufAddName(outBuf, d^.libraryName);
            bufAddChar(outBuf, '''');
          end;
          if d^.isVarArgs then
            bufAdd(outBuf, ' varargs');
        end;
        if d^.canonical <> nil then
          bufAdd(outBuf, ' definition');
        loc(d^.loc);
        indent := indent + 2;
        if d^.canonical = nil then
          for i := 0 to d^.params.count - 1 do
            printDecl(PDecl(listGet(d^.params, i)));
        if parseTree and (d^.resultSpec <> nil) then
        begin
          line('Result');
          loc(d^.resultSpec^.b);
          indent := indent + 2;
          printTypeSpec(d^.resultSpec);
          indent := indent - 2;
        end;
        { external library and name constants (not literals) }
        if parseTree and (d^.libraryExpr <> nil) and
           (d^.libraryExpr^.kind <> nkStringLit) then
        begin
          line('ExternalLibrary');
          loc(d^.libraryExpr^.b);
          indent := indent + 2;
          visitExpr(d^.libraryExpr);
          indent := indent - 2;
        end;
        if parseTree and (d^.nameExpr <> nil) and
           (d^.nameExpr^.kind <> nkStringLit) then
        begin
          line('ExternalName');
          loc(d^.nameExpr^.b);
          indent := indent + 2;
          visitExpr(d^.nameExpr);
          indent := indent - 2;
        end;
        if (d^.body <> nil) and not d^.isForward then
          printBlock(d^.body);
        indent := indent - 2;
      end;
    dkLabel:
      begin
        line('LabelDecl ');
        bufAddName(outBuf, d^.ident.name);
        loc(d^.loc);
      end;
    dkField: { a field of a class }
      begin
        lineName('FieldDecl ''', d^.ident.name);
        if not parseTree then
          declTypeSuffix(d^.fieldSpec, d^.fieldType);
        visibilitySuffix(d);
        loc(d^.loc);
        if parseTree then
        begin
          indent := indent + 2;
          printTypeSpec(d^.fieldSpec);
          indent := indent - 2;
        end;
      end;
    dkProperty:
      begin
        lineName('PropertyDecl ''', d^.ident.name);
        if not parseTree then
          declTypeSuffix(d^.propSpec, d^.propType);
        if identValid(d^.readName) then
        begin
          bufAdd(outBuf, ' read ''');
          bufAddName(outBuf, d^.readName.name);
          bufAddChar(outBuf, '''');
        end;
        if identValid(d^.writeName) then
        begin
          bufAdd(outBuf, ' write ''');
          bufAddName(outBuf, d^.writeName.name);
          bufAddChar(outBuf, '''');
        end;
        if d^.propIsDefault then
          bufAdd(outBuf, ' default');
        if d^.propNoDefault then
          bufAdd(outBuf, ' nodefault');
        visibilitySuffix(d);
        loc(d^.loc);
        indent := indent + 2;
        for i := 0 to d^.propParams.count - 1 do { an array property's indexes }
          printDecl(PDecl(listGet(d^.propParams, i)));
        if parseTree then
          printTypeSpec(d^.propSpec);
        if d^.propIndex <> nil then
        begin
          line('PropertyIndex');
          loc(d^.propIndex^.b);
          indent := indent + 2;
          visitExpr(d^.propIndex);
          indent := indent - 2;
        end;
        if d^.propDefaultValue <> nil then
        begin
          line('PropertyDefault');
          loc(d^.propDefaultValue^.b);
          indent := indent + 2;
          visitExpr(d^.propDefaultValue);
          indent := indent - 2;
        end;
        indent := indent - 2;
      end;
  else
    begin
      lineName('Decl ''', d^.ident.name);
      loc(d^.loc);
    end;
  end;
end;

procedure printBlock(b: PBlock);
var
  i: longint;
begin
  for i := 0 to b^.decls.count - 1 do
    printDecl(PDecl(listGet(b^.decls, i)));
  if b^.body <> nil then
    visitStmt(b^.body);
end;

procedure printUnit(u: PDecl);
var
  i: longint;
begin
  lineName('UnitDecl ''', u^.ident.name);
  loc(u^.loc);
  indent := indent + 2;
  line('Interface');
  loc(u^.loc);
  indent := indent + 2;
  printUses(u^.ifaceUses);
  for i := 0 to u^.iface^.decls.count - 1 do
    printDecl(PDecl(listGet(u^.iface^.decls, i)));
  indent := indent - 2;
  line('Implementation');
  loc(u^.loc);
  indent := indent + 2;
  printUses(u^.implUses);
  printBlock(u^.impl);
  indent := indent - 4;
end;

procedure printProgram(p: PDecl);
var
  i: longint;
  id: PIdent;
begin
  lineName('ProgramDecl ''', p^.ident.name);
  loc(p^.loc);
  indent := indent + 2;
  for i := 0 to p^.progParams.count - 1 do
  begin
    id := PIdent(listGet(p^.progParams, i));
    lineName('ProgramParam ''', id^.name);
    loc(id^.loc);
  end;
  if parseTree then
    printUses(p^.progUses);
  if p^.progBlock <> nil then
    printBlock(p^.progBlock);
  indent := indent - 2;
end;

{ --- Statements --- }

procedure visitStmtList(const l: TPtrList);
var
  i: longint;
begin
  for i := 0 to l.count - 1 do
    visitStmt(PNode(listGet(l, i)));
end;

{ try (objfpc): TryStmt / TryBody stmts / ExceptPart [OnHandler...
  ExceptElse] stmts, or FinallyPart stmts }
procedure visitTry(s: PNode);
var
  i: longint;
  h: PExceptHandler;
begin
  line('TryStmt');
  loc(s^.b);
  indent := indent + 2;
  line('TryBody');
  loc(s^.b);
  indent := indent + 2;
  visitStmtList(s^.tryBody);
  indent := indent - 2;
  if s^.isFinally then
    line('FinallyPart')
  else
    line('ExceptPart');
  loc(s^.partLoc);
  indent := indent + 2;
  for i := 0 to s^.handlers.count - 1 do
  begin
    h := PExceptHandler(listGet(s^.handlers, i));
    line('OnHandler');
    if identValid(h^.hVarName) then
    begin
      bufAdd(outBuf, ' ''');
      bufAddName(outBuf, h^.hVarName.name);
      bufAdd(outBuf, ''':');
    end;
    bufAdd(outBuf, ' ''');
    bufAddName(outBuf, h^.hClassName.name);
    bufAddChar(outBuf, '''');
    loc(h^.onLoc);
    indent := indent + 2;
    visitStmt(h^.hBody);
    indent := indent - 2;
  end;
  if s^.tryElseLoc <> noLoc then
  begin
    line('ExceptElse');
    loc(s^.tryElseLoc);
    indent := indent + 2;
    visitStmtList(s^.partBody);
    indent := indent - 2;
  end
  else
    visitStmtList(s^.partBody);
  indent := indent - 4;
end;

procedure visitCase(s: PNode);
var
  i, j: longint;
  arm: PCaseArm;
  l: PCaseLabel;
begin
  line('CaseStmt');
  loc(s^.b);
  indent := indent + 2;
  visitExpr(s^.selector);
  for i := 0 to s^.arms.count - 1 do
  begin
    arm := PCaseArm(listGet(s^.arms, i));
    line('CaseArm');
    if arm^.labels.count = 0 then
      loc(arm^.body^.b)
    else
      loc(PCaseLabel(listGet(arm^.labels, 0))^.lo^.b);
    indent := indent + 2;
    for j := 0 to arm^.labels.count - 1 do
    begin
      l := PCaseLabel(listGet(arm^.labels, j));
      if l^.hi <> nil then
        line('CaseLabelRange')
      else
        line('CaseLabel');
      loc(l^.lo^.b);
      indent := indent + 2;
      visitExpr(l^.lo);
      if l^.hi <> nil then
        visitExpr(l^.hi);
      indent := indent - 2;
    end;
    visitStmt(arm^.body);
    indent := indent - 2;
  end;
  if s^.elseLoc <> noLoc then
  begin
    line('CaseElse');
    loc(s^.elseLoc);
    indent := indent + 2;
    visitStmtList(s^.elseBody);
    indent := indent - 2;
  end;
  indent := indent - 2;
end;

procedure visitStmt(s: PNode);
var
  i: longint;
begin
  case s^.kind of
    nkEmpty:
      begin
        line('EmptyStmt');
        loc(s^.b);
      end;
    nkCompound:
      begin
        line('CompoundStmt');
        loc(s^.b);
        indent := indent + 2;
        visitStmtList(s^.stmts);
        indent := indent - 2;
      end;
    nkProcCall:
      begin
        lineName('ProcCallStmt ''', s^.callee.name);
        if parseTree and s^.hasParens then
          bufAdd(outBuf, ' ()');
        overloadNumber(s^.callDecl);
        loc(s^.b);
        indent := indent + 2;
        for i := 0 to s^.args.count - 1 do
          visitExpr(PNode(listGet(s^.args, i)));
        indent := indent - 2;
      end;
    nkIndirectCallStmt:
      begin
        line('IndirectCallStmt');
        loc(s^.b);
        indent := indent + 2;
        visitExpr(s^.calleeExpr);
        indent := indent - 2;
      end;
    nkAssign:
      begin
        if s^.assignCompound then
          line('AssignStmt compound')
        else
          line('AssignStmt');
        loc(s^.b);
        indent := indent + 2;
        visitExpr(s^.target);
        visitExpr(s^.value);
        indent := indent - 2;
      end;
    nkIf:
      begin
        if s^.elseStmt <> nil then
          line('IfStmt has_else')
        else
          line('IfStmt');
        loc(s^.b);
        indent := indent + 2;
        visitExpr(s^.cond);
        visitStmt(s^.thenStmt);
        if s^.elseStmt <> nil then
          visitStmt(s^.elseStmt);
        indent := indent - 2;
      end;
    nkWhile:
      begin
        line('WhileStmt');
        loc(s^.b);
        indent := indent + 2;
        visitExpr(s^.cond);
        visitStmt(s^.thenStmt);
        indent := indent - 2;
      end;
    nkRepeat:
      begin
        line('RepeatStmt');
        loc(s^.b);
        indent := indent + 2;
        visitStmtList(s^.stmts);
        visitExpr(s^.untilCond);
        indent := indent - 2;
      end;
    nkFor:
      begin
        if s^.isDownto then
          line('ForStmt downto')
        else
          line('ForStmt to');
        loc(s^.b);
        indent := indent + 2;
        visitExpr(s^.forVar);
        visitExpr(s^.forInit);
        visitExpr(s^.forFinal);
        visitStmt(s^.forBody);
        indent := indent - 2;
      end;
    nkForIn:
      begin
        line('ForInStmt');
        loc(s^.b);
        indent := indent + 2;
        visitExpr(s^.inVar);
        visitExpr(s^.inColl);
        visitStmt(s^.inBody);
        indent := indent - 2;
      end;
    nkCase:
      visitCase(s);
    nkTry:
      visitTry(s);
    nkRaise:
      begin
        line('RaiseStmt');
        loc(s^.b);
        if s^.raiseObj <> nil then
        begin
          indent := indent + 2;
          visitExpr(s^.raiseObj);
          indent := indent - 2;
        end;
      end;
    nkLabeled:
      begin
        line('LabeledStmt ');
        bufAddName(outBuf, s^.lbl.name);
        loc(s^.b);
        indent := indent + 2;
        visitStmt(s^.lblBody);
        indent := indent - 2;
      end;
    nkGoto:
      begin
        line('GotoStmt ');
        bufAddName(outBuf, s^.lbl.name);
        if (s^.lblDecl <> nil) and s^.lblDecl^.labelNonLocal then
          bufAdd(outBuf, ' non-local');
        loc(s^.b);
      end;
    nkWith:
      begin
        line('WithStmt');
        loc(s^.b);
        indent := indent + 2;
        for i := 0 to s^.withRecords.count - 1 do
          visitExpr(PWithContext(listGet(s^.withRecords, i))^.rec);
        visitStmt(s^.withBody);
        indent := indent - 2;
      end;
  else
    ;
  end;
end;

{ --- Expressions --- }

procedure visitExpr(e: PNode);
var
  i: longint;
  el: PAggElement;
  se: PSetElement;
begin
  case e^.kind of
    nkIntegerLit:
      begin
        line('IntegerLiteralExpr ' + uintToStr(e^.intValue));
        endExpr(e);
      end;
    nkRealLit:
      begin
        line('RealLiteralExpr ' + formatG(e^.realValue, 6));
        endExpr(e);
      end;
    nkStringLit:
      if e^.strWide <> 0 then
      begin
        line('WideCharLiteralExpr #' + intToStr(e^.strWide));
        endExpr(e);
      end
      else
      begin
        if e^.strValue^.len = 1 then
          line('CharLiteralExpr ')
        else
          line('StringLiteralExpr ');
        quoted(e^.strValue);
        endExpr(e);
      end;
    nkNameRef:
      begin
        lineName('NameRefExpr ''', e^.ref.name);
        if e^.refDecl <> nil then
          bufAdd(outBuf, ' ' + declKindName(e^.refDecl));
        overloadNumber(e^.refDecl);
        if e^.refWith <> nil then
        begin
          if e^.refWith^.isSelf then
            bufAdd(outBuf, ' (self)')
          else
            bufAdd(outBuf, ' (with)');
        end;
        endExpr(e);
      end;
    nkInherited:
      begin
        line('InheritedExpr');
        if identValid(e^.inhName) then
        begin
          bufAdd(outBuf, ' ''');
          bufAddName(outBuf, e^.inhName.name);
          bufAddChar(outBuf, '''');
        end;
        endExpr(e);
      end;
    nkParen:
      begin
        line('ParenExpr');
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.inner);
        indent := indent - 2;
      end;
    nkUnary:
      begin
        line('UnaryExpr ''' + unaryOpSpelling(e^.uop) + '''');
        operatorOf(e^.uopDecl, false);
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.operand);
        indent := indent - 2;
      end;
    nkBinary:
      begin
        line('BinaryExpr ''' + binaryOpSpelling(e^.bop) + '''');
        operatorOf(e^.opDecl, e^.opNegated);
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.lhs);
        visitExpr(e^.rhs);
        indent := indent - 2;
      end;
    nkCall:
      begin
        lineName('CallExpr ''', e^.callee.name);
        overloadNumber(e^.callDecl);
        endExpr(e);
        indent := indent + 2;
        for i := 0 to e^.args.count - 1 do
          visitExpr(PNode(listGet(e^.args, i)));
        indent := indent - 2;
      end;
    nkIndirectCall:
      begin
        line('IndirectCallExpr');
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.calleeExpr);
        for i := 0 to e^.args.count - 1 do
          visitExpr(PNode(listGet(e^.args, i)));
        indent := indent - 2;
      end;
    nkFormatted:
      begin
        line('FormattedExpr');
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.fmtValue);
        visitExpr(e^.fmtWidth);
        if e^.fmtPrecision <> nil then
          visitExpr(e^.fmtPrecision);
        indent := indent - 2;
      end;
    nkIndex:
      begin
        line('IndexExpr');
        if e^.idxProp <> nil then { objfpc: an array property }
        begin
          bufAdd(outBuf, ' property ''');
          bufAddName(outBuf, e^.idxProp^.ident.name);
          bufAddChar(outBuf, '''');
        end;
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.base);
        visitExpr(e^.index);
        indent := indent - 2;
      end;
    nkField:
      begin
        lineName('FieldExpr ''', e^.fieldName.name);
        overloadNumber(e^.fieldDecl);
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.base);
        indent := indent - 2;
      end;
    nkAggregate:
      begin
        line('AggregateExpr');
        endExpr(e);
        indent := indent + 2;
        for i := 0 to e^.aggElems.count - 1 do
        begin
          el := PAggElement(listGet(e^.aggElems, i));
          if identValid(el^.field) then
          begin
            lineName('Field ''', el^.field.name);
            loc(el^.field.loc);
            indent := indent + 2;
            visitExpr(el^.value);
            indent := indent - 2;
          end
          else
            visitExpr(el^.value);
        end;
        indent := indent - 2;
      end;
    nkAddrOf:
      begin
        line('AddrOfExpr');
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.inner);
        indent := indent - 2;
      end;
    nkNil:
      begin
        line('NilExpr');
        endExpr(e);
      end;
    nkDeref:
      begin
        line('DerefExpr');
        endExpr(e);
        indent := indent + 2;
        visitExpr(e^.inner);
        indent := indent - 2;
      end;
    nkSetCtor:
      begin
        line('SetCtorExpr');
        endExpr(e);
        indent := indent + 2;
        for i := 0 to e^.setElems.count - 1 do
        begin
          se := PSetElement(listGet(e^.setElems, i));
          visitExpr(se^.lo);
          if se^.hi <> nil then
            visitExpr(se^.hi);
        end;
        indent := indent - 2;
      end;
  else
    ;
  end;
end;

procedure printParseTree(var outText: TBuf; prog: PDecl; const units: TPtrList);
var
  i: longint;
begin
  outBuf := outText;
  indent := 0;
  parseTree := true;
  for i := 0 to units.count - 1 do
    printUnit(PDecl(listGet(units, i)));
  printProgram(prog);
  outText := outBuf;
end;

procedure printAST(var outText: TBuf; prog: PDecl);
begin
  outBuf := outText;
  indent := 0;
  parseTree := false;
  printProgram(prog);
  outText := outBuf;
end;

end.
