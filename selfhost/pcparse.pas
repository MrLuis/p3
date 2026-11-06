unit pcparse;
{ The recursive-descent parser of the self-hosted compiler; a port of
  src/parse/Parser.cpp. It must produce the same tree and report the same
  diagnostics, with the same error recovery, as the C++ parser.

  One parser runs at a time (units are parsed one after another), so its
  state is kept in this unit's variables. }

interface

uses pcsys, pcutil, pcdiag, pclex, pcast;

{ Parses a whole program; always returns a (possibly partial) tree. }
function parseProgram(fid: longint; mode: TLangMode): PDecl;
{ Parses a unit file (Turbo/FPC); nil on a fatal syntax error. }
function parseUnit(fid: longint; mode: TLangMode): PDecl;
{ The main input: a unit if it starts with 'unit' (Turbo/FPC; isUnit is
  set, and the unit is parsed only if parseUnits), else a program. }
function parseInput(fid: longint; mode: TLangMode; parseUnits: boolean;
                    var isUnit: boolean): PDecl;

implementation

type
  TTokenSet = set of TTokenKind;

const
  maxNesting = 256;

var
  lx: TLexer;
  mode: TLangMode;
  { expressions, statements and types being parsed; deeper than maxNesting
    is a fatal error instead of a stack overflow }
  nesting: longint;
  tok, peekTok: TToken;
  hasPeek: boolean;
  prevTokEnd: TLoc;

{ --- Token helpers --- }

procedure startParser(fid: longint; m: TLangMode);
begin
  mode := m;
  lexInit(lx, fid, m);
  hasPeek := false;
  prevTokEnd := noLoc;
  nesting := 0;
  lexNext(lx, tok);
end;

function consumeToken: TLoc;
begin
  consumeToken := tok.loc;
  prevTokEnd := tokenEnd(tok);
  if hasPeek then
  begin
    tok := peekTok;
    hasPeek := false;
  end
  else
    lexNext(lx, tok);
end;

procedure peekToken;
begin
  if not hasPeek then
  begin
    lexNext(lx, peekTok);
    hasPeek := true;
  end;
end;

function peekKind: TTokenKind;
begin
  peekToken;
  peekKind := peekTok.kind;
end;

function tryConsume(k: TTokenKind): boolean;
begin
  if tok.kind <> k then
    exit(false);
  consumeToken;
  tryConsume := true;
end;

function describe(k: TTokenKind): string;
begin
  case k of
    tkIdentifier: describe := 'identifier';
    tkIntegerLiteral: describe := 'integer';
    tkRealLiteral: describe := 'real number';
    tkStringLiteral: describe := 'string';
    tkEof: describe := 'end of file';
  else
    if tokenSpellings[k] <> '' then
      describe := '''' + tokenSpellings[k] + ''''
    else
      describe := tokenNames[k];
  end;
end;

{ If the current token is k, consumes it. Otherwise reports "expected 'k'
  <context>" right after the previous token and returns false. }
function expect(k: TTokenKind; const context: string): boolean;
var
  where: TLoc;
  msg: string;
begin
  if tryConsume(k) then
    exit(true);
  if prevTokEnd <> noLoc then
    where := prevTokEnd
  else
    where := tok.loc;
  msg := 'expected ' + describe(k);
  if context <> '' then
    msg := msg + ' ' + context;
  diagError(where, msg);
  expect := false;
end;

procedure skipUntil(stop: TTokenSet);
begin
  while tok.kind <> tkEof do
  begin
    if tok.kind in stop then
      exit;
    consumeToken;
  end;
end;

function tokenSpelling(const t: TToken): PName;
var
  fid: longint;
begin
  fid := smFileOf(t.loc);
  tokenSpelling := internChars(smFileData(fid), t.loc - smFileStart(fid), t.len);
end;

function makeIdentifier(const t: TToken): TIdent;
var
  id: TIdent;
begin
  id.name := t.name;
  id.spelling := tokenSpelling(t);
  id.loc := t.loc;
  makeIdentifier := id;
end;

{ Labels compare by value: 0010 and 10 are the same label. }
function makeLabel(const t: TToken): TIdent;
var
  id: TIdent;
begin
  id.name := intern(uintToStr(t.intValue));
  id.spelling := tokenSpelling(t);
  id.loc := t.loc;
  makeLabel := id;
end;

function expectIdentifier(var id: TIdent; const what: string): boolean;
begin
  if tok.kind <> tkIdentifier then
  begin
    diagError(tok.loc, 'expected ' + what);
    exit(false);
  end;
  id := makeIdentifier(tok);
  consumeToken;
  expectIdentifier := true;
end;

{ Appends PIdent items to names. }
function parseIdentList(var names: TPtrList; const what: string): boolean;
var
  id: TIdent;
begin
  repeat
    if not expectIdentifier(id, what) then
      exit(false);
    listAdd(names, newIdent(id));
  until not tryConsume(tkComma);
  parseIdentList := true;
end;

procedure unsupported(const what: string);
begin
  diagErrorR(tok.loc, what + ' not supported yet', tokenRange(tok));
end;

function startsStatement: boolean;
begin
  startsStatement := tok.kind in [tkIdentifier, tkKwBegin, tkKwIf, tkKwWhile,
                                  tkKwRepeat, tkKwFor, tkKwCase, tkKwWith,
                                  tkKwGoto, tkIntegerLiteral];
end;

function startsDeclarationPart: boolean;
begin
  startsDeclarationPart := tok.kind in [tkKwLabel, tkKwConst, tkKwType,
                                        tkKwVar, tkKwProcedure, tkKwFunction];
end;

{ Enters one nesting level; false (after a fatal error) if too deep. }
function enterNesting: boolean;
begin
  if nesting >= maxNesting then
  begin
    if not diagShouldStop then
      diagFatal(tok.loc, 'nesting too deep (more than ' + intToStr(maxNesting) +
                ' levels of expressions, statements or types)');
    exit(false);
  end;
  nesting := nesting + 1;
  enterNesting := true;
end;

function isIdentifierNamed(const s: string): boolean;
begin
  isIdentifierNamed := (tok.kind = tkIdentifier) and nameEquals(tok.name, s);
end;

function rawText(const t: TToken): string;
begin
  rawText := tokenText(t);
end;

{ --- Forward declarations of the mutually recursive parts --- }

{ A function without parameters cannot call itself by name in the dialect
  (FPC's objfpc mode reads the name as its result): these helpers re-enter
  parseTypedConstValue, parseType, parseStatement and parseFactor. }
function nestedTypedConstValue: PNode; forward;
function nestedType: PTypeSpec; forward;
function nestedStatement: PNode; forward;
function nestedFactor: PNode; forward;

function parseBlock: PBlock; forward;
procedure parseDeclarations(var decls: TPtrList; inInterface: boolean); forward;
function parseType: PTypeSpec; forward;
function parseSimpleType: PTypeSpec; forward;
function parseStatement: PNode; forward;
function parseCompoundStmt: PNode; forward;
procedure parseStatementSequence(var body: TPtrList; terminator: TTokenKind;
                                 openLoc: TLoc; const openWord: string); forward;
function parseExpression: PNode; forward;
function parseSimpleExpression: PNode; forward;
function parseSelectors(base: PNode): PNode; forward;
function parseFormalParams(var params: TPtrList): boolean; forward;
function parseCaseLabel(allowRange: boolean): PCaseLabel; forward;
function parseActualParams(var args: TPtrList; allowFormat: boolean): boolean; forward;

{ --- Program and units --- }

function parseUsesClause(var usesList: TPtrList): boolean;
begin
  consumeToken; { 'uses' }
  if not parseIdentList(usesList, 'unit name') or
     not expect(tkSemi, 'after uses clause') then
  begin
    skipUntil([tkSemi, tkKwBegin, tkKwVar, tkKwConst, tkKwType,
               tkKwProcedure, tkKwFunction]);
    tryConsume(tkSemi);
    exit(false);
  end;
  parseUsesClause := true;
end;

function parseProgramHere: PDecl;
var
  programLoc, lparen: TLoc;
  name: TIdent;
  params, usesList: TPtrList;
  b: PBlock;
  p: PDecl;
begin
  programLoc := tok.loc;
  name := noIdent;
  listInit(params);
  listInit(usesList);
  if not tryConsume(tkKwProgram) then
  begin
    diagError(tok.loc, 'expected ''program''');
    { recover by looking for the program's block }
    while (tok.kind <> tkEof) and (tok.kind <> tkKwBegin) and
          not startsDeclarationPart do
      consumeToken;
  end
  else
  begin
    if tok.kind = tkIdentifier then
    begin
      name := makeIdentifier(tok);
      consumeToken;
    end
    else
      diagError(tok.loc, 'expected program name after ''program''');

    if tok.kind = tkLParen then
    begin
      lparen := consumeToken;
      repeat
        if tok.kind = tkIdentifier then
        begin
          listAdd(params, newIdent(makeIdentifier(tok)));
          consumeToken;
        end
        else
        begin
          diagError(tok.loc, 'expected program parameter name');
          skipUntil([tkComma, tkRParen, tkSemi, tkKwBegin]);
        end;
      until not tryConsume(tkComma);
      if not expect(tkRParen, 'after program parameters') then
        diagNote(lparen, 'to match this ''(''');
    end;

    if not expect(tkSemi, 'after program heading') then
      skipUntil([tkSemi, tkKwBegin, tkKwVar, tkKwConst, tkKwType, tkKwLabel,
                 tkKwProcedure, tkKwFunction]);
    tryConsume(tkSemi);
  end;

  if tok.kind = tkKwUses then
    parseUsesClause(usesList)
  else if isIdentifierNamed('uses') and (peekKind = tkIdentifier) then
  begin
    { 'uses' is not reserved in ISO Pascal }
    diagErrorR(tok.loc, 'units are a Turbo/Free Pascal extension; use --mode=tp',
               tokenRange(tok));
    skipUntil([tkSemi]);
    tryConsume(tkSemi);
  end;

  b := parseBlock;

  if not diagShouldStop then
  begin
    if tok.kind = tkPeriod then
    begin
      consumeToken;
      if tok.kind <> tkEof then
        diagWarning(tok.loc, 'text after end of program is ignored');
    end
    else
      expect(tkPeriod, 'at end of program');
  end;

  p := newDecl(dkProgram, name, programLoc);
  p^.progParams := params;
  p^.progUses := usesList;
  p^.progBlock := b;
  parseProgramHere := p;
end;

function parseProgram(fid: longint; mode: TLangMode): PDecl;
begin
  startParser(fid, mode);
  parseProgram := parseProgramHere;
end;

function newBlock(const decls: TPtrList; body: PNode): PBlock;
var
  b: PBlock;
begin
  b := PBlock(arenaAlloc(sizeof(TBlock)));
  b^.decls := decls;
  b^.body := body;
  newBlock := b;
end;

{ unit = "unit" ident ";" "interface" [usesList] decls
         "implementation" [usesList] decls [("begin"|"initialization") stmts]
         "end" "." . }
function parseUnitHere: PDecl;
var
  unitLoc, initLoc, endLoc: TLoc;
  name: TIdent;
  ifaceUses, implUses, ifaceDecls, implDecls, init, none: TPtrList;
  empty, body: PNode;
  u: PDecl;
begin
  parseUnitHere := nil;
  unitLoc := tok.loc;
  if not tryConsume(tkKwUnit) then
  begin
    diagError(tok.loc, 'expected ''unit''');
    exit;
  end;
  if not expectIdentifier(name, 'unit name') or
     not expect(tkSemi, 'after unit name') then
    exit;
  if not expect(tkKwInterface, 'after unit heading') then
    exit;
  listInit(ifaceUses);
  listInit(implUses);
  if tok.kind = tkKwUses then
    parseUsesClause(ifaceUses);
  listInit(ifaceDecls);
  parseDeclarations(ifaceDecls, true);

  if not expect(tkKwImplementation, 'after the interface part') then
    exit;
  if tok.kind = tkKwUses then
    parseUsesClause(implUses);
  listInit(implDecls);
  parseDeclarations(implDecls, false);

  { optional initialization part }
  listInit(init);
  initLoc := tok.loc;
  if tok.kind in [tkKwBegin, tkKwInitialization] then
  begin
    initLoc := consumeToken;
    parseStatementSequence(init, tkKwEnd, initLoc, 'begin');
  end;
  endLoc := tokenEnd(tok);
  if not expect(tkKwEnd, 'at end of unit') or
     not expect(tkPeriod, 'after ''end''') then
    exit;

  listInit(none);
  empty := newNode(nkCompound, initLoc, initLoc);
  empty^.stmts := none;
  body := newNode(nkCompound, initLoc, endLoc);
  body^.stmts := init;
  u := newDecl(dkUnit, name, unitLoc);
  u^.ifaceUses := ifaceUses;
  u^.implUses := implUses;
  u^.iface := newBlock(ifaceDecls, empty);
  u^.impl := newBlock(implDecls, body);
  parseUnitHere := u;
end;

function parseUnit(fid: longint; mode: TLangMode): PDecl;
begin
  startParser(fid, mode);
  parseUnit := parseUnitHere;
end;

function parseInput(fid: longint; mode: TLangMode; parseUnits: boolean;
                    var isUnit: boolean): PDecl;
begin
  startParser(fid, mode);
  isUnit := tok.kind = tkKwUnit;
  if isUnit and not parseUnits then
    parseInput := nil
  else if isUnit then
    parseInput := parseUnitHere
  else
    parseInput := parseProgramHere;
end;

{ --- Blocks and declarations --- }

{ ISO 7185 6.2.1 order of declaration parts. }
function sectionRank(k: TTokenKind): longint;
begin
  case k of
    tkKwLabel: sectionRank := 0;
    tkKwConst: sectionRank := 1;
    tkKwType: sectionRank := 2;
    tkKwVar: sectionRank := 3;
  else
    sectionRank := 4; { procedure / function }
  end;
end;

function emptyCompound(loc: TLoc): PNode;
var
  n: PNode;
  none: TPtrList;
begin
  listInit(none);
  n := newNode(nkCompound, loc, loc);
  n^.stmts := none;
  emptyCompound := n;
end;

function parseBlock: PBlock;
var
  decls: TPtrList;
  body: PNode;
begin
  listInit(decls);
  parseDeclarations(decls, false);
  body := nil;
  if tok.kind = tkKwBegin then
    body := parseCompoundStmt
  else
  begin
    diagError(tok.loc, 'expected ''begin''');
    skipUntil([tkKwBegin]);
    if tok.kind = tkKwBegin then
      body := parseCompoundStmt;
  end;
  if body = nil then
    body := emptyCompound(tok.loc);
  parseBlock := newBlock(decls, body);
end;

function parseTypedConstValue: PNode; forward;

procedure parseConstPart(var decls: TPtrList);
var
  name: TIdent;
  ts: PTypeSpec;
  init: PNode;
  v, c: PDecl;
begin
  consumeToken; { 'const' }
  if tok.kind <> tkIdentifier then
  begin
    diagError(tok.loc, 'expected constant name after ''const''');
    exit;
  end;
  while (tok.kind = tkIdentifier) and not diagShouldStop do
  begin
    name := makeIdentifier(tok);
    consumeToken;
    if (tok.kind = tkColon) and (mode = lmTP) then
    begin
      { typed constant: const x: T = value; }
      consumeToken;
      ts := parseType;
      init := nil;
      if (ts <> nil) and expect(tkEqual, 'in typed constant') then
        init := parseTypedConstValue;
      if init <> nil then
      begin
        v := newDecl(dkVar, name, name.loc);
        v^.varSpec := ts;
        v^.varKind := vkVariable;
        v^.init := init;
        listAdd(decls, v);
      end
      else
        skipUntil([tkSemi, tkKwBegin]);
      if not expect(tkSemi, 'after typed constant') then
      begin
        skipUntil([tkSemi, tkKwBegin]);
        tryConsume(tkSemi);
      end;
      continue;
    end;
    if (tok.kind = tkColonEqual) or (tok.kind = tkColon) then
    begin
      diagErrorR(tok.loc, 'expected ''='' in constant declaration (typed ' +
                 'constants need --mode=tp)', tokenRange(tok));
      skipUntil([tkSemi, tkKwBegin]);
      tryConsume(tkSemi);
      continue;
    end;
    init := nil;
    if expect(tkEqual, 'after constant name') then
      init := parseExpression;
    if init <> nil then
    begin
      c := newDecl(dkConst, name, name.loc);
      c^.constInit := init;
      listAdd(decls, c);
    end
    else
      skipUntil([tkSemi, tkKwBegin]);
    if not expect(tkSemi, 'after constant declaration') then
    begin
      skipUntil([tkSemi, tkKwBegin]);
      tryConsume(tkSemi);
    end;
  end;
end;

procedure parseTypePart(var decls: TPtrList);
var
  name: TIdent;
  ts: PTypeSpec;
  d: PDecl;
begin
  consumeToken; { 'type' }
  if tok.kind <> tkIdentifier then
  begin
    diagError(tok.loc, 'expected type name after ''type''');
    exit;
  end;
  while (tok.kind = tkIdentifier) and not diagShouldStop do
  begin
    name := makeIdentifier(tok);
    consumeToken;
    ts := nil;
    if expect(tkEqual, 'after type name') then
      ts := parseType;
    if ts <> nil then
    begin
      d := newDecl(dkType, name, name.loc);
      d^.typeSpec := ts;
      listAdd(decls, d);
    end
    else
      skipUntil([tkSemi, tkKwBegin]);
    if not expect(tkSemi, 'after type declaration') then
    begin
      skipUntil([tkSemi, tkKwBegin]);
      tryConsume(tkSemi);
    end;
  end;
end;

function newVar(const id: TIdent; ts: PTypeSpec; vk: TVarKind): PDecl;
var
  v: PDecl;
begin
  v := newDecl(dkVar, id, id.loc);
  v^.varSpec := ts;
  v^.varKind := vk;
  newVar := v;
end;

procedure parseVarPart(var decls: TPtrList);
var
  names: TPtrList;
  ts: PTypeSpec;
  i: longint;
begin
  consumeToken; { 'var' }
  if tok.kind <> tkIdentifier then
  begin
    diagError(tok.loc, 'expected variable name after ''var''');
    exit;
  end;
  while (tok.kind = tkIdentifier) and not diagShouldStop do
  begin
    listInit(names);
    ts := nil;
    if parseIdentList(names, 'variable name') and
       expect(tkColon, 'after variable name') then
      ts := parseType;
    if ts = nil then
      skipUntil([tkSemi, tkKwBegin])
    else
      for i := 0 to names.count - 1 do
        listAdd(decls, newVar(PIdent(listGet(names, i))^, ts, vkVariable));
    if not expect(tkSemi, 'after variable declaration') then
    begin
      skipUntil([tkSemi, tkKwBegin]);
      tryConsume(tkSemi);
    end;
  end;
end;

function newRoutine(loc: TLoc; const name: TIdent; isFunction: boolean;
                    const params: TPtrList; hasParamList: boolean;
                    resultSpec: PTypeSpec; body: PBlock;
                    isForward: boolean): PDecl;
var
  r: PDecl;
begin
  r := newDecl(dkRoutine, name, loc);
  r^.isFunction := isFunction;
  r^.params := params;
  r^.hasParamList := hasParamList;
  r^.resultSpec := resultSpec;
  r^.body := body;
  r^.isForward := isForward;
  r^.isExternal := false;
  r^.externalName := nil;
  r^.level := 1;
  newRoutine := r;
end;

function parseTypeIdentifier(const context: string): PTypeSpec; forward;

{ routineDecl = ("procedure" ident [params] | "function" ident [params]
                [":" typeIdent]) ";" ( "forward" | block ) ";" . }
function parseRoutineDecl(headingOnly: boolean): PDecl;
var
  isFunction, hasParamList, isExternal: boolean;
  kind: string;
  loc: TLoc;
  name: TIdent;
  params: TPtrList;
  resultSpec: PTypeSpec;
  externalName: PName;
  r: PDecl;
  body: PBlock;
begin
  parseRoutineDecl := nil;
  isFunction := tok.kind = tkKwFunction;
  if isFunction then
    kind := 'function'
  else
    kind := 'procedure';
  loc := consumeToken;

  name := noIdent;
  if not expectIdentifier(name, kind + ' name') then
  begin
    skipUntil([tkSemi, tkKwBegin]);
    tryConsume(tkSemi);
  end;

  listInit(params);
  hasParamList := tok.kind = tkLParen;
  if hasParamList and not parseFormalParams(params) then
    skipUntil([tkColon, tkSemi, tkKwBegin]);

  resultSpec := nil;
  if tryConsume(tkColon) then
  begin
    if not isFunction then
      diagError(prevTokEnd, 'a procedure cannot have a result type');
    resultSpec := parseTypeIdentifier('function result type');
  end;

  if not expect(tkSemi, 'after ' + kind + ' heading') then
    skipUntil([tkSemi, tkKwBegin, tkKwVar, tkKwConst, tkKwType]);
  tryConsume(tkSemi);

  { Turbo/FPC directives:  cdecl;  external ['lib'] [name 'symbol']; }
  isExternal := false;
  externalName := nil;
  while mode = lmTP do
  begin
    if isIdentifierNamed('cdecl') then { the only calling convention }
    begin
      consumeToken;
      expect(tkSemi, 'after ''cdecl''');
      continue;
    end;
    if not isIdentifierNamed('external') then
      break;
    consumeToken;
    isExternal := true;
    if tok.kind = tkStringLiteral then { library name: linking is the user's job }
      consumeToken;
    if isIdentifierNamed('name') then
    begin
      consumeToken;
      if tok.kind = tkStringLiteral then
      begin
        externalName := tok.strValue;
        consumeToken;
      end
      else
        diagError(tok.loc, 'expected the external symbol name');
    end;
    expect(tkSemi, 'after ''external''');
  end;
  if isExternal then
  begin
    if not identValid(name) then
      exit;
    r := newRoutine(loc, name, isFunction, params, hasParamList, resultSpec,
                    nil, false);
    r^.isExternal := true;
    r^.externalName := externalName;
    exit(r);
  end;

  if headingOnly then { interface part of a unit: the body comes later }
  begin
    if identValid(name) then
      parseRoutineDecl := newRoutine(loc, name, isFunction, params,
                                     hasParamList, resultSpec, nil, true);
    exit;
  end;

  if isIdentifierNamed('forward') then
  begin
    consumeToken;
    expect(tkSemi, 'after ''forward''');
    if identValid(name) then
      parseRoutineDecl := newRoutine(loc, name, isFunction, params,
                                     hasParamList, resultSpec, nil, true);
    exit;
  end;

  body := parseBlock;
  if not expect(tkSemi, 'after ' + kind + ' body') then
  begin
    skipUntil([tkSemi, tkKwBegin, tkKwProcedure, tkKwFunction]);
    tryConsume(tkSemi);
  end;
  if identValid(name) then
    parseRoutineDecl := newRoutine(loc, name, isFunction, params, hasParamList,
                                   resultSpec, body, false);
end;

procedure parseDeclarations(var decls: TPtrList; inInterface: boolean);
var
  lastRank, rank: longint;
  lastWord, word: string;
  r: PDecl;
  ok: boolean;
begin
  lastRank := -1;
  lastWord := '';
  while startsDeclarationPart and not diagShouldStop do
  begin
    rank := sectionRank(tok.kind);
    word := lowerStr(rawText(tok));
    if mode = lmISO then
    begin
      if rank < lastRank then
        diagErrorR(tok.loc, 'in ISO Pascal, ''' + word +
                   ''' declarations must come before ''' + lastWord +
                   ''' declarations; use --mode=tp to mix them', tokenRange(tok))
      else if (rank = lastRank) and (rank <> 4) then
        diagErrorR(tok.loc, 'ISO Pascal allows only one ''' + word +
                   ''' section per block', tokenRange(tok));
    end;
    if rank >= lastRank then
    begin
      lastRank := rank;
      lastWord := tokenSpellings[tok.kind];
    end;

    case tok.kind of
      tkKwConst: parseConstPart(decls);
      tkKwType: parseTypePart(decls);
      tkKwVar: parseVarPart(decls);
      tkKwProcedure, tkKwFunction:
        begin
          r := parseRoutineDecl(inInterface);
          if r <> nil then
            listAdd(decls, r);
        end;
    else
      begin { label 1, 2, 3; }
        consumeToken;
        repeat
          if tok.kind <> tkIntegerLiteral then
          begin
            diagError(tok.loc, 'expected label number');
            skipUntil([tkSemi, tkKwBegin]);
            break;
          end;
          r := newDecl(dkLabel, makeLabel(tok), tok.loc);
          listAdd(decls, r);
          consumeToken;
          ok := tryConsume(tkComma);
        until not ok;
        if not expect(tkSemi, 'after label declaration') then
        begin
          skipUntil([tkSemi, tkKwBegin]);
          tryConsume(tkSemi);
        end;
      end;
    end;
  end;
end;

// typedConstValue = expression | aggregate .
// aggregate = "(" typedConstValue { "," typedConstValue } ")"
// | "(" ident ":" typedConstValue { ";" ident ":" typedConstValue } ")" .
// A parenthesized scalar (1 + 2) parses as a one-element aggregate.
function parseTypedConstValue: PNode;
var
  lparen: TLoc;
  elems: TPtrList;
  recordForm: boolean;
  field: TIdent;
  v, a: PNode;
  el: PAggElement;
  sep: TTokenKind;
begin
  parseTypedConstValue := nil;
  if tok.kind <> tkLParen then
    exit(parseExpression);
  lparen := consumeToken;
  listInit(elems);
  recordForm := (tok.kind = tkIdentifier) and (peekKind = tkColon);
  if recordForm then
    sep := tkSemi
  else
    sep := tkComma;
  repeat
    if recordForm and (tok.kind = tkRParen) then
      break; { (x: 1; y: 2;) -- trailing ';' }
    field := noIdent;
    if recordForm and (not expectIdentifier(field, 'field name') or
                       not expect(tkColon, 'after field name')) then
      exit;
    v := nestedTypedConstValue;
    if v = nil then
      exit;
    el := PAggElement(arenaAlloc(sizeof(TAggElement)));
    el^.field := field;
    el^.value := v;
    listAdd(elems, el);
  until not tryConsume(sep);
  if not expect(tkRParen, 'after typed constant value') then
  begin
    diagNote(lparen, 'to match this ''(''');
    exit;
  end;
  a := newNode(nkAggregate, lparen, prevTokEnd);
  a^.aggElems := elems;
  parseTypedConstValue := a;
end;

// params = "(" section { ";" section } ")" ;
// section = ["var" | "const"] identList ":" typeIdent .   ("const": TP)
function parseFormalParams(var params: TPtrList): boolean;
var
  lparen, start: TLoc;
  isFunction, isVar, isConst: boolean;
  name: TIdent;
  inner, names: TPtrList;
  resultSpec, ts: PTypeSpec;
  vk: TVarKind;
  i: longint;
begin
  lparen := consumeToken; { '(' }
  repeat
    if tok.kind in [tkKwProcedure, tkKwFunction] then
    begin
      { procedure p(params) / function f(params): T }
      isFunction := tok.kind = tkKwFunction;
      start := consumeToken;
      if not expectIdentifier(name, 'parameter name') then
      begin
        skipUntil([tkSemi, tkRParen]);
        continue;
      end;
      listInit(inner);
      if (tok.kind = tkLParen) and not parseFormalParams(inner) then
      begin
        skipUntil([tkSemi, tkRParen]);
        continue;
      end;
      resultSpec := nil;
      if isFunction then
      begin
        if not expect(tkColon, 'before the result type of a functional parameter') then
        begin
          skipUntil([tkSemi, tkRParen]);
          continue;
        end;
        resultSpec := parseTypeIdentifier('function result type');
        if resultSpec = nil then
        begin
          skipUntil([tkSemi, tkRParen]);
          continue;
        end;
      end;
      ts := newTypeSpec(tsRoutine, start, prevTokEnd);
      ts^.rtIsFunction := isFunction;
      ts^.rtParams := inner;
      ts^.rtResult := resultSpec;
      listAdd(params, newVar(name, ts, vkValueParam));
      continue;
    end;
    isVar := tryConsume(tkKwVar);
    isConst := false;
    if not isVar and (tok.kind = tkKwConst) and (mode = lmTP) then
    begin
      consumeToken;
      isConst := true;
    end;
    listInit(names);
    if not parseIdentList(names, 'parameter name') or
       not expect(tkColon, 'after parameter name') then
    begin
      skipUntil([tkSemi, tkRParen]);
      continue;
    end;
    ts := parseTypeIdentifier('parameter type');
    if ts = nil then
    begin
      skipUntil([tkSemi, tkRParen]);
      continue;
    end;
    if isVar then
      vk := vkVarParam
    else if isConst then
      vk := vkConstParam
    else
      vk := vkValueParam;
    for i := 0 to names.count - 1 do
      listAdd(params, newVar(PIdent(listGet(names, i))^, ts, vk));
  until not tryConsume(tkSemi);

  if not expect(tkRParen, 'after parameters') then
  begin
    diagNote(lparen, 'to match this ''(''');
    exit(false);
  end;
  parseFormalParams := true;
end;

{ --- Types --- }

function namedSpec(const id: TIdent): PTypeSpec;
var
  ts: PTypeSpec;
  r: TRange;
begin
  r := identRange(id);
  ts := newTypeSpec(tsNamed, r.b, r.e);
  ts^.tname := id;
  namedSpec := ts;
end;

function parseArrayType(start: TLoc; packed_: boolean): PTypeSpec; forward;
function parseRecordType(start: TLoc; packed_: boolean): PTypeSpec; forward;

{ type = simpleType | ["packed"] (arrayType | recordType) . }
function parseTypeImpl: PTypeSpec;
var
  start: TLoc;
  isPacked, isFunction, isSet: boolean;
  elem, resultSpec, ts: PTypeSpec;
  maxLen: PNode;
  params: TPtrList;
  target: TIdent;
begin
  parseTypeImpl := nil;
  start := tok.loc;
  isPacked := tryConsume(tkKwPacked);
  case tok.kind of
    tkKwArray:
      exit(parseArrayType(start, isPacked));
    tkKwRecord:
      exit(parseRecordType(start, isPacked));
    tkKwSet, tkKwFile:
      begin
        isSet := tok.kind = tkKwSet;
        consumeToken;
        if isSet then
        begin
          if not expect(tkKwOf, 'after ''set''') then
            exit;
          elem := parseSimpleType;
        end
        else
        begin
          if not expect(tkKwOf, 'after ''file''') then
            exit;
          elem := nestedType;
        end;
        if elem = nil then
          exit;
        if isSet then
          ts := newTypeSpec(tsSet, start, prevTokEnd)
        else
          ts := newTypeSpec(tsFile, start, prevTokEnd);
        ts^.elemSpec := elem;
        exit(ts);
      end;
    tkKwString:
      begin { string [ '[' N ']' ] }
        consumeToken;
        maxLen := nil;
        if tryConsume(tkLSquare) then
        begin
          maxLen := parseExpression;
          if (maxLen = nil) or not expect(tkRSquare, 'after string length') then
            exit;
        end;
        ts := newTypeSpec(tsString, start, prevTokEnd);
        ts^.maxLen := maxLen;
        exit(ts);
      end;
    tkKwProcedure, tkKwFunction:
      { procedural type (Turbo/FPC): procedure(params) / function(params): T }
      if not isPacked and (mode = lmTP) then
      begin
        isFunction := tok.kind = tkKwFunction;
        consumeToken;
        listInit(params);
        if (tok.kind = tkLParen) and not parseFormalParams(params) then
          exit;
        resultSpec := nil;
        if isFunction then
        begin
          if not expect(tkColon, 'before the result type') then
            exit;
          resultSpec := parseTypeIdentifier('function result type');
          if resultSpec = nil then
            exit;
        end;
        ts := newTypeSpec(tsRoutine, start, prevTokEnd);
        ts^.rtIsFunction := isFunction;
        ts^.rtParams := params;
        ts^.rtResult := resultSpec;
        exit(ts);
      end;
    tkCaret:
      if not isPacked then
      begin
        consumeToken;
        if not expectIdentifier(target, 'type name after ''^''') then
          exit;
        ts := newTypeSpec(tsPointer, start, prevTokEnd);
        ts^.tname := target;
        exit(ts);
      end;
  else
    ;
  end;
  if isPacked then
  begin
    diagError(tok.loc, '''packed'' must be followed by ''array'', ''record'' or ''set''');
    exit;
  end;
  parseTypeImpl := parseSimpleType;
end;

function parseType: PTypeSpec;
begin
  if not enterNesting then
    exit(nil);
  parseType := parseTypeImpl;
  nesting := nesting - 1;
end;

{ typeIdent (ISO requires these for parameters and function results) }
function parseTypeIdentifier(const context: string): PTypeSpec;
var
  ts: PTypeSpec;
begin
  parseTypeIdentifier := nil;
  if tok.kind = tkKwString then
  begin
    { Turbo/FPC: string names a type here }
    ts := newTypeSpec(tsString, tok.loc, tokenEnd(tok));
    ts^.maxLen := nil;
    consumeToken;
    exit(ts);
  end;
  if tok.kind = tkIdentifier then
  begin
    ts := namedSpec(makeIdentifier(tok));
    consumeToken;
    exit(ts);
  end;
  if (tok.kind in [tkKwArray, tkKwPacked]) and (context = 'parameter type') then
    unsupported('conformant array parameters are')
  else
    diagError(tok.loc, 'expected type name for ' + context +
                       ' (declare the type in a ''type'' section)');
end;

{ Tokens that can continue a constant expression after an identifier. }
function continuesConstExpr(k: TTokenKind): boolean;
begin
  continuesConstExpr := k in [tkDotdot, tkPlus, tkMinus, tkStar, tkKwDiv,
                              tkKwMod];
end;

{ simpleType = typeIdent | "(" identList ")" | constExpr ".." constExpr . }
function parseSimpleType: PTypeSpec;
var
  start: TLoc;
  names: TPtrList;
  ts: PTypeSpec;
  lo, hi: PNode;
begin
  parseSimpleType := nil;
  start := tok.loc;
  if tok.kind = tkLParen then
  begin
    consumeToken;
    listInit(names);
    if not parseIdentList(names, 'enumeration value name') then
      exit;
    if not expect(tkRParen, 'after enumeration values') then
      exit;
    ts := newTypeSpec(tsEnum, start, prevTokEnd);
    ts^.enumNames := names;
    exit(ts);
  end;
  if (tok.kind = tkIdentifier) and not continuesConstExpr(peekKind) then
  begin
    ts := namedSpec(makeIdentifier(tok));
    consumeToken;
    exit(ts);
  end;
  if not (tok.kind in [tkIdentifier, tkIntegerLiteral, tkStringLiteral,
                       tkPlus, tkMinus]) then
  begin
    diagError(tok.loc, 'expected type');
    exit;
  end;
  { subrange: constant bounds (constant expressions in --mode=tp; Sema
    enforces the ISO form) }
  lo := parseSimpleExpression;
  if lo = nil then
    exit;
  if not expect(tkDotdot, 'in subrange type') then
    exit;
  hi := parseSimpleExpression;
  if hi = nil then
    exit;
  ts := newTypeSpec(tsSubrange, start, prevTokEnd);
  ts^.lo := lo;
  ts^.hi := hi;
  parseSimpleType := ts;
end;

// arrayType = "array" "[" simpleType { "," simpleType } "]" "of" type .
function parseArrayType(start: TLoc; packed_: boolean): PTypeSpec;
var
  lbrack: TLoc;
  indexes: TPtrList;
  idx, elem, ts: PTypeSpec;
begin
  parseArrayType := nil;
  consumeToken; { 'array' }
  lbrack := tok.loc;
  if not expect(tkLSquare, 'after ''array''') then
    exit;
  listInit(indexes);
  repeat
    idx := parseSimpleType;
    if idx = nil then
      exit;
    listAdd(indexes, idx);
  until not tryConsume(tkComma);
  if not expect(tkRSquare, 'after array index types') then
  begin
    diagNote(lbrack, 'to match this ''[''');
    exit;
  end;
  if not expect(tkKwOf, 'after array index types') then
    exit;
  elem := parseType;
  if elem = nil then
    exit;
  ts := newTypeSpec(tsArray, start, prevTokEnd);
  ts^.indexes := indexes;
  ts^.element := elem;
  ts^.arrPacked := packed_;
  parseArrayType := ts;
end;

function parseFieldList(close: TTokenKind): PFieldList; forward;

{ recordType = "record" fieldList "end" . }
function parseRecordType(start: TLoc; packed_: boolean): PTypeSpec;
var
  recordLoc: TLoc;
  fields: PFieldList;
  ts: PTypeSpec;
begin
  parseRecordType := nil;
  recordLoc := consumeToken; { 'record' }
  fields := parseFieldList(tkKwEnd);
  if fields = nil then
    exit;
  if not expect(tkKwEnd, 'at end of record type') then
  begin
    diagNote(recordLoc, 'to match this ''record''');
    exit;
  end;
  ts := newTypeSpec(tsRecord, start, prevTokEnd);
  ts^.fields := fields;
  ts^.recPacked := packed_;
  parseRecordType := ts;
end;

function parseVariantPart(close: TTokenKind): PVariantPart; forward;

// fieldList = [ identList ":" type { ";" identList ":" type } ]
// [ ";" variantPart ] [ ";" ] .
function parseFieldList(close: TTokenKind): PFieldList;
var
  fixed, names: TPtrList;
  ts: PTypeSpec;
  vp: PVariantPart;
  fl: PFieldList;
  f: PDecl;
  i: longint;
begin
  parseFieldList := nil;
  listInit(fixed);
  while tok.kind = tkIdentifier do
  begin
    listInit(names);
    if not parseIdentList(names, 'field name') or
       not expect(tkColon, 'after field name') then
      exit;
    ts := parseType;
    if ts = nil then
      exit;
    for i := 0 to names.count - 1 do
    begin
      f := newDecl(dkField, PIdent(listGet(names, i))^, PIdent(listGet(names, i))^.loc);
      f^.fieldSpec := ts;
      listAdd(fixed, f);
    end;
    if not tryConsume(tkSemi) then
      break;
  end;

  vp := nil;
  if tok.kind = tkKwCase then
  begin
    vp := parseVariantPart(close);
    if vp = nil then
      exit;
    tryConsume(tkSemi);
  end;

  if tok.kind <> close then
  begin
    diagError(tok.loc, 'expected field name, ''case'' or ' + describe(close) +
                       ' in record');
    exit;
  end;
  fl := PFieldList(arenaAlloc(sizeof(TFieldList)));
  fl^.fixed := fixed;
  fl^.variant := vp;
  parseFieldList := fl;
end;

// variantPart = "case" [ ident ":" ] typeIdent "of" variant { ";" variant } .
// variant     = caseLabel { "," caseLabel } ":" "(" fieldList ")" .
function parseVariantPart(close: TTokenKind): PVariantPart;
var
  vp: PVariantPart;
  first: TIdent;
  tagTS: PTypeSpec;
  labels: TPtrList;
  l: PCaseLabel;
  lparen: TLoc;
  fields: PFieldList;
  v: PVariant;
begin
  parseVariantPart := nil;
  vp := PVariantPart(arenaAlloc(sizeof(TVariantPart)));
  listInit(vp^.variants);
  vp^.caseLoc := consumeToken; { 'case' }
  if not expectIdentifier(first, 'variant tag or tag type after ''case''') then
    exit;
  if tryConsume(tkColon) then
  begin
    tagTS := parseTypeIdentifier('variant tag type');
    if tagTS = nil then
      exit;
    vp^.tag := newDecl(dkField, first, first.loc);
    vp^.tag^.fieldSpec := tagTS;
    vp^.tagSpec := tagTS;
  end
  else
    vp^.tagSpec := namedSpec(first);
  if not expect(tkKwOf, 'after variant tag') then
    exit;

  while (tok.kind <> close) and (tok.kind <> tkEof) do
  begin
    listInit(labels);
    repeat
      l := parseCaseLabel(false);
      if l = nil then
        exit;
      listAdd(labels, l);
    until not tryConsume(tkComma);
    if not expect(tkColon, 'after variant label') then
      exit;
    lparen := tok.loc;
    if not expect(tkLParen, 'before variant fields') then
      exit;
    fields := parseFieldList(tkRParen);
    if fields = nil then
      exit;
    if not expect(tkRParen, 'after variant fields') then
    begin
      diagNote(lparen, 'to match this ''(''');
      exit;
    end;
    v := PVariant(arenaAlloc(sizeof(TVariant)));
    v^.labels := labels;
    v^.fields := fields;
    listAdd(vp^.variants, v);
    if not tryConsume(tkSemi) then
      break;
  end;
  if vp^.variants.count = 0 then
  begin
    diagError(tok.loc, 'expected at least one variant');
    exit;
  end;
  parseVariantPart := vp;
end;

{ --- Statements --- }

function emptyStmt(loc: TLoc): PNode;
begin
  emptyStmt := newNode(nkEmpty, loc, loc);
end;

function parseCompoundStmt: PNode;
var
  beginLoc, endLoc: TLoc;
  body: TPtrList;
  n: PNode;
begin
  beginLoc := consumeToken; { 'begin' }
  listInit(body);
  parseStatementSequence(body, tkKwEnd, beginLoc, 'begin');
  endLoc := tokenEnd(tok);
  tryConsume(tkKwEnd);
  n := newNode(nkCompound, beginLoc, endLoc);
  n^.stmts := body;
  parseCompoundStmt := n;
end;

procedure parseStatementSequence(var body: TPtrList; terminator: TTokenKind;
                                 openLoc: TLoc; const openWord: string);
var
  semiLoc: TLoc;
begin
  while not diagShouldStop do
  begin
    listAdd(body, parseStatement);

    if tok.kind = tkSemi then
    begin
      semiLoc := consumeToken;
      { if c then s; else t -- the ';' already ended the if statement }
      if tok.kind = tkKwElse then
      begin
        diagError(semiLoc, ''';'' before ''else'' ends the ''if'' statement; ' +
                           'remove the '';''');
        consumeToken; { recover by treating the else branch as a statement }
      end;
      continue;
    end;
    if tok.kind = terminator then
      exit;
    if tok.kind = tkEof then
    begin
      diagError(tok.loc, 'expected ' + describe(terminator));
      diagNote(openLoc, 'to match this ''' + openWord + '''');
      exit;
    end;
    if startsStatement then
    begin
      { most likely a forgotten ';': report it and carry on }
      diagError(prevTokEnd, 'expected '';'' between statements');
      continue;
    end;
    diagError(tok.loc, 'expected '';'' or ' + describe(terminator) +
                       ' after statement');
    skipUntil([tkSemi, terminator]);
    if tryConsume(tkSemi) then
      continue;
    if tok.kind <> terminator then
    begin
      diagNote(openLoc, 'to match this ''' + openWord + '''');
      exit;
    end;
  end;
end;

function parseAssignmentOrCall: PNode; forward;
function parseIfStmt: PNode; forward;
function parseWhileStmt: PNode; forward;
function parseRepeatStmt: PNode; forward;
function parseForStmt: PNode; forward;
function parseCaseStmt: PNode; forward;
function parseWithStmt: PNode; forward;

function parseStatementImpl: PNode;
var
  gotoLoc, start, l: TLoc;
  lbl: TIdent;
  n, body: PNode;
begin
  case tok.kind of
    tkIdentifier: exit(parseAssignmentOrCall);
    tkKwBegin: exit(parseCompoundStmt);
    tkKwIf: exit(parseIfStmt);
    tkKwWhile: exit(parseWhileStmt);
    tkKwRepeat: exit(parseRepeatStmt);
    tkKwFor: exit(parseForStmt);
    tkKwCase: exit(parseCaseStmt);
    tkKwWith: exit(parseWithStmt);
    { the empty statement: nothing before a separator or terminator }
    tkSemi, tkKwEnd, tkKwElse, tkKwUntil, tkEof:
      exit(emptyStmt(tok.loc));
    tkKwGoto:
      begin
        gotoLoc := consumeToken;
        if tok.kind <> tkIntegerLiteral then
          diagError(tok.loc, 'expected label number after ''goto''')
        else
        begin
          lbl := makeLabel(tok);
          consumeToken;
          n := newNode(nkGoto, gotoLoc, prevTokEnd);
          n^.lbl := lbl;
          exit(n);
        end;
      end;
    tkIntegerLiteral:
      begin
        lbl := makeLabel(tok);
        start := consumeToken;
        if expect(tkColon, 'after statement label') then
        begin
          body := nestedStatement;
          n := newNode(nkLabeled, start, prevTokEnd);
          n^.lbl := lbl;
          n^.lblBody := body;
          exit(n);
        end;
      end;
  else
    diagError(tok.loc, 'expected statement');
  end;
  l := tok.loc;
  skipUntil([tkSemi, tkKwEnd]);
  parseStatementImpl := emptyStmt(l);
end;

function parseStatement: PNode;
begin
  if not enterNesting then
    exit(emptyStmt(tok.loc));
  parseStatement := parseStatementImpl;
  nesting := nesting - 1;
end;

function nameRef(const id: TIdent): PNode;
var
  n: PNode;
  r: TRange;
begin
  r := identRange(id);
  n := newNode(nkNameRef, r.b, r.e);
  n^.ref := id;
  nameRef := n;
end;

function indirectCall(b, e: TLoc; callee: PNode; const args: TPtrList): PNode;
var
  n: PNode;
begin
  n := newNode(nkIndirectCall, b, e);
  n^.calleeExpr := callee;
  n^.args := args;
  n^.callee := noIdent;
  indirectCall := n;
end;

// assignment = designator ":=" expression .
// procCall   = ident [ "(" actualParam { "," actualParam } ")" ] .
function parseAssignmentOrCall: PNode;
var
  name: TIdent;
  start, assignLoc: TLoc;
  target, value, call, n: PNode;
  hasSelectors, hasParens: boolean;
  args, none: TPtrList;
begin
  name := makeIdentifier(tok);
  start := consumeToken;

  target := nameRef(name);
  hasSelectors := tok.kind in [tkLSquare, tkPeriod, tkCaret];
  if hasSelectors then
  begin
    target := parseSelectors(target);
    if target = nil then
    begin
      skipUntil([tkSemi, tkKwEnd]);
      exit(emptyStmt(start));
    end;
  end;

  { common slip: '=' for ':='; report it, then parse an assignment anyway }
  if tok.kind = tkEqual then
    diagErrorR(tok.loc, 'expected '':='' in assignment, found ''=''',
               tokenRange(tok));

  if tok.kind in [tkColonEqual, tkEqual] then
  begin
    assignLoc := consumeToken;
    value := parseExpression;
    if value = nil then
    begin
      skipUntil([tkSemi, tkKwEnd]);
      exit(emptyStmt(start));
    end;
    n := newNode(nkAssign, start, prevTokEnd);
    n^.target := target;
    n^.value := value;
    n^.assignLoc := assignLoc;
    exit(n);
  end;

  { Turbo/FPC: a procedure call through a procedural value: r.p(x); a[i]; }
  if hasSelectors and (mode = lmTP) and
     (tok.kind in [tkSemi, tkKwEnd, tkKwElse, tkKwUntil, tkEof]) then
  begin
    call := target;
    if call^.kind <> nkIndirectCall then { r.p without arguments }
    begin
      listInit(none);
      call := indirectCall(target^.b, target^.e, target, none);
    end;
    n := newNode(nkIndirectCallStmt, start, prevTokEnd);
    n^.calleeExpr := call;
    n^.callee := noIdent;
    listInit(n^.args);
    exit(n);
  end;

  if hasSelectors then
  begin
    diagError(prevTokEnd, 'expected '':='' after variable');
    skipUntil([tkSemi, tkKwEnd]);
    exit(emptyStmt(start));
  end;

  listInit(args);
  hasParens := tok.kind = tkLParen;
  if hasParens then
    parseActualParams(args, true);
  n := newNode(nkProcCall, start, prevTokEnd);
  n^.callee := name;
  n^.args := args;
  n^.hasParens := hasParens;
  parseAssignmentOrCall := n;
end;

function parseActualParams(var args: TPtrList; allowFormat: boolean): boolean;
var
  lparen: TLoc;
  e, width, precision, f: PNode;
  ok: boolean;
begin
  lparen := consumeToken; { '(' }
  if tok.kind = tkRParen then
  begin
    diagErrorR(tok.loc, 'expected expression', makeRange(lparen, tokenEnd(tok)));
    diagNote(lparen, 'an empty argument list ''()'' is not allowed; ' +
                     'omit the parentheses');
    consumeToken;
    exit(false);
  end;
  repeat
    e := parseExpression;
    if e = nil then
    begin
      skipUntil([tkComma, tkRParen, tkSemi, tkKwEnd]);
      continue;
    end;
    if allowFormat and (tok.kind = tkColon) then
    begin
      consumeToken;
      width := parseExpression;
      precision := nil;
      ok := width <> nil;
      if ok and tryConsume(tkColon) then
      begin
        precision := parseExpression;
        ok := precision <> nil;
      end;
      if not ok then
      begin
        skipUntil([tkComma, tkRParen, tkSemi, tkKwEnd]);
        continue;
      end;
      f := newNode(nkFormatted, e^.b, prevTokEnd);
      f^.fmtValue := e;
      f^.fmtWidth := width;
      f^.fmtPrecision := precision;
      e := f;
    end;
    listAdd(args, e);
  until not tryConsume(tkComma);

  if not expect(tkRParen, 'after arguments') then
  begin
    diagNote(lparen, 'to match this ''(''');
    exit(false);
  end;
  parseActualParams := true;
end;

{ if = "if" expression "then" statement [ "else" statement ] . The dangling
  else binds to the nearest if: the inner parseIfStmt sees it first. }
function parseIfStmt: PNode;
var
  ifLoc: TLoc;
  cond, thenS, elseS, n: PNode;
begin
  ifLoc := consumeToken;
  cond := parseExpression;
  if cond = nil then
  begin
    skipUntil([tkKwThen, tkSemi, tkKwEnd]);
    if tok.kind <> tkKwThen then
      exit(emptyStmt(ifLoc));
  end;
  if not expect(tkKwThen, 'after ''if'' condition') and not startsStatement then
  begin
    skipUntil([tkSemi, tkKwEnd]);
    exit(emptyStmt(ifLoc));
  end;
  thenS := parseStatement;
  elseS := nil;
  if tryConsume(tkKwElse) then
    elseS := parseStatement;
  if cond = nil then
    exit(emptyStmt(ifLoc));
  n := newNode(nkIf, ifLoc, prevTokEnd);
  n^.cond := cond;
  n^.thenStmt := thenS;
  n^.elseStmt := elseS;
  parseIfStmt := n;
end;

{ while = "while" expression "do" statement . }
function parseWhileStmt: PNode;
var
  whileLoc: TLoc;
  cond, body, n: PNode;
begin
  whileLoc := consumeToken;
  cond := parseExpression;
  if cond = nil then
    skipUntil([tkKwDo, tkSemi, tkKwEnd]);
  if not expect(tkKwDo, 'after ''while'' condition') and not startsStatement then
  begin
    skipUntil([tkSemi, tkKwEnd]);
    exit(emptyStmt(whileLoc));
  end;
  body := parseStatement;
  if cond = nil then
    exit(emptyStmt(whileLoc));
  n := newNode(nkWhile, whileLoc, prevTokEnd);
  n^.cond := cond;
  n^.thenStmt := body;
  n^.elseStmt := nil;
  parseWhileStmt := n;
end;

{ repeat = "repeat" stmtSeq "until" expression . }
function parseRepeatStmt: PNode;
var
  repeatLoc: TLoc;
  body: TPtrList;
  cond, n: PNode;
begin
  repeatLoc := consumeToken;
  listInit(body);
  parseStatementSequence(body, tkKwUntil, repeatLoc, 'repeat');
  if not tryConsume(tkKwUntil) then
    exit(emptyStmt(repeatLoc));
  cond := parseExpression;
  if cond = nil then
    exit(emptyStmt(repeatLoc));
  n := newNode(nkRepeat, repeatLoc, prevTokEnd);
  n^.stmts := body;
  n^.untilCond := cond;
  parseRepeatStmt := n;
end;

{ for = "for" ident ":=" expression ("to"|"downto") expression "do" statement . }
function parseForStmt: PNode;
var
  forLoc: TLoc;
  v, init, final, body, n: PNode;
  down: boolean;

  function fail: PNode;
  begin
    skipUntil([tkSemi, tkKwEnd]);
    fail := emptyStmt(forLoc);
  end;

begin
  forLoc := consumeToken;
  if tok.kind <> tkIdentifier then
  begin
    diagError(tok.loc, 'expected control variable after ''for''');
    exit(fail);
  end;
  v := nameRef(makeIdentifier(tok));
  consumeToken;

  if tok.kind = tkEqual then
  begin
    diagErrorR(tok.loc, 'expected '':='' in ''for'' statement, found ''=''',
               tokenRange(tok));
    consumeToken;
  end
  else if not expect(tkColonEqual, 'after ''for'' control variable') then
    exit(fail);

  init := parseExpression;
  if init = nil then
    exit(fail);

  down := tok.kind = tkKwDownto;
  if not (tok.kind in [tkKwTo, tkKwDownto]) then
  begin
    diagError(prevTokEnd, 'expected ''to'' or ''downto'' in ''for'' statement');
    exit(fail);
  end;
  consumeToken;

  final := parseExpression;
  if final = nil then
    exit(fail);
  if not expect(tkKwDo, 'in ''for'' statement') and not startsStatement then
    exit(fail);

  body := parseStatement;
  n := newNode(nkFor, forLoc, prevTokEnd);
  n^.forVar := v;
  n^.forInit := init;
  n^.forFinal := final;
  n^.isDownto := down;
  n^.forBody := body;
  parseForStmt := n;
end;

function isCaseElse: boolean;
begin
  isCaseElse := (tok.kind = tkKwElse) or isIdentifierNamed('otherwise');
end;

// case = "case" expression "of" caseArm { ";" caseArm } [ ";" ]
// [ ("else"|"otherwise") stmtSeq ] "end" .
function parseCaseStmt: PNode;
var
  caseLoc, elseLoc, endLoc: TLoc;
  selector, body, n: PNode;
  arms, elseBody, labels: TPtrList;
  ok: boolean;
  l: PCaseLabel;
  arm: PCaseArm;
begin
  caseLoc := consumeToken;
  selector := parseExpression;
  if selector = nil then
    skipUntil([tkKwOf, tkKwEnd]);
  if not expect(tkKwOf, 'after ''case'' selector') then
  begin
    skipUntil([tkKwEnd]);
    tryConsume(tkKwEnd);
    exit(emptyStmt(caseLoc));
  end;

  listInit(arms);
  listInit(elseBody);
  elseLoc := noLoc;
  ok := selector <> nil;

  while not diagShouldStop and (tok.kind <> tkKwEnd) and (tok.kind <> tkEof) do
  begin
    if isCaseElse then
    begin
      if mode = lmISO then
        diagErrorR(tok.loc, '''' + rawText(tok) + ''' in a ''case'' statement ' +
                   'is a Turbo/Free Pascal extension; use --mode=tp',
                   tokenRange(tok));
      elseLoc := consumeToken;
      tryConsume(tkColon); { some dialects write otherwise: }
      parseStatementSequence(elseBody, tkKwEnd, caseLoc, 'case');
      break;
    end;

    listInit(labels);
    repeat
      l := parseCaseLabel(true);
      if l <> nil then
        listAdd(labels, l)
      else
        ok := false;
    until not tryConsume(tkComma);

    if not expect(tkColon, 'after case label') then
    begin
      ok := false;
      skipUntil([tkSemi, tkKwEnd]);
      if tryConsume(tkSemi) then
        continue;
      break;
    end;
    body := parseStatement;
    arm := PCaseArm(arenaAlloc(sizeof(TCaseArm)));
    arm^.labels := labels;
    arm^.body := body;
    listAdd(arms, arm);

    if tryConsume(tkSemi) then
      continue;
    if (tok.kind = tkKwEnd) or isCaseElse then
      continue;
    diagError(prevTokEnd, 'expected '';'' or ''end'' after case arm');
    ok := false;
    skipUntil([tkSemi, tkKwEnd]);
    tryConsume(tkSemi);
  end;

  if (arms.count = 0) and ok then
  begin
    diagError(tok.loc, 'a ''case'' statement needs at least one case label');
    ok := false;
  end;
  endLoc := tokenEnd(tok);
  if not tryConsume(tkKwEnd) then
  begin
    diagError(tok.loc, 'expected ''end'' at end of ''case'' statement');
    diagNote(caseLoc, 'to match this ''case''');
    exit(emptyStmt(caseLoc));
  end;
  if not ok then
    exit(emptyStmt(caseLoc));
  n := newNode(nkCase, caseLoc, endLoc);
  n^.selector := selector;
  n^.arms := arms;
  n^.elseLoc := elseLoc;
  n^.elseBody := elseBody;
  parseCaseStmt := n;
end;

function parseConstant: PNode; forward;

{ caseLabel = constant [ ".." constant ] . }
function parseCaseLabel(allowRange: boolean): PCaseLabel;
var
  lo, hi: PNode;
  l: PCaseLabel;
begin
  parseCaseLabel := nil;
  lo := parseConstant;
  if lo = nil then
    exit;
  hi := nil;
  if tok.kind = tkDotdot then
  begin
    if not allowRange then
      diagErrorR(tok.loc, 'ranges are not allowed in variant labels',
                 tokenRange(tok))
    else if mode = lmISO then
      diagErrorR(tok.loc, 'case label ranges are a Turbo/Free Pascal ' +
                 'extension; use --mode=tp', tokenRange(tok));
    consumeToken;
    hi := parseConstant;
    if hi = nil then
      exit;
  end;
  l := PCaseLabel(arenaAlloc(sizeof(TCaseLabel)));
  l^.lo := lo;
  l^.hi := hi;
  parseCaseLabel := l;
end;

function literalNode(const t: TToken): PNode;
var
  n: PNode;
begin
  case t.kind of
    tkIntegerLiteral:
      begin
        n := newNode(nkIntegerLit, t.loc, tokenEnd(t));
        n^.intValue := t.intValue;
      end;
    tkRealLiteral:
      begin
        n := newNode(nkRealLit, t.loc, tokenEnd(t));
        n^.realValue := t.realValue;
      end;
  else
    begin
      n := newNode(nkStringLit, t.loc, tokenEnd(t));
      n^.strValue := t.strValue;
    end;
  end;
  literalNode := n;
end;

function unaryNode(op: TUnaryOp; opLoc: TLoc; operand: PNode): PNode;
var
  n: PNode;
begin
  n := newNode(nkUnary, opLoc, operand^.e);
  n^.uop := op;
  n^.operand := operand;
  unaryNode := n;
end;

{ constant = [ "+" | "-" ] ( unsignedNumber | ident ) | string . }
function parseConstant: PNode;
var
  op: TUnaryOp;
  opLoc: TLoc;
  operand, e: PNode;
begin
  parseConstant := nil;
  if tok.kind in [tkPlus, tkMinus] then
  begin
    if tok.kind = tkPlus then
      op := uoPlus
    else
      op := uoMinus;
    opLoc := consumeToken;
    if tok.kind in [tkIntegerLiteral, tkRealLiteral] then
      operand := literalNode(tok)
    else if tok.kind = tkIdentifier then
      operand := nameRef(makeIdentifier(tok))
    else
    begin
      diagError(tok.loc, 'expected constant after sign');
      exit;
    end;
    consumeToken;
    exit(unaryNode(op, opLoc, operand));
  end;
  case tok.kind of
    tkIntegerLiteral, tkRealLiteral, tkStringLiteral:
      e := literalNode(tok);
    tkIdentifier:
      e := nameRef(makeIdentifier(tok));
  else
    begin
      diagError(tok.loc, 'expected constant');
      exit;
    end;
  end;
  consumeToken;
  parseConstant := e;
end;

// with = "with" designator { "," designator } "do" statement .
function parseDesignator: PNode; forward;

function parseWithStmt: PNode;
var
  withLoc: TLoc;
  records: TPtrList;
  r, body, n: PNode;
  w: PWithContext;
begin
  withLoc := consumeToken;
  listInit(records);
  repeat
    r := parseDesignator;
    if r = nil then
    begin
      skipUntil([tkSemi, tkKwEnd]);
      exit(emptyStmt(withLoc));
    end;
    w := PWithContext(arenaAlloc(sizeof(TWithContext)));
    w^.rec := r;
    listAdd(records, w);
  until not tryConsume(tkComma);
  if not expect(tkKwDo, 'after ''with'' records') and not startsStatement then
  begin
    skipUntil([tkSemi, tkKwEnd]);
    exit(emptyStmt(withLoc));
  end;
  body := parseStatement;
  n := newNode(nkWith, withLoc, prevTokEnd);
  n^.withRecords := records;
  n^.withBody := body;
  parseWithStmt := n;
end;

{ --- Expressions --- }

function peekRelOp(var op: TBinaryOp): boolean;
begin
  peekRelOp := true;
  case tok.kind of
    tkEqual: op := boEq;
    tkNotEqual: op := boNe;
    tkLess: op := boLt;
    tkLessEqual: op := boLe;
    tkGreater: op := boGt;
    tkGreaterEqual: op := boGe;
    tkKwIn: op := boIn;
  else
    peekRelOp := false;
  end;
end;

function peekAddOp(var op: TBinaryOp): boolean;
begin
  peekAddOp := true;
  case tok.kind of
    tkPlus: op := boAdd;
    tkMinus: op := boSub;
    tkKwOr: op := boOr;
    tkKwXor: op := boXor;
  else
    peekAddOp := false;
  end;
end;

function peekMulOp(var op: TBinaryOp): boolean;
begin
  peekMulOp := true;
  case tok.kind of
    tkStar: op := boMul;
    tkSlash: op := boRealDiv;
    tkKwDiv: op := boIntDiv;
    tkKwMod: op := boMod;
    tkKwAnd: op := boAnd;
    tkKwShl: op := boShl;
    tkKwShr: op := boShr;
  else
    peekMulOp := false;
  end;
end;

function makeBinary(op: TBinaryOp; opLoc: TLoc; l, r: PNode): PNode;
var
  n: PNode;
begin
  n := newNode(nkBinary, l^.b, r^.e);
  n^.bop := op;
  n^.opLoc := opLoc;
  n^.lhs := l;
  n^.rhs := r;
  makeBinary := n;
end;

{ expression = simpleExpr [ relOp simpleExpr ] . }
function parseExpressionImpl: PNode;
var
  lhs, rhs, res: PNode;
  op, op2: TBinaryOp;
  opLoc: TLoc;
begin
  parseExpressionImpl := nil;
  lhs := parseSimpleExpression;
  if lhs = nil then
    exit;
  if not peekRelOp(op) then
    exit(lhs);
  opLoc := consumeToken;
  rhs := parseSimpleExpression;
  if rhs = nil then
    exit;
  res := makeBinary(op, opLoc, lhs, rhs);

  { a < b and c < d parses as a < (b and c) followed by < d }
  if peekRelOp(op2) then
  begin
    diagErrorR(tok.loc, 'comparison operators cannot be chained', tokenRange(tok));
    if (rhs^.kind = nkBinary) and (rhs^.bop in [boAnd, boOr]) then
      diagNote(rhs^.opLoc, '''' + binaryOpSpelling(rhs^.bop) +
               ''' binds more tightly than comparison operators; use ' +
               'parentheses, e.g. ''(a < b) ' + binaryOpSpelling(rhs^.bop) +
               ' (c < d)''');
    { recover: consume the rest of the chain }
    while peekRelOp(op2) do
    begin
      consumeToken;
      if parseSimpleExpression = nil then
        exit;
    end;
  end;
  parseExpressionImpl := res;
end;

function parseExpression: PNode;
begin
  if not enterNesting then
    exit(nil);
  parseExpression := parseExpressionImpl;
  nesting := nesting - 1;
end;

function parseTerm: PNode; forward;

// simpleExpr = [ "+" | "-" ] term { addOp term } .
function parseSimpleExpression: PNode;
var
  lhs, rhs, operand: PNode;
  uop: TUnaryOp;
  op: TBinaryOp;
  opLoc: TLoc;
begin
  parseSimpleExpression := nil;
  if tok.kind in [tkPlus, tkMinus] then
  begin
    { ISO: the sign applies to the whole first term: -a*b = -(a*b) }
    if tok.kind = tkPlus then
      uop := uoPlus
    else
      uop := uoMinus;
    opLoc := consumeToken;
    operand := parseTerm;
    if operand = nil then
      exit;
    lhs := unaryNode(uop, opLoc, operand);
  end
  else
    lhs := parseTerm;
  if lhs = nil then
    exit;
  while peekAddOp(op) do
  begin
    opLoc := consumeToken;
    rhs := parseTerm;
    if rhs = nil then
      exit;
    lhs := makeBinary(op, opLoc, lhs, rhs);
  end;
  parseSimpleExpression := lhs;
end;

function parseFactor: PNode; forward;

// term = factor { mulOp factor } .
function parseTerm: PNode;
var
  lhs, rhs: PNode;
  op: TBinaryOp;
  opLoc: TLoc;
begin
  parseTerm := nil;
  lhs := parseFactor;
  if lhs = nil then
    exit;
  while peekMulOp(op) do
  begin
    opLoc := consumeToken;
    rhs := parseFactor;
    if rhs = nil then
      exit;
    lhs := makeBinary(op, opLoc, lhs, rhs);
  end;
  parseTerm := lhs;
end;

// designator = ident { "[" expression { "," expression } "]" | "." ident } .
function parseDesignator: PNode;
var
  name: TIdent;
begin
  if not expectIdentifier(name, 'variable name') then
    exit(nil);
  parseDesignator := parseSelectors(nameRef(name));
end;

function parseSelectors(base: PNode): PNode;
var
  lbrack: TLoc;
  idx, n: PNode;
  field: TIdent;
  args: TPtrList;
begin
  parseSelectors := nil;
  while true do
  begin
    if tok.kind = tkLSquare then
    begin
      lbrack := consumeToken;
      repeat
        idx := parseExpression;
        if idx = nil then
          exit;
        n := newNode(nkIndex, base^.b, idx^.e);
        n^.base := base;
        n^.index := idx;
        base := n;
      until not tryConsume(tkComma);
      if not expect(tkRSquare, 'after array index') then
      begin
        diagNote(lbrack, 'to match this ''[''');
        exit;
      end;
      continue;
    end;
    if tok.kind = tkPeriod then
    begin
      consumeToken;
      if not expectIdentifier(field, 'field name after ''.''') then
        exit;
      n := newNode(nkField, base^.b, prevTokEnd);
      n^.base := base;
      n^.fieldName := field;
      base := n;
      continue;
    end;
    if tok.kind = tkCaret then
    begin
      consumeToken;
      n := newNode(nkDeref, base^.b, prevTokEnd);
      n^.inner := base;
      base := n;
      continue;
    end;
    { Turbo/FPC: a call through a procedural value, passes[i].run(v).
      (A plain name followed by '(' is a call.) }
    if (tok.kind = tkLParen) and (mode = lmTP) and (base^.kind <> nkNameRef) then
    begin
      listInit(args);
      if not parseActualParams(args, false) and (args.count = 0) then
        exit;
      base := indirectCall(base^.b, prevTokEnd, base, args);
      continue;
    end;
    exit(base);
  end;
end;

{ factor = unsignedConst | designator | ident "(" args ")"
         | "(" expression ")" | "not" factor . }
function parseFactor: PNode;
var
  name: TIdent;
  args, elems: TPtrList;
  call, inner, operand, n, lo, hi: PNode;
  lparen, opLoc, atLoc, lbrack: TLoc;
  uop: TUnaryOp;
  el: PSetElement;
begin
  parseFactor := nil;
  case tok.kind of
    tkIntegerLiteral, tkRealLiteral, tkStringLiteral:
      begin
        n := literalNode(tok);
        consumeToken;
        exit(n);
      end;
    tkIdentifier:
      begin
        name := makeIdentifier(tok);
        consumeToken;
        if tok.kind = tkLParen then
        begin
          listInit(args);
          if not parseActualParams(args, false) and (args.count = 0) then
            exit;
          call := newNode(nkCall, name.loc, prevTokEnd);
          call^.callee := name;
          call^.args := args;
          { Turbo/FPC: a call (or typecast) can be dereferenced and selected
            from: PNode(p)^.next, f(x)[1] }
          if mode = lmTP then
            exit(parseSelectors(call));
          exit(call);
        end;
        exit(parseSelectors(nameRef(name)));
      end;
    tkLParen:
      begin
        lparen := consumeToken;
        inner := parseExpression;
        if inner = nil then
          exit;
        if not expect(tkRParen, 'after expression') then
        begin
          diagNote(lparen, 'to match this ''(''');
          exit;
        end;
        n := newNode(nkParen, lparen, prevTokEnd);
        n^.inner := inner;
        exit(n);
      end;
    tkKwNot:
      begin
        opLoc := consumeToken;
        operand := nestedFactor;
        if operand = nil then
          exit;
        exit(unaryNode(uoNot, opLoc, operand));
      end;
    tkPlus, tkMinus:
      begin
        { a sign inside a term (a * -b): ISO only allows a sign at the
          start of a simple expression; Turbo/FPC anywhere }
        if mode = lmISO then
          diagErrorR(tok.loc, 'a sign is not allowed here in ISO Pascal; use ' +
                     'parentheses', tokenRange(tok));
        if tok.kind = tkPlus then
          uop := uoPlus
        else
          uop := uoMinus;
        opLoc := consumeToken;
        operand := nestedFactor;
        if operand = nil then
          exit;
        exit(unaryNode(uop, opLoc, operand));
      end;
    tkAt:
      begin { @x (Turbo/FPC) }
        atLoc := consumeToken;
        operand := parseDesignator;
        if operand = nil then
          exit;
        n := newNode(nkAddrOf, atLoc, prevTokEnd);
        n^.inner := operand;
        exit(n);
      end;
    tkKwNil:
      begin
        n := newNode(nkNil, tok.loc, tokenEnd(tok));
        consumeToken;
        exit(n);
      end;
    tkLSquare:
      begin
        // set constructor: "[" [ elem { "," elem } ] "]"; elem = expr [".." expr]
        lbrack := consumeToken;
        listInit(elems);
        if tok.kind <> tkRSquare then
          repeat
            lo := parseExpression;
            if lo = nil then
              exit;
            hi := nil;
            if tryConsume(tkDotdot) then
            begin
              hi := parseExpression;
              if hi = nil then
                exit;
            end;
            el := PSetElement(arenaAlloc(sizeof(TSetElement)));
            el^.lo := lo;
            el^.hi := hi;
            listAdd(elems, el);
          until not tryConsume(tkComma);
        if not expect(tkRSquare, 'after set elements') then
        begin
          diagNote(lbrack, 'to match this ''[''');
          exit;
        end;
        n := newNode(nkSetCtor, lbrack, prevTokEnd);
        n^.setElems := elems;
        exit(n);
      end;
  else
    diagError(tok.loc, 'expected expression');
  end;
end;

function nestedTypedConstValue: PNode;
begin
  nestedTypedConstValue := parseTypedConstValue;
end;

function nestedType: PTypeSpec;
begin
  nestedType := parseType;
end;

function nestedStatement: PNode;
begin
  nestedStatement := parseStatement;
end;

function nestedFactor: PNode;
begin
  nestedFactor := parseFactor;
end;

end.
