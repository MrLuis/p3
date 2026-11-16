unit pcast;
{ The abstract syntax tree of the self-hosted compiler; the Pascal
  counterpart of include/pcc/AST/AST.h.

  Expressions and statements are one record type, TNode, with a variant
  part per kind; declarations are TDecl, written types TTypeSpec. All nodes
  are allocated in the arena (pcutil) and never freed. Lists of nodes are
  TPtrList.

  Fields that Sema fills in (types, resolved declarations, constant
  values) are marked (Sema). Semantic types are TType (Type.h): integer
  types, real, boolean, char, enumerations, subranges, string literals,
  arrays, records, pointers, nil, sets, files, procedural types and short
  strings, and (--mode=objfpc) classes. Types are compared by identity
  (name equivalence). }

interface

uses pcsys, pcutil, pcdiag;

{$I builtins.inc}

type
  { a name as written: name is the lower-case interned name (nil when
    missing), spelling the text as written }
  TIdent = record
    name: PName;
    spelling: PName;
    loc: TLoc;
  end;
  PIdent = ^TIdent;

  TConstKind = (ckNone, ckOrdinal, ckReal, ckString);
  TConstValue = record
    kind: TConstKind;
    intVal: int64;
    realVal: double;
    strVal: PName;
  end;

  TUnaryOp = (uoPlus, uoMinus, uoNot);
  TBinaryOp = (boAdd, boSub, boOr, boXor, boMul, boRealDiv, boIntDiv, boMod,
               boAnd, boShl, boShr, boEq, boNe, boLt, boLe, boGt, boGe, boIn,
               boIs, boAs);

  TNodeKind = (
    { expressions }
    nkIntegerLit, nkRealLit, nkStringLit, nkNameRef, nkParen, nkUnary,
    nkBinary, nkCall, nkFormatted, nkIndex, nkField, nkNil, nkDeref,
    nkSetCtor, nkAddrOf, nkAggregate, nkIndirectCall, nkInherited, nkMove,
    nkTemplate, nkTuple, nkNamedArg, nkCondExpr,
    { statements }
    nkEmpty, nkCompound, nkProcCall, nkAssign, nkIf, nkWhile, nkRepeat,
    nkFor, nkCase, nkWith, nkLabeled, nkGoto, nkIndirectCallStmt,
    nkTry, nkRaise, nkForIn, nkVarDecl, nkDestructure);

  PNode = ^TNode;
  PDecl = ^TDecl;
  PTypeSpec = ^TTypeSpec;
  PBlock = ^TBlock;
  PType = ^TType;
  PWithContext = ^TWithContext;
  PFieldList = ^TFieldList;

  TTypeKind = (tyError, tyInteger, tyReal, tyBoolean, tyChar, tyEnum,
               tySubrange, tyStringLit, tyArray, tyRecord, tyPointer, tyNil,
               tySet, tyFile, tyRoutine, tyShortString, tyAnsiString, tyArrayOfConst, tyOpenArray, tyDynArray, tyClass, tyClassRef,
               tyInterface,
               { an untyped parameter's (Turbo/FPC  var x;  const x) }
               tyUntyped,
               { widechar (Turbo/FPC): a 16-bit character code }
               tyWideChar);

  TType = record
    kind: TTypeKind;
    id: longint;     { unique: maps keyed by type use it }
    declName: PName; { first name given in a type declaration, or nil }
    case TTypeKind of
      tyInteger: (bits: longint; isSigned: boolean; intName: PName);
      tyEnum: (enumValues: TPtrList); { of PDecl (dkConst) }
      tySubrange: (host: PType; lo, hi: int64);
      tyStringLit: (strLength: int64);
      { isBitPacked...: bitpacked (Free Pascal): ordinal fields and
        elements take bitPackedWidth bits each, without addresses }
      tyArray: (indexType, elementType: PType; isPackedArray: boolean;
                isBitPackedArray: boolean);
      { memberTable: an advanced record's (objfpc): a class type that is
        not a type of values, holding the fields, methods, operators and
        properties (its classRecord is the record); nil for a plain record }
      { recordPackRecords: $packrecords N where declared (0: natural; see
        fieldAlignment) }
      tyRecord: (fieldList: PFieldList; allFields: TPtrList; { of PDecl }
                 isPackedRecord: boolean; memberTable: PType;
                 isBitPackedRecord: boolean; recordPackRecords: longint;
                 { a tuple type: (integer, string) }
                 isTupleRecord: boolean);
      tyPointer: (pointee: PType; untyped: boolean);
      tySet: (setElement: PType); { nil: the empty set [] }
      tyFile: (fileElement: PType; isText: boolean);
      { routineReference: reference to (objfpc), a closure: its value is
        the same pair as a procedural value's, (function, static link),
        but the link is nil or a frame on the heap, reference counted: the
        type is managed }
      tyRoutine: (routineParams: TPtrList; routineResult: PType;
                  routineCDecl, routineOfObject, routineReference: boolean);
      tyShortString: (maxLength: longint);
      tyArrayOfConst: (aocElement: PType); { TVarRec }
      tyOpenArray: (openElement: PType);
      tyDynArray: (dynElement: PType);
      { a class (objfpc): its parent (nil only for TObject), own members in
        source order, all fields (the ancestors' first), the virtual methods
        by slot, the program or unit declaring it; a forward declaration
        is not defined yet }
      { classInterfaces: the interfaces the class implements (listed by
        it or an ancestor), of PInterfaceImpl (Sema) }
      { classRecord: the advanced record whose member table this is (nil
        for a class): no parent, VMT or instances; its methods' Self is the
        record (by reference), its class methods and operators have none }
      { classOuter: the class (or advanced record's member table) whose type
        section declares this one (objfpc nested types); nil at program or
        unit level }
      { classHelperFor: a type helper's (objfpc): the type it extends; a
        class type that is not a type of values, its methods' Self is a
        value of that type (by reference); classRecordHelper: a record
        helper (not a type helper) }
      tyClass: (classParent: PType; classMembers, classAllFields,
                classVTable: TPtrList; classModule: pointer;
                classDefined: boolean; classInterfaces: TPtrList;
                classRecord: PType; classOuter: PType; classHelperFor: PType;
                classRecordHelper: boolean;
                { its class constructor and class destructor (not members),
                  or nil }
                classCtor, classDtor: PDecl);
      { class of C (objfpc): a reference to C or a descendant (its VMT) }
      tyClassRef: (refClass: PType);
      { interface (objfpc): its parent, own methods (source order) and all
        methods by slot (the parent's first); a value is (object, method
        table). COM interfaces (IUnknown and its descendants) are
        reference counted, CORBA ones are not. }
      tyInterface: (intfParent: PType; intfMethods, intfAllMethods: TPtrList;
                    intfModule: pointer; intfDefined: boolean;
                    intfCOM: boolean);
  end;

  TNode = record
    kind: TNodeKind;
    b, e: TLoc;      { source range [b, e) }
    typ: PType;      { expressions: the type (Sema) }
    { a user-defined conversion (objfpc advanced records): the := or
      Explicit operator convOp converts the value to typ; convFrom is the
      type it had; convSuspended while code generation computes it }
    convOp: PDecl;
    convFrom: PType;
    convSuspended: boolean;
    { an argument for a constref parameter that is a variable of the
      parameter's type: its address is passed (else a temporary's) (Sema) }
    constRefAddr: boolean;
    case TNodeKind of
      { intBased: written $FF, %1010 or &17 (beyond int64: the bits of an
        int64, as in Free Pascal, not a qword) }
      nkIntegerLit: (intValue: int64; intBased: boolean);
      nkRealLit: (realValue: double);
      { strWide: a lone character code above 255 (#$D800), a widechar
        value (strValue is then '?', the char it converts to); else 0 }
      nkStringLit: (strValue: PName; strWide: longint);
      nkNameRef: (ref: TIdent; refDecl: PDecl; refWith: PWithContext);
      { paren: the inner expression; deref: the pointer; addrof: the operand;
        move (objfpc): move p, the owned variable p (a name) }
      { addrLambda: a lambda, function(x) => e (objfpc): the hidden
        routine the parser made of it, which inner names; Sema completes
        it where the type wanted is known }
      nkParen, nkDeref, nkAddrOf, nkMove: (inner: PNode; addrMethod, addrCode: PDecl;
                                         addrLambda: PDecl);
      { opDecl: an advanced record's class operator implementing it
        (Sema); opNegated: <> through the = operator }
      nkUnary: (uop: TUnaryOp; operand: PNode; uopDecl: PDecl);
      nkBinary: (bop: TBinaryOp; opLoc: TLoc; lhs, rhs: PNode;
                 operandType: PType; opDecl: PDecl; opNegated: boolean);
      { call / proccall: callee named; indirectcall: calleeExpr; the
        indirect call statement holds an nkIndirectCall in calleeExpr }
      { callWith: a method found through a class scope is called on the
        object of this with context (Self, or a with statement's) }
      nkCall, nkProcCall, nkIndirectCall, nkIndirectCallStmt:
        (callValue: PNode; callee: TIdent; args: TPtrList; hasParens: boolean;
         { specialize F<T1, T2>(args) (objfpc): the type arguments, of
           PTypeSpec }
         typeArgs: TPtrList;
         calleeExpr: PNode; callDecl: PDecl; callWith: PWithContext;
         { T(x) of an untyped parameter x (Sema): a variable of type T at
           x's address }
         isVarCast: boolean;
         { TMethod(m): 0 no, 1 of a value, 2 of a variable }
         methodCast: longint);
      nkFormatted: (fmtValue, fmtWidth, fmtPrecision: PNode);
      { a template string with interpolations (tp, objfpc), an ansistring
        made of its text and of its values, each as write writes it:
        text and values in turn, from the first text to the last (string
        literals, which may be empty; a value with a field width is an
        nkFormatted), of PNode }
      nkTemplate: (tmplParts: TPtrList);
      { (a, b + 1) (objfpc): a tuple value, or the targets of (a, b) := e.
        tupElems: of PNode; tupLong: H+ there (a string constant is an
        ansistring); tupTemp: the hidden variable it is built in (Sema) }
      nkTuple: (tupElems: TPtrList; tupLong: boolean; tupTemp: PDecl);
      { naName := naValue among the arguments of a call (objfpc): the
        argument for the parameter naName. Sema puts the arguments of such
        a call in the order of the parameters, without these nodes }
      nkNamedArg: (naName: TIdent; naValue: PNode);
      { if ceCond then ceThen else ceElse, as an expression (objfpc); only
        the branch chosen is evaluated. ceLong: H+ there. Sema: ceLowered,
        the statement if ceCond then tmp := ceThen else tmp := ceElse for
        a hidden variable tmp, which code generation runs, and ceResult,
        tmp, the value }
      nkCondExpr: (ceCond, ceThen, ceElse: PNode; ceLong: boolean;
                   ceLowered, ceResult: PNode);
      { index: base[index]; field: base.fieldName; fieldDecl is a field,
        or (objfpc) a method or property of a class }
      { objfpc: an array property (obj.P[i, j], P[i] or obj[i], the default
        property) is the outermost nkIndex of the chain ((root[i])[j]);
        Sema sets the property (idxProp), the chain's root and the
        indexes }
      { constRef (field, Sema): a constant member (TFoo.C, obj.C), a name
        reference to it that code generation uses instead }
      { helperValue (field, Sema): a type helper's member used on a value
        (not a variable): Self is a temporary }
      nkIndex, nkField: (base, index: PNode; fieldName: TIdent;
                         fieldDecl: PDecl; idxProp: PDecl; idxRoot: PNode;
                         idxArgs: TPtrList; constRef: PNode;
                         helperValue: boolean);
      { inherited [inhName] (objfpc): the method called and the method
        calling it (Sema); a bare inherited passes the calling method's
        parameters (inhArgs, Sema) }
      nkInherited: (inhName: TIdent; inhMethod, inhEnclosing: PDecl;
                    inhArgs: TPtrList);
      nkSetCtor: (setElems: TPtrList);      { of PSetElement }
      nkAggregate: (aggElems: TPtrList);    { of PAggElement }
      { compound: stmts; repeat: stmts until untilCond }
      nkCompound, nkRepeat: (stmts: TPtrList; untilCond: PNode);
      { assignCompound: written x += e (-=, *=, /=; Free Pascal's C
        operators): value is the binary x + (e), x parsed again }
      nkAssign: (target, value: PNode; assignLoc: TLoc;
                 assignCompound: boolean);
      { if: cond then thenStmt else elseStmt; while: cond do thenStmt }
      nkIf, nkWhile: (cond, thenStmt, elseStmt: PNode);
      { forInline (objfpc): for var x ..., the variable the statement
        declares, visible in the loop only }
      nkFor: (forVar, forInit, forFinal, forBody: PNode; isDownto: boolean;
              forInline: PDecl);
      nkCase: (selector: PNode; arms: TPtrList; elseLoc: TLoc;
               elseBody: TPtrList);
      nkWith: (withRecords: TPtrList; withBody: PNode); { of PWithContext }
      { labeled: lbl: lblBody; goto lbl }
      nkLabeled, nkGoto: (lbl: TIdent; lblBody: PNode; lblDecl: PDecl);
      { try (objfpc): tryBody, then the except part (handlers, of
        PExceptHandler, and partBody: without handlers the statements
        handling every exception, else the else part, if tryElseLoc is
        valid) or the finally part (isFinally, partBody) }
      nkTry: (tryBody: TPtrList; isFinally: boolean; partLoc: TLoc;
              handlers: TPtrList; tryElseLoc: TLoc; partBody: TPtrList);
      { raise raiseObj [at raiseAddr [, raiseFrame]]; raiseObj nil: a
        re-raise }
      nkRaise: (raiseObj, raiseAddr, raiseFrame: PNode);
      { for inVar in inColl do inBody (Turbo/FPC). Sema classifies the
        collection (inKind, a fik* constant) and builds the loop out of
        ordinary nodes over hidden variables, which code generation binds:
        inStep is inVar := <element> (inElemVar, or inEnumVar.Current); for
        an enumerator, inGetEnum is inCollVar.GetEnumerator and inMoveNext
        inEnumVar.MoveNext. inElemType: not for an enumerator; nil: [] }
      nkForIn: (inVar, inColl, inBody: PNode; inLoc: TLoc; inKind: longint;
                inElemType: PType; inElemVar, inCollVar, inEnumVar: PDecl;
                inStep, inGetEnum, inMoveNext: PNode;
                inInline: PDecl); { for var x in ... (objfpc) }
      { A variable declared among the statements (objfpc, as in Delphi):
        var x: T;  var x, y: T;  var x: T := e;  var x := e;  visible to
        the end of the statement sequence it is in. vdVars: of PDecl;
        vdInit: e, or nil; vdLong: H+ there (a string constant gives an
        ansistring); vdAssign: x := e (Sema) }
      { vdConst: const x := e, x cannot be changed afterwards }
      nkVarDecl: (vdVars: TPtrList; vdInit: PNode; vdAssignLoc: TLoc;
                  vdLong: boolean; vdAssign: PNode; vdConst: boolean);
      { (a, b) := e;  var (a, b) := e;  (objfpc): the elements of a tuple
        assigned to variables, or to new ones. dsTargets: of PNode (Sema
        makes them for var (a, b)); dsVars: of PDecl, empty unless var.
        Sema: dsInit, the value kept in a hidden variable (nil: dsValue is
        a tuple written out, built in its own), and dsAssigns, each target
        assigned (of PNode) }
      nkDestructure: (dsTargets, dsVars: TPtrList; dsValue: PNode;
                      dsAssignLoc: TLoc; dsLong: boolean; dsInit: PNode;
                      dsAssigns: TPtrList);
  end;

  { on [hVarName:] hClassName do hBody (objfpc); hVar (a dkVar, if
    named) and hClass: Sema }
  { an interface a class supports: the methods implementing the
    interface's methods, by slot (of PDecl) }
  PInterfaceImpl = ^TInterfaceImpl;
  TInterfaceImpl = record
    intf: PType;
    methods: TPtrList;
  end;

  PExceptHandler = ^TExceptHandler;
  TExceptHandler = record
    onLoc: TLoc;
    hVarName, hClassName: TIdent;
    hBody: PNode;
    hVar: PDecl;
    hClass: PType;
  end;

  PSetElement = ^TSetElement;
  TSetElement = record
    lo, hi: PNode; { hi: nil unless a range }
  end;

  PAggElement = ^TAggElement;
  TAggElement = record
    field: TIdent;       { name = nil for positional elements }
    value: PNode;        { an expression or a nested aggregate }
    folded: TConstValue; { Sema }
  end;

  PCaseLabel = ^TCaseLabel;
  TCaseLabel = record
    lo, hi: PNode; { hi: nil unless a range }
    loValue, hiValue: int64; { Sema }
  end;

  PCaseArm = ^TCaseArm;
  TCaseArm = record
    labels: TPtrList; { of PCaseLabel }
    body: PNode;
  end;

  { A record of a with statement, or (objfpc) an object: of a with
    statement, or Self in a method body (isSelf). }
  TWithContext = record
    rec: PNode;
    isSelf: boolean;
    helper: PType; { Self in a method of this type helper }
    { the routine whose with statement (or whose Self) this is (Sema) }
    routine: PDecl;
    cgAddr: pointer; { code generation: the record's address (a boxed value) }
  end;

  TTypeSpecKind = (tsNamed, tsEnum, tsSubrange, tsArray, tsRecord,
                   tsPointer, tsSet, tsFile, tsRoutine, tsString, tsClass,
                   tsClassRef, tsInterface, tsHelper,
                   tsUntyped, { an untyped parameter's (absent) type }
                   tsArrayOfConst, tsOpenArray, tsDynArray, tsTuple,
                   tsSpecialize);

  PVariantPart = ^TVariantPart;

  TTypeSpec = record
    kind: TTypeSpecKind;
    b, e: TLoc;
    resolved: PType; { Sema }
    case TTypeSpecKind of
      { named: the type name; pointer: the pointee's name; class ref
        (objfpc): class of tname }
      { tquals (named, objfpc): the classes or records enclosing tname,
        outermost first (TOuter.TInner), of PIdent }
      tsNamed, tsPointer, tsClassRef: (tname: TIdent; tquals: TPtrList);
      { enumScoped: declared under $scopedenums on (the values only as
        TColor.Red) }
      { enumTuple (objfpc, two names or more): the tuple type this is if
        every name is the name of a type (Sema decides); not printed }
      tsEnum: (enumNames: TPtrList; enumScoped: boolean; { of PIdent }
               enumTuple: PTypeSpec);
      { a tuple type (objfpc): (integer, string), (Min: integer, Max:
        integer). tupNames: of PIdent (name = nil: none, the element is
        Item1, Item2, ...); tupSpecs: of PTypeSpec }
      tsTuple: (tupNames, tupSpecs: TPtrList);
      { specialize spName<spArgs> (objfpc): the type made of the generic
        type spName for these type arguments (of PTypeSpec) }
      tsSpecialize: (spName: TIdent; spArgs: TPtrList);
      tsSubrange: (lo, hi: PNode);
      tsArray: (indexes: TPtrList; element: PTypeSpec; arrPacked: boolean;
                arrBitPacked: boolean);
      { recMembers: an advanced record's methods, class operators and
        properties (objfpc), in source order }
      { recPackRecords: $packrecords N where declared (0: natural) }
      tsRecord: (fields: PFieldList; recPacked: boolean; recMembers: TPtrList;
                 recBitPacked: boolean; recPackRecords: longint);
      tsSet, tsFile: (elemSpec: PTypeSpec);
      tsRoutine: (rtIsFunction: boolean; rtParams: TPtrList; rtResult: PTypeSpec;
                  rtCallConv: longint; rtOfObject: boolean;
                  rtReference: boolean); { reference to (objfpc): a closure }
      tsOpenArray: (openSpec: PTypeSpec);
      tsDynArray: (dynSpec: PTypeSpec);
      tsString: (maxLen: PNode; isLong: boolean); { nil for plain string; isLong: under H+ }
      { class (objfpc): the parent's name (nil name: TObject), the members
        (fields, method headings, properties) in source order }
      { (interfaceNames: the rest of class(P, I1, I2), of PIdent) }
      tsClass: (parentName: TIdent; memberList: TPtrList; isForwardClass: boolean;
                interfaceNames: TPtrList);
      { interface (objfpc): the parent's name, method headings, forward,
        the GUID string (no effect; nil: none), declared under
        $interfaces corba (else COM) }
      tsInterface: (intfParentName: TIdent; intfMemberList: TPtrList;
                    isForwardIntf: boolean; guid: PName; isCorba: boolean);
      { type helper [(helperParent)] for helperFor (objfpc): methods,
        properties and constants; helperIsRecord: a record helper (record
        helper ... for a record type) }
      tsHelper: (helperParent: TIdent; helperFor: PTypeSpec;
                 helperMembers: TPtrList; helperIsRecord: boolean);
  end;

  TFieldList = record
    fixed: TPtrList; { of PDecl (dkField) }
    variant: PVariantPart;
  end;

  PVariant = ^TVariant;
  TVariant = record
    labels: TPtrList; { of PCaseLabel }
    fields: PFieldList;
  end;

  TVariantPart = record
    tag: PDecl;         { the tag field, if named }
    tagSpec: PTypeSpec;
    tagType: PType;     { Sema }
    variants: TPtrList; { of PVariant }
    caseLoc: TLoc;
  end;

  TBlock = record
    decls: TPtrList; { of PDecl, in source order }
    body: PNode;     { nkCompound }
  end;

  TDeclKind = (dkProgram, dkVar, dkConst, dkType, dkField, dkRoutine,
               dkBuiltin, dkLabel, dkUnit, dkProperty);
  TVarKind = (vkVariable, vkValueParam, vkVarParam, vkConstParam, vkResult);
  { objfpc: the kind of a method, the methods of TObject (no Pascal body),
    the visibility of a class member }
  { mtOperator: an advanced record's class operator (a static function) }
  TMethodKind = (mtNone, mtMethod, mtConstructor, mtDestructor, mtOperator);
  TObjMethod = (obNone, obCreate, obDestroy, obFree, obClassName,
                obExceptionCreate, obExceptionCreateFmt, obClassType, obClassParent, obInheritsFrom,
                obInstanceSize);
  { strict private: visible only in the class's methods; strict
    protected: also in its descendants' (not in the rest of the module) }
  TVisibility = (vsPublic, vsPublished, vsProtected, vsPrivate,
                 vsStrictProtected, vsStrictPrivate);

  TDecl = record
    kind: TDeclKind;
    ident: TIdent;
    loc: TLoc;          { noLoc for predeclared entities }
    { Free Pascal's hint directives (hintDeprecated, ...): each use of the
      entity is warned about; hintMessage: deprecated's, or nil }
    hints: longint;
    hintMessage: PName;
    { Sema: referenced anywhere; a function whose result is assigned; the
      number of labels in this routine targeted by non-local gotos }
    isUsed, resultAssigned: boolean;
    nonLocalLabels: longint;
    { Sema: a routine's variable that a routine nested in it may use (it
      lives in the routine's frame record, which nested routines reach
      through their static link); a routine with a try statement (an
      exception continues in it after a _longjmp, so code generation
      keeps all its variables in memory) }
    captured, hasTry: boolean;
    { code generation (pcgen): a static variable's address (a boxed value),
      a routine's function, frame type and jmp_buf slot, the frame slot of
      a local, a field's byte offset, a label's basic block }
    cgValue, cgFunc, cgFrame, cgBlock, cgDbg: pointer;
    { a routine's variable: in its frame record (slot cgSlot), else in
      an alloca (cgLocal, a boxed value); a routine whose frame record
      starts with its static link }
    cgInFrame, cgLinkInFrame: boolean;
    cgLocal: pointer;
    { a cdecl routine's C entry (cthunk, a PIRFunc), or nil }
    cgThunk: pointer;
    { separate units: the external symbol of an interface variable or
      routine (nil: none), and whether its unit was compiled separately }
    cgExport: PName;
    cgImported: boolean;
    cgSlot, cgJmpSlot: longint;
    cgOffset: int64;
    cgBitOffset: int64; { an ordinal field of a bitpacked record: its bit }
    vis: TVisibility; { class members }
    case TDeclKind of
      { owner: the routine whose frame holds it; nil for program-level
        variables and typed constants (Sema) }
      { isClassVar (objfpc): a class var, static storage for the class
        varClass (Sema) and its descendants }
      { absTarget (Turbo/FPC): x: T absolute y: the expression y (x has no
        storage of its own, it is another view of y); absRoot: the variable
        y is in (Sema) }
      { isInitVar (objfpc): init belongs to an initialized variable
        (var x: T = v;), an ordinary variable set to its initial value
        whenever its routine is entered, not to a typed constant }
      dkVar: (varSpec: PTypeSpec; varType: PType; varKind: TVarKind;
              owner: PDecl; init: PNode; initValue: TConstValue;
              isClassVar: boolean; varClass: PType; isInitVar: boolean;
              absTarget: PNode; absRoot: PDecl;
              { objfpc: x: TFoo owned, a routine's variable that owns its
                object, which is freed when the variable gets another one
                and when the routine ends; move x gives the object up }
              isOwned: boolean;
              { objfpc: declared by a var statement (nkVarDecl) or by
                for var: its scope is the rest of the statement sequence,
                or the loop. Sema adds it to its block's declarations,
                after the others }
              isInlineVar: boolean;
              { const x := e among the statements (objfpc): set once,
                where it is declared, and not changed afterwards (Sema) }
              isReadOnly: boolean;
              { objfpc: a parameter's default value (b: integer = 1),
                passed by the calls that leave it out }
              paramDefault: PNode;
              { objfpc: an out parameter, a var parameter (vkVarParam) whose
                managed values the caller releases before the call }
              isOut: boolean;
              { Turbo/FPC: a constref parameter, a const one (vkConstParam)
                always passed by reference: the caller's variable, or a
                temporary }
              isConstRef: boolean;
              { Turbo/FPC  x: T; [cvar;] external ['lib'] [name 'sym'];  a C
                variable (the library and symbol: string constants,
                evaluated by Sema; the symbol: the name as written unless
                given) }
              varExternal, varCVar: boolean; varLibName, varExtName: PName;
              varLibExpr, varNameExpr: PNode);
      dkConst: (constInit: PNode; constType: PType; constValue: TConstValue);
      { tgGeneric: generic TList<T> = ... (objfpc), a template. Its tokens
        (tgTokens, the parser's PGenericTokens), and those of each method
        implementation (TList.Add), are parsed again for every
        specialization (specialize TList<integer>). tgParams: the type
        parameters, of PIdent; tgScope: the scope it is declared in (Sema) }
      dkType: (typeSpec: PTypeSpec; declType: PType; tgGeneric: boolean;
               tgParams: TPtrList; tgTokens, tgScope: pointer);
      dkField: (fieldSpec: PTypeSpec; fieldType: PType);
      { methods (objfpc): methodClassName: TFoo of an implementation
        TFoo.Bar; ownerClass, selfVar (the hidden parameter), vmtIndex (a
        virtual method's slot, else -1) and implDecl (the implementation of
        a method heading): Sema }
      dkRoutine: (isFunction, hasParamList, isForward, isExternal: boolean;
                  params: TPtrList; resultSpec: PTypeSpec; resultType: PType;
                  body: PBlock; externalName: PName;
                  canonical, parent, resultVar: PDecl; level: longint;
                  methodKind: TMethodKind; methodClassName: TIdent;
                  { TOuter of an implementation TOuter.TInner.Bar: the
                    classes or records enclosing methodClassName, outermost
                    first (of PIdent) }
                  outerClassNames: TPtrList;
                  { objfpc: generic function F<T, U>..., a template: its type
                    parameters (of PIdent) and its tokens (the parser's
                    PGenericTokens), parsed again for each specialization;
                    not checked or compiled itself (Sema: the scope it is
                    declared in, and the declaration with its body) }
                  isGeneric: boolean; typeParams: TPtrList;
                  genericTokens, genericScope: pointer; genericDef: PDecl;
                  { the implementation of a generic type's method
                    (TList.Add): a template too, parsed again for each
                    specialization of the type }
                  isTemplateMember: boolean;
                  { the routine of a lambda (function(x) => e): hidden,
                    nested in the routine the lambda is written in, with
                    parameter and result types that Sema may take from where
                    it is used (lambdaDone once it has; lambdaBad: it does
                    not fit there, reported). lambdaCapture: the first name
                    it uses of the routines around it (valid: it must not
                    outlive them) }
                  isLambda, lambdaDone, lambdaBad: boolean;
                  lambdaCapture: TIdent;
                  { isClosure: a lambda made a reference to value; it may
                    outlive the routines around it, whose frames it uses
                    are then on the heap (heapFrame), reference counted }
                  isClosure, heapFrame: boolean;
                  { isMethodLambda: a lambda made a method pointer (of
                    object): a routine with a Self of its own, of the class
                    of the method it is written in, and that method's Self
                    (lambdaSelf; nil outside methods) as its object. It is
                    not a method of the class (methodKind mtNone) but has a
                    Self }
                  isMethodLambda: boolean;
                  lambdaSelf: PDecl;
                  isVirtual, isOverride, isAbstract, isClassMethod: boolean;
                  { overloading (objfpc): the overload directive, the next
                    routine of the name in the same scope or class and this
                    one's number among them (1, 2, ...; 0: not overloaded),
                    which goes into its symbol (Sema) }
                  hasOverloadDirective: boolean; nextOverload: PDecl;
                  overloadIndex: longint;
                  { a method heading of an interface (its slot: vmtIndex) }
                  routineIntf: PType;
                  ownerClass: PType; selfVar, implDecl: PDecl;
                  vmtIndex: longint; objMethod: TObjMethod;
                  { an advanced record's class method or operator: no Self }
                  isStatic: boolean;
                  { a routine or method implementation's static directive
                    (Free Pascal: repeats the declaration's; ignored on
                    plain routines); 0 if none }
                  staticDirLoc: TLoc;
                  { Result names the function's result in its body
                    ($modeswitch result, on in objfpc mode, where the body
                    is) }
                  resultName: boolean;
                  { the inline directive (with $inline on): code generation
                    asks LLVM to inline the routine (inlinehint) }
                  isInline: boolean;
                  { the calling convention written (ccDefault, ccCDecl,
                    ccStdCall); inheritedCDecl: an override of a cdecl
                    method (Sema). See isCDecl. }
                  callConv: longint; inheritedCDecl: boolean;
                  { external [library] [name symbol]: the library to link
                    with (nil: none), resolved by Sema when written as a
                    constant expression (libraryExpr, nameExpr: as
                    written, nil if absent); isVarArgs: 'varargs' }
                  libraryName: PName; libraryExpr, nameExpr: PNode;
                  isVarArgs: boolean;
                  { the body computes values of managed types (COM
                    interfaces): code generation releases the references
                    it holds when an exception leaves the routine (Sema) }
                  usesManaged: boolean;
                  { a class constructor or destructor (objfpc, classes and
                    advanced records; ciNone, ciConstructor, ciDestructor):
                    a static class procedure without parameters that the
                    program or unit declaring the type runs at its start
                    (before its initialization part) or end; not a member
                    (it cannot be called) }
                  classInit: longint);
      dkBuiltin: (builtin: TBuiltinKind; isProcedure: boolean);
      { labels (Sema): the statement labeled, the statement sequence that
        directly contains it, whether that is the block's outermost one,
        and whether a goto from a nested routine targets it }
      dkLabel: (labelOwner: PDecl; labelTarget: PNode; labelSequence: pointer;
                labelTopLevel, labelNonLocal: boolean; labelIndex: longint);
      dkProgram: (progParams: TPtrList; progUses: TPtrList; progBlock: PBlock);
      dkUnit: (ifaceUses, implUses: TPtrList; iface, impl: PBlock);
      { property (objfpc): the accessors' names and declarations (Sema) }
      { an array property's index parameters (passed to the accessors
        first), index N (passed next), default V and nodefault (no effect),
        a default array property (obj[i] means obj.P[i]) }
      dkProperty: (propSpec: PTypeSpec; propType: PType;
                   readName, writeName: TIdent; readDecl, writeDecl: PDecl;
                   propParams: TPtrList; propIndex, propDefaultValue: PNode;
                   propIsDefault, propNoDefault: boolean;
                   { class property (objfpc, classes and advanced records):
                     its accessors are class variables or static class
                     methods, so it is used without an object }
                   propIsClass: boolean);
  end;

const
  { what a for-in loop enumerates (nkForIn's inKind) }
  fikInvalid = 0;
  fikArray = 1;
  fikOpenArray = 2;
  fikDynArray = 3;
  fikAnsiString = 4;
  fikShortString = 5;
  fikStringConst = 6;
  fikSet = 7;
  fikOrdinals = 8;
  fikEnumerator = 9;

  hintDeprecated = 1;
  hintExperimental = 2;
  hintPlatform = 4;
  hintLibrary = 8;
  hintUnimplemented = 16;

  { Calling convention directives (Turbo/FPC), as written. pcc calls every
    routine with the target's C convention (a cdecl routine's procedural
    values point to a C entry without the static link); as in Free Pascal on 64-bit
    targets, 'stdcall' is the default convention and 'cdecl' a different
    one, so headings, implementations, overrides, interface implementations
    and procedural values must agree on whether they are 'cdecl'. }
  ccDefault = 0;
  ccCDecl = 1;
  ccStdCall = 2;
  { a routine's classInit }
  ciNone = 0;
  ciConstructor = 1;
  ciDestructor = 2;

procedure addHints(d: PDecl; flags: longint; msg: PName);
function callConvName(cc: longint): string;
{ Whether routine d is called as 'cdecl' (written, or inherited by an
  override). }
function isCDecl(d: PDecl): boolean;

function newNode(kind: TNodeKind; b, e: TLoc): PNode;
function newDecl(kind: TDeclKind; const id: TIdent; loc: TLoc): PDecl;
function newTypeSpec(kind: TTypeSpecKind; b, e: TLoc): PTypeSpec;
function newIdent(const id: TIdent): PIdent;
function noIdent: TIdent;
function identValid(const id: TIdent): boolean;
function identRange(const id: TIdent): TRange;
function nodeRange(n: PNode): TRange;

function unaryOpSpelling(op: TUnaryOp): string;
function binaryOpSpelling(op: TBinaryOp): string;
function isRelational(op: TBinaryOp): boolean;

implementation

function newNode(kind: TNodeKind; b, e: TLoc): PNode;
var
  n: PNode;
begin
  n := PNode(arenaAlloc(sizeof(TNode)));
  n^.kind := kind;
  n^.b := b;
  n^.e := e;
  newNode := n;
end;

function callConvName(cc: longint): string;
begin
  if cc = ccCDecl then
    callConvName := 'cdecl'
  else if cc = ccStdCall then
    callConvName := 'stdcall'
  else
    callConvName := '';
end;

function isCDecl(d: PDecl): boolean;
begin
  isCDecl := (d^.callConv = ccCDecl) or d^.inheritedCDecl;
end;

procedure addHints(d: PDecl; flags: longint; msg: PName);
begin
  d^.hints := d^.hints or flags;
  if msg <> nil then
    d^.hintMessage := msg;
end;

function newDecl(kind: TDeclKind; const id: TIdent; loc: TLoc): PDecl;
var
  d: PDecl;
begin
  d := PDecl(arenaAlloc(sizeof(TDecl)));
  d^.kind := kind;
  d^.ident := id;
  d^.loc := loc;
  newDecl := d;
end;

function newTypeSpec(kind: TTypeSpecKind; b, e: TLoc): PTypeSpec;
var
  t: PTypeSpec;
begin
  t := PTypeSpec(arenaAlloc(sizeof(TTypeSpec)));
  t^.kind := kind;
  t^.b := b;
  t^.e := e;
  newTypeSpec := t;
end;

function newIdent(const id: TIdent): PIdent;
var
  p: PIdent;
begin
  p := PIdent(arenaAlloc(sizeof(TIdent)));
  p^ := id;
  newIdent := p;
end;

function noIdent: TIdent;
var
  id: TIdent;
begin
  id.name := nil;
  id.spelling := nil;
  id.loc := noLoc;
  noIdent := id;
end;

function identValid(const id: TIdent): boolean;
begin
  identValid := id.name <> nil;
end;

function identRange(const id: TIdent): TRange;
var
  len: longint;
begin
  len := 0;
  if id.spelling <> nil then
    len := id.spelling^.len;
  identRange := makeRange(id.loc, id.loc + len);
end;

function nodeRange(n: PNode): TRange;
begin
  nodeRange := makeRange(n^.b, n^.e);
end;

function unaryOpSpelling(op: TUnaryOp): string;
begin
  case op of
    uoPlus: unaryOpSpelling := '+';
    uoMinus: unaryOpSpelling := '-';
    uoNot: unaryOpSpelling := 'not';
  end;
end;

function binaryOpSpelling(op: TBinaryOp): string;
begin
  case op of
    boAdd: binaryOpSpelling := '+';
    boSub: binaryOpSpelling := '-';
    boOr: binaryOpSpelling := 'or';
    boXor: binaryOpSpelling := 'xor';
    boShl: binaryOpSpelling := 'shl';
    boShr: binaryOpSpelling := 'shr';
    boMul: binaryOpSpelling := '*';
    boRealDiv: binaryOpSpelling := '/';
    boIntDiv: binaryOpSpelling := 'div';
    boMod: binaryOpSpelling := 'mod';
    boAnd: binaryOpSpelling := 'and';
    boEq: binaryOpSpelling := '=';
    boNe: binaryOpSpelling := '<>';
    boLt: binaryOpSpelling := '<';
    boLe: binaryOpSpelling := '<=';
    boGt: binaryOpSpelling := '>';
    boGe: binaryOpSpelling := '>=';
    boIn: binaryOpSpelling := 'in';
    boIs: binaryOpSpelling := 'is';
    boAs: binaryOpSpelling := 'as';
  end;
end;

function isRelational(op: TBinaryOp): boolean;
begin
  isRelational := op in [boEq, boNe, boLt, boLe, boGt, boGe, boIn, boIs];
end;

end.
