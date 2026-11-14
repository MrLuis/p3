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
{ A specialization of generic routine g: its declaration parsed again from
  its tokens (an ordinary routine, named as g). }
function reparseGeneric(g: PDecl): PDecl;

type
  { declared(NAME) in $IF: the used units' names (their own, and those of
    their interfaces) are added to names, given the units of the uses
    clauses read since the last call and the unit being parsed (if any) }
  TUsedUnitNames = procedure(const usesList: TPtrList; selfName: PName;
                             var names: TPtrList);

var
  usedUnitNames: TUsedUnitNames; { set by the driver }

implementation

type
  TTokenSet = set of TTokenKind;

const
  maxNesting = 256;

type
  PPtrList = ^TPtrList;
  PToken = ^TToken;
  { The tokens of a generic routine (objfpc), parsed again for each
    specialization, and the lexer switches in effect where it was declared. }
  PGenericTokens = ^TGenericTokens;
  TGenericTokens = record
    toks: TPtrList; { of PToken }
    longStrings, advancedRecords, corbaInterfaces, inlineOn: boolean;
    typeHelpers, scopedEnums: boolean;
    packRecords: longint;
    resultSwitch, outSwitch, defaultParamsSwitch: boolean;
  end;

var
  lx: TLexer;
  mode: TLangMode;
  { expressions, statements and types being parsed; deeper than maxNesting
    is a fatal error instead of a stack overflow }
  nesting: longint;
  tok, peekTok: TToken;
  hasPeek: boolean;
  prevTokEnd: TLoc;
  { parseFieldList skipped to the record's end after an error }
  recordBodySkipped: boolean;
  { generic routines: the tokens consumed are recorded (recording, of
    PToken); or tokens are replayed from a generic's instead of being
    lexed }
  recording: PPtrList;
  replay: pointer; { PGenericTokens }
  { the statement starting with a designator being parsed: its tokens so
    far (of PToken; for += -= *= /=, which read the designator again), or
    nil }
  stmtTokens: PPtrList;
  { in parseRoutineDecl: a class constructor or destructor's
    implementation }
  parsingClassInit: boolean;
  replayPos: longint;
  { parsing a generic routine: type parameters may follow its name
    (genericTypeParams receives them) }
  parsingGeneric: boolean;
  genericTypeParams: TPtrList;
  { declared(NAME): the units of the uses clauses read (of PIdent), not yet
    given to usedUnitNames; the unit being parsed }
  pendingUses: TPtrList;
  unitName: PName;

{ --- Token helpers --- }

procedure nextToken(var t: TToken);
var
  g: PGenericTokens;
begin
  if replay = nil then
  begin
    lexNext(lx, t);
    exit;
  end;
  g := PGenericTokens(replay);
  if replayPos < g^.toks.count then
  begin
    t := PToken(listGet(g^.toks, replayPos))^;
    replayPos := replayPos + 1;
  end
  else
  begin
    fillchar(t, sizeof(TToken), 0);
    t.kind := tkEOF;
  end;
end;

procedure declareName(const name: TIdent);
begin
  if identValid(name) then
    lexDeclareName(lx, name.name, false);
end;

{ before declared(NAME): the names of the units used so far }
procedure loadPendingUses(var l: TLexer);
var
  usesList, names: TPtrList;
  i: longint;
begin
  if (pendingUses.count = 0) or not assigned(usedUnitNames) then
    exit;
  usesList := pendingUses;
  listInit(pendingUses);
  listInit(names);
  usedUnitNames(usesList, unitName, names);
  for i := 0 to names.count - 1 do
    lexDeclareName(l, PName(listGet(names, i)), true);
end;

procedure startParser(fid: longint; m: TLangMode);
begin
  recordBodySkipped := false;
  recording := nil;
  replay := nil;
  stmtTokens := nil;
  parsingClassInit := false;
  parsingGeneric := false;
  listInit(pendingUses);
  unitName := nil;
  beforeDeclared := @loadPendingUses;
  mode := m;
  lexInit(lx, fid, m);
  hasPeek := false;
  prevTokEnd := noLoc;
  nesting := 0;
  lexNext(lx, tok);
end;

function consumeToken: TLoc;
var
  p: PToken;
begin
  if recording <> nil then
  begin
    p := PToken(arenaAlloc(sizeof(TToken)));
    p^ := tok;
    listAdd(recording^, p);
  end;
  if stmtTokens <> nil then
  begin
    p := PToken(arenaAlloc(sizeof(TToken)));
    p^ := tok;
    listAdd(stmtTokens^, p);
  end;
  consumeToken := tok.loc;
  prevTokEnd := tokenEnd(tok);
  if hasPeek then
  begin
    tok := peekTok;
    hasPeek := false;
  end
  else
    nextToken(tok);
end;

procedure peekToken;
begin
  if not hasPeek then
  begin
    nextToken(peekTok);
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
                                  tkKwGoto, tkIntegerLiteral, tkKwInherited,
                                  tkKwTry, tkKwRaise];
end;

function startsDeclarationPart: boolean;
begin
  if (mode = lmObjFPC) and (tok.kind in [tkKwConstructor, tkKwDestructor, tkKwClass]) then
    exit(true); { method implementations (class procedure TFoo.P) }
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

{ objfpc: as in Free Pascal (without -Sg), at label and at goto. The labels
  and gotos are parsed as usual, so no other errors follow. }
procedure diagNoGoto;
begin
  diagErrorR(tok.loc, 'labels and ''goto'' are not supported with --mode=objfpc',
             tokenRange(tok));
end;

function isIdentifierNamed(const s: string): boolean;
begin
  isIdentifierNamed := (tok.kind = tkIdentifier) and nameEquals(tok.name, s);
end;

{ objfpc: generic function F<T, U>(...): ...; (a template) }
function startsGeneric: boolean;
begin
  startsGeneric := (mode = lmObjFPC) and isIdentifierNamed('generic') and
                   (peekKind in [tkKwFunction, tkKwProcedure]);
end;

function startsSpecialize: boolean;
begin
  startsSpecialize := (mode = lmObjFPC) and isIdentifierNamed('specialize') and
                      (peekKind = tkIdentifier);
end;

{ Free Pascal's hint directives: the flag of the word tok is, or 0. }
{ The calling convention word (cdecl, stdcall) at tok, or ccDefault. }
function callConvOfTok(const t: TToken): longint;
var
  w: string;
begin
  callConvOfTok := ccDefault;
  if (mode = lmISO) or (t.kind <> tkIdentifier) then
    exit;
  w := nameStr(t.name);
  if w = 'cdecl' then
    callConvOfTok := ccCDecl
  else if w = 'stdcall' then
    callConvOfTok := ccStdCall;
end;

{ Consumes the calling convention word at tok into cc (warning if one was
  already given). }
procedure takeCallConv(var cc: longint);
var
  nw: longint;
  loc: TLoc;
  r: TRange;
begin
  nw := callConvOfTok(tok);
  loc := tok.loc;
  r := makeRange(loc, loc + length(callConvName(nw)));
  consumeToken;
  if cc = nw then
    diagWarningR(loc, 'calling convention ''' + callConvName(nw) +
                 ''' is given twice', r)
  else if cc <> ccDefault then
    diagWarningR(loc, 'calling convention ''' + callConvName(nw) +
                 ''' replaces ''' + callConvName(cc) + '''', r);
  cc := nw;
end;

function hintFlagOf: longint;
var
  w: string;
begin
  hintFlagOf := 0;
  if (mode = lmISO) or (tok.kind <> tkIdentifier) then
    exit;
  w := nameStr(tok.name);
  if w = 'deprecated' then
    hintFlagOf := hintDeprecated
  else if w = 'experimental' then
    hintFlagOf := hintExperimental
  else if w = 'platform' then
    hintFlagOf := hintPlatform
  else if w = 'library' then
    hintFlagOf := hintLibrary
  else if w = 'unimplemented' then
    hintFlagOf := hintUnimplemented;
end;

{ hints = ( "deprecated" [ string ] | "experimental" | "platform"
            | "library" | "unimplemented" ) ... (Turbo/FPC), after a
  declaration's type or value }
procedure parseHints(var flags: longint; var msg: PName);
var
  f: longint;
begin
  flags := 0;
  msg := nil;
  f := hintFlagOf;
  while f <> 0 do
  begin
    consumeToken;
    flags := flags or f;
    if (f = hintDeprecated) and (tok.kind = tkStringLiteral) then
    begin
      msg := tok.strValue;
      consumeToken;
    end;
    f := hintFlagOf;
  end;
end;

{ A hint directive after a routine's or property's ";":  deprecated 'm'; }
function parseSeparatedHint(var flags: longint; var msg: PName): boolean;
var
  f: longint;
  w: string;
begin
  f := hintFlagOf;
  if f = 0 then
    exit(false);
  w := nameStr(tok.name);
  consumeToken;
  flags := flags or f;
  if (f = hintDeprecated) and (tok.kind = tkStringLiteral) then
  begin
    msg := tok.strValue;
    consumeToken;
  end;
  expect(tkSemi, 'after ''' + w + '''');
  parseSeparatedHint := true;
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
function parseNamedType: PTypeSpec; forward;
function parseStatement: PNode; forward;
function parseCompoundStmt: PNode; forward;
procedure parseStatementSequence(var body: TPtrList; terminator: TTokenKind;
                                 openLoc: TLoc; const openWord: string); forward;
function parseExpression: PNode; forward;
function parseSimpleExpression: PNode; forward;
{ first: the expression's first factor, already parsed (no sign) }
function parseSimpleExpressionFrom(first: PNode): PNode; forward;
function nameRef(const id: TIdent): PNode; forward;
function parseSelectors(base: PNode): PNode; forward;
function parseFormalParams(var params: TPtrList): boolean; forward;
function parseFormalParamsClose(var params: TPtrList; close: TTokenKind): boolean; forward;
function parseCaseLabel(allowRange: boolean): PCaseLabel; forward;
function parseActualParams(var args: TPtrList; allowFormat: boolean): boolean; forward;

{ --- Program and units --- }

function parseUsesClause(var usesList: TPtrList): boolean;
var
  listed: boolean;
  i: longint;
begin
  consumeToken; { 'uses' }
  listed := parseIdentList(usesList, 'unit name');
  { declared(NAME) right after the clause sees the units' names }
  for i := 0 to usesList.count - 1 do
    listAdd(pendingUses, listGet(usesList, i));
  if not listed or not expect(tkSemi, 'after uses clause') then
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
  unitName := name.name;
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
var
  savedLx: TLexer;
  savedMode: TLangMode;
  savedTok, savedPeek: TToken;
  savedHasPeek, savedSkipped, savedGeneric: boolean;
  savedPrev: TLoc;
  savedNesting, savedPos: longint;
  savedRec, savedStmtToks: PPtrList;
  savedReplay: pointer;
  savedTypeParams, savedUses: TPtrList;
  savedUnit: PName;
begin
  { a used unit may be loaded while another file is parsed (declared(NAME)
    in $IF): the parser's state is kept }
  savedLx := lx;
  savedMode := mode;
  savedTok := tok;
  savedPeek := peekTok;
  savedHasPeek := hasPeek;
  savedSkipped := recordBodySkipped;
  savedGeneric := parsingGeneric;
  savedPrev := prevTokEnd;
  savedNesting := nesting;
  savedPos := replayPos;
  savedRec := recording;
  savedReplay := replay;
  savedStmtToks := stmtTokens;
  savedTypeParams := genericTypeParams;
  savedUses := pendingUses;
  savedUnit := unitName;
  startParser(fid, mode);
  parseUnit := parseUnitHere;
  lx := savedLx;
  mode := savedMode;
  tok := savedTok;
  peekTok := savedPeek;
  hasPeek := savedHasPeek;
  recordBodySkipped := savedSkipped;
  parsingGeneric := savedGeneric;
  prevTokEnd := savedPrev;
  nesting := savedNesting;
  replayPos := savedPos;
  recording := savedRec;
  replay := savedReplay;
  stmtTokens := savedStmtToks;
  genericTypeParams := savedTypeParams;
  pendingUses := savedUses;
  unitName := savedUnit;
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

function isVisibilityWord: boolean;
begin
  isVisibilityWord := isIdentifierNamed('private') or isIdentifierNamed('protected') or
                      isIdentifierNamed('public') or isIdentifierNamed('published') or
                      isIdentifierNamed('strict');
end;

{ inMembers: a section of a class or advanced record, which ends at a
  visibility word (private, public, ...). }
procedure parseConstPart(var decls: TPtrList; inMembers: boolean);
var
  hf: longint;
  hm: PName;
  name: TIdent;
  ts: PTypeSpec;
  init: PNode;
  v, c: PDecl;
begin
  consumeToken; { 'const' }
  if (tok.kind <> tkIdentifier) or (inMembers and isVisibilityWord) then
  begin
    diagError(tok.loc, 'expected constant name after ''const''');
    exit;
  end;
  while (tok.kind = tkIdentifier) and not (inMembers and isVisibilityWord) and
        not startsGeneric and not diagShouldStop do
  begin
    name := makeIdentifier(tok);
    if not inMembers then
      declareName(name);
    consumeToken;
    if (tok.kind = tkColon) and (mode <> lmISO) then
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
        parseHints(hf, hm);
        addHints(v, hf, hm);
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
    parseHints(hf, hm);
    if init <> nil then
    begin
      c := newDecl(dkConst, name, name.loc);
      c^.constInit := init;
      addHints(c, hf, hm);
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

function parseTypeHelper(start: TLoc; isRecord: boolean): PTypeSpec; forward;

procedure parseTypePart(var decls: TPtrList; inMembers: boolean);
var
  hf: longint;
  hm: PName;
  typeLoc: TLoc;
  name: TIdent;
  ts: PTypeSpec;
  d: PDecl;
begin
  consumeToken; { 'type' }
  if (tok.kind <> tkIdentifier) or (inMembers and isVisibilityWord) then
  begin
    diagError(tok.loc, 'expected type name after ''type''');
    exit;
  end;
  while (tok.kind = tkIdentifier) and not (inMembers and isVisibilityWord) and
        not startsGeneric and not diagShouldStop do
  begin
    name := makeIdentifier(tok);
    if not inMembers then
      declareName(name);
    consumeToken;
    ts := nil;
    if expect(tkEqual, 'after type name') then
    begin
      { T = type X (Free Pascal): a distinct type, here an alias of X; or
        T = type helper for X (objfpc) }
      if tok.kind = tkKwType then
      begin
        typeLoc := consumeToken;
        if isIdentifierNamed('helper') and (peekKind in [tkKwFor, tkLParen]) then
          ts := parseTypeHelper(typeLoc, false)
        else
        begin
          if mode = lmISO then
            diagError(typeLoc, '''type'' before a type (a distinct type) is a ' +
                               'Free Pascal extension; use --mode=tp');
          ts := parseType;
        end;
      end
      else
        ts := parseType;
    end;
    parseHints(hf, hm);
    if ts <> nil then
    begin
      d := newDecl(dkType, name, name.loc);
      d^.typeSpec := ts;
      addHints(d, hf, hm);
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
  count: longint;
  isCVar: boolean;
  dirLoc: TLoc;
  libExpr, nameExpr: PNode;
  listed: boolean;
  hf: longint;
  hm: PName;
  names: TPtrList;
  ts: PTypeSpec;
  i: longint;
  init, absTarget: PNode;
  eqLoc, absLoc: TLoc;
  v: PDecl;
begin
  consumeToken; { 'var' }
  if tok.kind <> tkIdentifier then
  begin
    diagError(tok.loc, 'expected variable name after ''var''');
    exit;
  end;
  while (tok.kind = tkIdentifier) and not startsGeneric and not diagShouldStop do
  begin
    listInit(names);
    ts := nil;
    listed := parseIdentList(names, 'variable name');
    for i := 0 to names.count - 1 do
      declareName(PIdent(listGet(names, i))^);
    if listed and expect(tkColon, 'after variable name') then
      ts := parseType;
    { Turbo/FPC: an overlay  x: T absolute y;  (one name only); absolute
      is not a reserved word }
    absTarget := nil;
    if (ts <> nil) and (tok.kind = tkIdentifier) and (nameStr(tok.name) = 'absolute') then
    begin
      absLoc := consumeToken;
      if mode = lmISO then
        diagError(absLoc, '''absolute'' is a Turbo/Free Pascal extension; ' +
                  'use --mode=tp')
      else if names.count > 1 then
        diagErrorR(absLoc, '''absolute'' can only be used with one variable',
                   makeRange(PIdent(listGet(names, 0))^.loc,
                             identRange(PIdent(listGet(names, names.count - 1))^).e));
      absTarget := parseExpression;
      if absTarget = nil then
        ts := nil;
    end;
    { objfpc: an initialized variable  x: T = value;  (one name only) }
    init := nil;
    if (ts <> nil) and (tok.kind = tkEqual) then
    begin
      eqLoc := consumeToken;
      if mode <> lmObjFPC then
        diagError(eqLoc, 'initialized variables are a Free Pascal extension; ' +
                  'use --mode=objfpc')
      else if names.count > 1 then
        diagErrorR(eqLoc, 'only one variable can be initialized',
                   makeRange(PIdent(listGet(names, 0))^.loc,
                             identRange(PIdent(listGet(names, names.count - 1))^).e));
      init := parseTypedConstValue;
      if init = nil then
        ts := nil
      else if (mode <> lmObjFPC) or (names.count > 1) then
        init := nil; { (reported) }
    end;
    parseHints(hf, hm);
    if ts = nil then
      skipUntil([tkSemi, tkKwBegin])
    else
      for i := 0 to names.count - 1 do
      begin
        v := newVar(PIdent(listGet(names, i))^, ts, vkVariable);
        if init <> nil then
        begin
          v^.init := init;
          v^.isInitVar := true;
        end;
        v^.absTarget := absTarget;
        addHints(v, hf, hm);
        listAdd(decls, v);
      end;
    if not expect(tkSemi, 'after variable declaration') then
    begin
      skipUntil([tkSemi, tkKwBegin]);
      tryConsume(tkSemi);
      continue;
    end;

    { Turbo/FPC: a C variable  x: T; [cvar;] external ['lib'] [name 'sym'];
      (neither word is reserved: external: T declares a variable) }
    count := 0;
    if ts <> nil then
      count := names.count;
    while (mode <> lmISO) and
          (isIdentifierNamed('cvar') or isIdentifierNamed('external')) and
          not (peekKind in [tkColon, tkComma]) do
    begin
      isCVar := isIdentifierNamed('cvar');
      dirLoc := consumeToken;
      libExpr := nil;
      nameExpr := nil;
      if not isCVar then
      begin
        if (tok.kind <> tkSemi) and not isIdentifierNamed('name') then
          libExpr := parseExpression;
        if isIdentifierNamed('name') then
        begin
          consumeToken;
          if tok.kind = tkSemi then
            diagError(tok.loc, 'expected the external symbol name')
          else
            nameExpr := parseExpression;
        end;
        if (nameExpr <> nil) and (count > 1) then
          diagErrorR(dirLoc, '''name'' can only be given for one variable',
                     makeRange(PIdent(listGet(names, 0))^.loc,
                               identRange(PIdent(listGet(names, names.count - 1))^).e));
      end;
      if isCVar then
        expect(tkSemi, 'after ''cvar''')
      else
        expect(tkSemi, 'after ''external''');
      for i := decls.count - count to decls.count - 1 do
      begin
        v := PDecl(listGet(decls, i));
        if isCVar then
          v^.varCVar := true
        else
        begin
          v^.varExternal := true;
          v^.varLibExpr := libExpr;
          v^.varNameExpr := nameExpr;
        end;
      end;
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
  r^.methodKind := mtNone;
  r^.methodClassName := noIdent;
  r^.vmtIndex := -1;
  newRoutine := r;
end;

function methodKindOf(k: TTokenKind): TMethodKind;
begin
  case k of
    tkKwConstructor: methodKindOf := mtConstructor;
    tkKwDestructor: methodKindOf := mtDestructor;
  else
    methodKindOf := mtNone;
  end;
end;

function routineKindWord(k: TTokenKind): string;
begin
  case k of
    tkKwFunction: routineKindWord := 'function';
    tkKwConstructor: routineKindWord := 'constructor';
    tkKwDestructor: routineKindWord := 'destructor';
  else
    routineKindWord := 'procedure';
  end;
end;

function parseTypeIdentifier(const context: string): PTypeSpec; forward;

{ routineDecl = ("procedure" name [params] | "function" name [params]
                [":" typeIdent]) ";" ( "forward" | block ) ";" .
  name = ident [ "." ident ]   (objfpc: a method implementation, TClass.M;
  constructors and destructors are always methods) }
{ The inline directive (after the keyword at loc): whether the routine is to
  be inlined. It needs $inline on, the default in objfpc mode (as in Free
  Pascal); with $inline off there it is accepted and ignored. }
function parseInlineDirective(loc: TLoc): boolean;
begin
  expect(tkSemi, 'after ''inline''');
  if lx.inlineOn then
    exit(true);
  if mode <> lmObjFPC then
    diagError(loc, 'the ''inline'' directive needs {$inline on}');
  parseInlineDirective := false;
end;

function parseRoutineDecl(headingOnly: boolean): PDecl;
var
  hf: longint;
  hm: PName;
  cc: longint;
  ccw: string;
  isFunction, hasParamList, isExternal, overloadDir, inlineDir: boolean;
  isVarArgs, resultName: boolean;
  inlineLoc, forwardLoc, varArgsLoc, staticLoc: TLoc;
  libraryName: PName;
  libraryExpr, nameExpr: PNode;
  kind: string;
  loc: TLoc;
  name, clsName, typeParam: TIdent;
  i: longint;
  params, outerNames, typeParams: TPtrList;
  resultSpec: PTypeSpec;
  externalName: PName;
  r: PDecl;
  body: PBlock;
  mk: TMethodKind;
begin
  parseRoutineDecl := nil;
  isFunction := tok.kind = tkKwFunction;
  resultName := lx.resultSwitch; { $modeswitch result here }
  mk := methodKindOf(tok.kind);
  kind := routineKindWord(tok.kind);
  loc := consumeToken;

  name := noIdent;
  clsName := noIdent;
  listInit(outerNames);
  if not expectIdentifier(name, kind + ' name') then
  begin
    skipUntil([tkSemi, tkKwBegin]);
    tryConsume(tkSemi);
  end
  else if (mode = lmObjFPC) and (tok.kind = tkPeriod) then
  begin
    { TFoo.Bar, or TOuter.TInner.Bar (a nested class or record) }
    while tok.kind = tkPeriod do
    begin
      consumeToken;
      if identValid(clsName) then
        listAdd(outerNames, newIdent(clsName));
      clsName := name;
      if not expectIdentifier(name, 'method name after ''.''') then
      begin
        name := noIdent;
        skipUntil([tkSemi, tkKwBegin]);
        tryConsume(tkSemi);
        break;
      end;
    end;
  end;
  if identValid(name) and not identValid(clsName) then
    declareName(name); { (declared(NAME); a method's is its class's) }
  if parsingGeneric and identValid(name) and (tok.kind = tkLess) then { <T, U> }
  begin
    consumeToken;
    listInit(typeParams);
    repeat
      if not expectIdentifier(typeParam, 'type parameter name') then
        break;
      listAdd(typeParams, newIdent(typeParam));
      if tok.kind = tkColon then
      begin
        diagError(tok.loc, 'constraints on type parameters are not supported yet');
        skipUntil([tkGreater, tkSemi]);
      end;
    until not (tryConsume(tkComma) or tryConsume(tkSemi));
    expect(tkGreater, 'after the type parameters');
    genericTypeParams := typeParams;
  end;
  if identValid(name) and (mk <> mtNone) and not identValid(clsName) then
  begin
    if parsingClassInit then
      kind := 'class ' + kind;
    diagErrorR(name.loc, 'a ' + kind + ' must be declared in a class; ' +
               'implement it as ''' + kind + ' TClass.' + nameStr(name.spelling) +
               '''', identRange(name));
  end;
  if identValid(clsName) and (mk = mtNone) then
    mk := mtMethod;

  listInit(params);
  hasParamList := tok.kind = tkLParen;
  if hasParamList and not parseFormalParams(params) then
    skipUntil([tkColon, tkSemi, tkKwBegin]);

  resultSpec := nil;
  if tryConsume(tkColon) then
  begin
    if not isFunction then
      diagError(prevTokEnd, 'a ' + kind + ' cannot have a result type');
    resultSpec := parseTypeIdentifier('function result type');
  end;

  if not expect(tkSemi, 'after ' + kind + ' heading') then
    skipUntil([tkSemi, tkKwBegin, tkKwVar, tkKwConst, tkKwType]);
  tryConsume(tkSemi);

  { Turbo/FPC directives:  cdecl;  stdcall;  varargs;
    external [library] [name symbol];  (library, symbol: string constants)
    objfpc: overload; }
  cc := ccDefault;
  isVarArgs := false;
  varArgsLoc := noLoc;
  libraryName := nil;
  libraryExpr := nil;
  nameExpr := nil;
  isExternal := false;
  overloadDir := false;
  inlineDir := false;
  inlineLoc := noLoc;
  staticLoc := noLoc;
  externalName := nil;
  hf := 0;
  hm := nil;
  while mode <> lmISO do
  begin
    if parseSeparatedHint(hf, hm) then
      continue;
    if tok.kind = tkKwInline then
    begin
      inlineLoc := consumeToken;
      inlineDir := parseInlineDirective(inlineLoc);
      continue;
    end;
    if callConvOfTok(tok) <> ccDefault then
    begin
      ccw := callConvName(callConvOfTok(tok));
      takeCallConv(cc);
      expect(tkSemi, 'after ''' + ccw + '''');
      continue;
    end;
    if isIdentifierNamed('rtlproc') then { Free Pascal's RTL marker: no effect }
    begin
      consumeToken;
      expect(tkSemi, 'after ''rtlproc''');
      continue;
    end;
    { static: a class method's implementation repeating its declaration's
      (Free Pascal; Sema checks it), or no effect on a plain routine }
    if isIdentifierNamed('static') and (peekKind = tkSemi) then
    begin
      staticLoc := consumeToken;
      expect(tkSemi, 'after ''static''');
      continue;
    end;
    if isIdentifierNamed('varargs') then
    begin
      varArgsLoc := consumeToken;
      isVarArgs := true;
      expect(tkSemi, 'after ''varargs''');
      continue;
    end;
    if (mode = lmObjFPC) and isIdentifierNamed('overload') then
    begin
      consumeToken;
      overloadDir := true;
      expect(tkSemi, 'after ''overload''');
      continue;
    end;
    if not isIdentifierNamed('external') then
      break;
    consumeToken;
    isExternal := true;
    { the library (linked with the program) and the symbol: string
      constants (literals here; Sema evaluates the others) }
    if (tok.kind <> tkSemi) and not isIdentifierNamed('name') then
    begin
      libraryExpr := parseExpression;
      if (libraryExpr <> nil) and (libraryExpr^.kind = nkStringLit) then
        libraryName := libraryExpr^.strValue;
    end;
    if isIdentifierNamed('name') then
    begin
      consumeToken;
      if tok.kind = tkSemi then
        diagError(tok.loc, 'expected the external symbol name')
      else
      begin
        nameExpr := parseExpression;
        if (nameExpr <> nil) and (nameExpr^.kind = nkStringLit) then
          externalName := nameExpr^.strValue;
      end;
    end;
    expect(tkSemi, 'after ''external''');
  end;
  if isVarArgs and not isExternal then
    diagError(varArgsLoc, '''varargs'' is only allowed on external routines');
  if isExternal then
  begin
    if inlineLoc <> noLoc then
      diagError(inlineLoc, '''inline'' cannot be used with ''external''');
    if not identValid(name) then
      exit;
    r := newRoutine(loc, name, isFunction, params, hasParamList, resultSpec,
                    nil, false);
    r^.isExternal := true;
    r^.externalName := externalName;
    r^.libraryName := libraryName;
    r^.libraryExpr := libraryExpr;
    r^.nameExpr := nameExpr;
    r^.isVarArgs := isVarArgs;
    r^.hasOverloadDirective := overloadDir;
    r^.callConv := cc;
    addHints(r, hf, hm);
    exit(r);
  end;

  if headingOnly and identValid(clsName) then
  begin
    diagErrorR(clsName.loc,
               'methods are implemented in the implementation part of a unit',
               makeRange(clsName.loc, identRange(name).e));
    exit;
  end;
  if headingOnly then { interface part of a unit: the body comes later }
  begin
    if identValid(name) then
    begin
      r := newRoutine(loc, name, isFunction, params, hasParamList, resultSpec,
                      nil, true);
      r^.hasOverloadDirective := overloadDir;
      r^.isInline := inlineDir;
      r^.callConv := cc;
      r^.staticDirLoc := staticLoc;
      r^.resultName := resultName;
      addHints(r, hf, hm);
      parseRoutineDecl := r;
    end;
    exit;
  end;

  if isIdentifierNamed('forward') then
  begin
    forwardLoc := consumeToken;
    if inlineLoc <> noLoc then
      diagError(forwardLoc, '''forward'' cannot be used with ''inline''');
    expect(tkSemi, 'after ''forward''');
    if identValid(name) then
    begin
      r := newRoutine(loc, name, isFunction, params, hasParamList, resultSpec,
                      nil, true);
      r^.hasOverloadDirective := overloadDir;
      r^.isInline := inlineDir;
      r^.callConv := cc;
      r^.staticDirLoc := staticLoc;
      r^.resultName := resultName;
      addHints(r, hf, hm);
      parseRoutineDecl := r;
    end;
    exit;
  end;

  { declared(NAME): its parameters and declarations in a scope of its own }
  lexPushNames(lx);
  for i := 0 to params.count - 1 do
    declareName(PDecl(listGet(params, i))^.ident);
  body := parseBlock;
  lexPopNames(lx);
  if not expect(tkSemi, 'after ' + kind + ' body') then
  begin
    skipUntil([tkSemi, tkKwBegin, tkKwProcedure, tkKwFunction]);
    tryConsume(tkSemi);
  end;
  if identValid(name) then
  begin
    r := newRoutine(loc, name, isFunction, params, hasParamList, resultSpec,
                    body, false);
    r^.methodKind := mk;
    r^.methodClassName := clsName;
    r^.outerClassNames := outerNames;
    r^.hasOverloadDirective := overloadDir;
    r^.isInline := inlineDir;
    r^.callConv := cc;
    r^.staticDirLoc := staticLoc;
    r^.resultName := resultName;
    addHints(r, hf, hm);
    parseRoutineDecl := r;
  end;
end;

// methodHeading = ("procedure" | "function" | "constructor" | "destructor")
//                 ident [params] [":" typeIdent] ";" { directive ";" } .
// directive     = "virtual" | "override" | "abstract" | "overload"
//               | "reintroduce" .
{ inRecord: in an advanced record (static allowed); recordClassMethod: a
  record's class method, which must be static }
function parseMethodHeadingR(inRecord, recordClassMethod: boolean): PDecl;
var
  hf: longint;
  hm: PName;
  isFunction, hasParamList, isVirtual, isOverride, isAbstract, overloadDir: boolean;
  isStatic, inlineDir: boolean;
  inlineLoc: TLoc;
  mk: TMethodKind;
  kind, wd: string;
  cc: longint;
  loc: TLoc;
  name: TIdent;
  params: TPtrList;
  resultSpec: PTypeSpec;
  r: PDecl;
begin
  parseMethodHeadingR := nil;
  isFunction := tok.kind = tkKwFunction;
  mk := methodKindOf(tok.kind);
  if mk = mtNone then
    mk := mtMethod;
  kind := routineKindWord(tok.kind);
  loc := consumeToken;

  if not expectIdentifier(name, kind + ' name') then
    exit;
  listInit(params);
  hasParamList := tok.kind = tkLParen;
  if hasParamList and not parseFormalParams(params) then
    exit;
  resultSpec := nil;
  if tryConsume(tkColon) then
  begin
    if not isFunction then
      diagError(prevTokEnd, 'a ' + kind + ' cannot have a result type');
    resultSpec := parseTypeIdentifier('function result type');
    if resultSpec = nil then
      exit;
  end;
  if not expect(tkSemi, 'after ' + kind + ' heading') then
    exit;

  isVirtual := false;
  isOverride := false;
  isAbstract := false;
  overloadDir := false;
  isStatic := false;
  inlineDir := false;
  inlineLoc := noLoc;
  cc := ccDefault;
  hf := 0;
  hm := nil;
  while (tok.kind = tkIdentifier) or (tok.kind = tkKwInline) do
  begin
    if parseSeparatedHint(hf, hm) then
      continue;
    if tok.kind = tkKwInline then
    begin
      inlineLoc := consumeToken;
      inlineDir := parseInlineDirective(inlineLoc);
      continue;
    end;
    if callConvOfTok(tok) <> ccDefault then
    begin
      wd := callConvName(callConvOfTok(tok));
      takeCallConv(cc);
      if not expect(tkSemi, 'after ''' + wd + '''') then
        exit;
      continue;
    end;
    wd := nameStr(tok.name);
    if wd = 'static' then { a record's class method, or (objfpc) a class's }
      isStatic := true
    else if wd = 'virtual' then
      isVirtual := true
    else if wd = 'override' then
      isOverride := true
    else if wd = 'abstract' then
      isAbstract := true
    else if wd = 'overload' then
      overloadDir := true
    else if (wd = 'reintroduce') or
            ((wd = 'rtlproc') and not (peekKind in [tkColon, tkComma])) then
      { reintroduce: hides an inherited virtual method without a warning;
        rtlproc: Free Pascal's RTL marker (a field if followed by : or ,) }
    else if (wd = 'dynamic') or (wd = 'message') or
            (wd = 'final') or (wd = 'external') or (wd = 'forward') then
    begin
      unsupported('''' + wd + ''' on methods is');
      exit;
    end
    else
      break; { the next member (a field) }
    consumeToken;
    if not expect(tkSemi, 'after ''' + wd + '''') then
      exit;
  end;
  if (inlineLoc <> noLoc) and (isVirtual or isOverride or isAbstract) then
  begin
    if isVirtual then
      diagError(inlineLoc, '''inline'' cannot be used with ''virtual''')
    else if isOverride then
      diagError(inlineLoc, '''inline'' cannot be used with ''override''')
    else
      diagError(inlineLoc, '''inline'' cannot be used with ''abstract''');
  end;
  if recordClassMethod and not isStatic then
    diagErrorR(name.loc, 'the class methods of a record must be ''static''',
               identRange(name))
  else if inRecord and isStatic and not recordClassMethod then
    diagErrorR(name.loc, 'only the class methods of a record can be ''static''',
               identRange(name));
  r := newRoutine(loc, name, isFunction, params, hasParamList, resultSpec,
                  nil, false);
  r^.methodKind := mk;
  r^.isVirtual := isVirtual;
  r^.isOverride := isOverride;
  r^.isAbstract := isAbstract;
  r^.hasOverloadDirective := overloadDir;
  r^.isInline := inlineDir and not isVirtual and not isOverride and not isAbstract;
  r^.callConv := cc;
  addHints(r, hf, hm);
  if isStatic and not inRecord then { a class's (Sema checks it is a class method) }
    r^.isStatic := true;
  parseMethodHeadingR := r;
end;

{ Makes r a class constructor or destructor (a static class procedure). }
procedure markClassInit(r: PDecl; ci: longint);
begin
  r^.methodKind := mtMethod;
  r^.isClassMethod := true;
  r^.isStatic := true;
  r^.classInit := ci;
end;

{ classInit = ("class constructor" | "class destructor") name ";"
              ( directive ";" )      (in a class or an advanced record) }
function parseClassInitHeading: PDecl;
var
  ci: longint;
  r: PDecl;
begin
  if tok.kind = tkKwConstructor then
    ci := ciConstructor
  else
    ci := ciDestructor;
  r := parseMethodHeadingR(false, false);
  if r <> nil then
    markClassInit(r, ci);
  parseClassInitHeading := r;
end;

function parseMethodHeading: PDecl;
begin
  parseMethodHeading := parseMethodHeadingR(false, false);
end;

{ operatorName = "+" | "-" | "*" | "/" | "=" | "<>" | "<" | ">" | "<=" | ">="
                 | ":=" | "div" | "mod" | "and" | "or" | "xor" | "shl" | "shr"
                 | "in" | "not" | "explicit" .
  The name of the operator's routine: "operator" followed by the symbol
  (lower case), spelled "operator <symbol>". }
function parseOperatorName(var name: TIdent): boolean;
var
  sym: string;
begin
  case tok.kind of
    tkPlus: sym := '+';
    tkMinus: sym := '-';
    tkStar: sym := '*';
    tkSlash: sym := '/';
    tkEqual: sym := '=';
    tkNotEqual: sym := '<>';
    tkLess: sym := '<';
    tkGreater: sym := '>';
    tkLessEqual: sym := '<=';
    tkGreaterEqual: sym := '>=';
    tkColonEqual: sym := ':=';
    tkKwDiv: sym := 'div';
    tkKwMod: sym := 'mod';
    tkKwAnd: sym := 'and';
    tkKwOr: sym := 'or';
    tkKwXor: sym := 'xor';
    tkKwShl: sym := 'shl';
    tkKwShr: sym := 'shr';
    tkKwIn: sym := 'in';
    tkKwNot: sym := 'not';
  else
    sym := '';
  end;
  if (sym = '') and isIdentifierNamed('explicit') then
    sym := 'explicit';
  if sym = '' then
  begin
    diagError(tok.loc, 'expected an operator (+, -, *, /, =, <>, <, >, <=, >=, :=, ' +
              'div, mod, and, or, xor, shl, shr, in, not or Explicit)');
    exit(false);
  end;
  name.name := intern('operator' + sym);
  name.spelling := intern('operator ' + rawText(tok));
  name.loc := tok.loc;
  consumeToken;
  parseOperatorName := true;
end;

{ operatorHeading = "operator" operatorName "(" params ")" ":" typeIdent ";"
                    [ "overload" ";" ] .      (after "class", in a record) }
function parseOperatorHeading: PDecl;
var
  loc: TLoc;
  name: TIdent;
  params: TPtrList;
  resultSpec: PTypeSpec;
  overloadDir, inlineDir: boolean;
  r: PDecl;
begin
  parseOperatorHeading := nil;
  loc := consumeToken; { 'operator' }
  if not parseOperatorName(name) then
    exit;
  listInit(params);
  if tok.kind <> tkLParen then
  begin
    diagError(tok.loc, 'expected ''('' after the operator');
    exit;
  end;
  if not parseFormalParams(params) or
     not expect(tkColon, 'after the operator''s parameters') then
    exit;
  resultSpec := parseTypeIdentifier('operator result type');
  if (resultSpec = nil) or not expect(tkSemi, 'after operator heading') then
    exit;
  overloadDir := false;
  inlineDir := false;
  while true do
  begin
    if isIdentifierNamed('overload') then
    begin
      consumeToken;
      overloadDir := true;
      if not expect(tkSemi, 'after ''overload''') then
        exit;
      continue;
    end;
    if tok.kind = tkKwInline then
    begin
      inlineDir := parseInlineDirective(consumeToken);
      continue;
    end;
    break;
  end;
  r := newRoutine(loc, name, true, params, true, resultSpec, nil, false);
  r^.methodKind := mtOperator;
  r^.isClassMethod := true;
  r^.hasOverloadDirective := overloadDir;
  r^.isInline := inlineDir;
  parseOperatorHeading := r;
end;

{ operatorDecl = "operator" ident "." operatorName "(" params ")" ":"
                 typeIdent ";" block ";" .        (after "class") }
function parseOperatorDecl(headingOnly: boolean): PDecl;
var
  i: longint;
  loc: TLoc;
  clsName, name: TIdent;
  params: TPtrList;
  resultSpec: PTypeSpec;
  body: PBlock;
  ok, resultName: boolean;
  r: PDecl;
begin
  parseOperatorDecl := nil;
  resultName := lx.resultSwitch; { $modeswitch result here }
  loc := consumeToken; { 'operator' }
  listInit(params);
  resultSpec := nil;
  ok := expectIdentifier(clsName, 'record name after ''operator''') and
        expect(tkPeriod, 'after the record name') and parseOperatorName(name);
  if ok then
  begin
    if tok.kind = tkLParen then
      ok := parseFormalParams(params)
    else
    begin
      diagError(tok.loc, 'expected ''('' after the operator');
      ok := false;
    end;
  end;
  ok := ok and expect(tkColon, 'after the operator''s parameters');
  if ok then
  begin
    resultSpec := parseTypeIdentifier('operator result type');
    ok := resultSpec <> nil;
  end;
  ok := ok and expect(tkSemi, 'after operator heading');
  if not ok then
  begin
    skipUntil([tkSemi, tkKwBegin]);
    tryConsume(tkSemi);
    exit;
  end;
  if headingOnly then
  begin
    diagErrorR(clsName.loc,
               'methods are implemented in the implementation part of a unit',
               makeRange(clsName.loc, identRange(name).e));
    exit;
  end;
  lexPushNames(lx);
  for i := 0 to params.count - 1 do
    declareName(PDecl(listGet(params, i))^.ident);
  body := parseBlock;
  lexPopNames(lx);
  if not expect(tkSemi, 'after operator body') then
  begin
    skipUntil([tkSemi, tkKwBegin, tkKwProcedure, tkKwFunction]);
    tryConsume(tkSemi);
  end;
  r := newRoutine(loc, name, true, params, true, resultSpec, body, false);
  r^.methodKind := mtOperator;
  r^.methodClassName := clsName;
  r^.isClassMethod := true;
  r^.resultName := resultName;
  parseOperatorDecl := r;
end;

// property = "property" ident ":" typeIdent [ "read" ident ] [ "write" ident ] ";" .
{ property = "property" ident [ "[" params "]" ] ":" typeIdent
             [ "index" constant ] [ "read" ident ] [ "write" ident ]
             [ "default" constant | "nodefault" ] ";" [ "default" ";" ] . }
function parsePropertyDecl: PDecl;
var
  name, readName, writeName: TIdent;
  ts: PTypeSpec;
  wd: string;
  p: PDecl;
  params: TPtrList;
  indexE, defaultE, e: PNode;
  noDefault: boolean;
begin
  parsePropertyDecl := nil;
  consumeToken; { 'property' }
  if not expectIdentifier(name, 'property name') then
    exit;
  listInit(params);
  if tok.kind = tkLSquare then
  begin
    if not parseFormalParamsClose(params, tkRSquare) or (params.count = 0) then
      exit; { (an empty list was reported) }
  end;
  if not expect(tkColon, 'after property name') then
    exit;
  ts := parseTypeIdentifier('property type');
  if ts = nil then
    exit;
  readName := noIdent;
  writeName := noIdent;
  indexE := nil;
  defaultE := nil;
  noDefault := false;
  while tok.kind = tkIdentifier do
  begin
    wd := nameStr(tok.name);
    if wd = 'read' then
    begin
      consumeToken;
      if not expectIdentifier(readName, 'field or method name after ''read''') then
        exit;
      continue;
    end;
    if wd = 'write' then
    begin
      consumeToken;
      if not expectIdentifier(writeName, 'field or method name after ''write''') then
        exit;
      continue;
    end;
    if (wd = 'index') or (wd = 'default') then
    begin
      consumeToken;
      e := parseExpression;
      if e = nil then
        exit;
      if wd = 'index' then
        indexE := e
      else
        defaultE := e;
      continue;
    end;
    if wd = 'nodefault' then
    begin
      consumeToken;
      noDefault := true;
      continue;
    end;
    if (wd = 'stored') or (wd = 'implements') then
    begin
      unsupported('''' + wd + ''' in a property is');
      exit;
    end;
    break;
  end;
  if not expect(tkSemi, 'after property declaration') then
    exit;
  p := newDecl(dkProperty, name, name.loc);
  p^.propSpec := ts;
  p^.readName := readName;
  p^.writeName := writeName;
  p^.propParams := params;
  p^.propIndex := indexE;
  p^.propDefaultValue := defaultE;
  p^.propNoDefault := noDefault;
  p^.propIsDefault := false;
  { property Items[i: integer]: T read GetItem; default; }
  if isIdentifierNamed('default') then
  begin
    consumeToken;
    p^.propIsDefault := true;
    if not expect(tkSemi, 'after ''default''') then
      exit;
  end;
  parsePropertyDecl := p;
end;

function parseGenericRoutine(headingOnly: boolean): PDecl;
var
  genericLoc: TLoc;
  toks: PGenericTokens;
  rec: TPtrList;
  savedRec: PPtrList;
  r: PDecl;
  kind: string;
begin
  parseGenericRoutine := nil;
  genericLoc := consumeToken; { 'generic' }
  toks := PGenericTokens(arenaAlloc(sizeof(TGenericTokens)));
  toks^.longStrings := lx.longStrings;
  toks^.advancedRecords := lx.advancedRecords;
  toks^.corbaInterfaces := lx.corbaInterfaces;
  toks^.inlineOn := lx.inlineOn;
  toks^.typeHelpers := lx.typeHelpers;
  toks^.scopedEnums := lx.scopedEnums;
  toks^.packRecords := lx.packRecords;
  toks^.resultSwitch := lx.resultSwitch;
  toks^.outSwitch := lx.outSwitch;
  toks^.defaultParamsSwitch := lx.defaultParamsSwitch;
  listInit(rec);
  savedRec := recording;
  recording := @rec;
  parsingGeneric := true;
  listInit(genericTypeParams);
  r := parseRoutineDecl(headingOnly);
  parsingGeneric := false;
  recording := savedRec;
  if r = nil then
    exit;
  if identValid(r^.methodClassName) then
  begin
    diagError(genericLoc, 'generic methods are not supported yet');
    exit;
  end;
  if genericTypeParams.count = 0 then
  begin
    if r^.isFunction then
      kind := 'function'
    else
      kind := 'procedure';
    diagErrorR(r^.ident.loc, 'expected ''<'' and type parameters after the name ' +
               'of generic ' + kind + ' ''' + nameStr(r^.ident.spelling) +
               '''', identRange(r^.ident));
    exit;
  end;
  toks^.toks := rec;
  r^.isGeneric := true;
  r^.typeParams := genericTypeParams;
  r^.genericTokens := toks;
  parseGenericRoutine := r;
end;

{ specialize Name<T1, ...>, after which a call's arguments may follow. }
function parseSpecialization(var name: TIdent; var typeArgs: TPtrList): boolean;
var
  t: PTypeSpec;
begin
  parseSpecialization := false;
  consumeToken; { 'specialize' }
  if not expectIdentifier(name, 'generic routine name after ''specialize''') or
     not expect(tkLess, 'after the name of a generic routine') then
    exit;
  repeat
    t := parseTypeIdentifier('type argument');
    if t = nil then
      exit;
    listAdd(typeArgs, t);
  until not tryConsume(tkComma);
  parseSpecialization := expect(tkGreater, 'after the type arguments');
end;

function reparseGeneric(g: PDecl): PDecl;
var
  savedLx: TLexer;
  savedTok, savedPeek: TToken;
  savedHasPeek, savedGeneric: boolean;
  savedPrev: TLoc;
  savedReplay: pointer;
  savedPos, savedNesting: longint;
  savedRec, savedStmtToks: PPtrList;
  toks: PGenericTokens;
begin
  savedLx := lx;
  savedTok := tok;
  savedPeek := peekTok;
  savedHasPeek := hasPeek;
  savedPrev := prevTokEnd;
  savedReplay := replay;
  savedPos := replayPos;
  savedNesting := nesting;
  savedRec := recording;
  savedStmtToks := stmtTokens;
  savedGeneric := parsingGeneric;
  toks := PGenericTokens(g^.genericTokens);
  lx.longStrings := toks^.longStrings;
  lx.advancedRecords := toks^.advancedRecords;
  lx.corbaInterfaces := toks^.corbaInterfaces;
  lx.inlineOn := toks^.inlineOn;
  lx.typeHelpers := toks^.typeHelpers;
  lx.scopedEnums := toks^.scopedEnums;
  lx.packRecords := toks^.packRecords;
  lx.resultSwitch := toks^.resultSwitch;
  lx.outSwitch := toks^.outSwitch;
  lx.defaultParamsSwitch := toks^.defaultParamsSwitch;
  recording := nil;
  stmtTokens := nil;
  replay := toks;
  replayPos := 0;
  hasPeek := false;
  nesting := 0;
  nextToken(tok);
  parsingGeneric := true;
  reparseGeneric := parseRoutineDecl(false);
  lx := savedLx;
  tok := savedTok;
  peekTok := savedPeek;
  hasPeek := savedHasPeek;
  prevTokEnd := savedPrev;
  replay := savedReplay;
  replayPos := savedPos;
  nesting := savedNesting;
  recording := savedRec;
  stmtTokens := savedStmtToks;
  parsingGeneric := savedGeneric;
end;

procedure parseDeclarations(var decls: TPtrList; inInterface: boolean);
var
  lastRank, rank: longint;
  lastWord, word: string;
  r: PDecl;
  ci: longint;
  ok: boolean;
begin
  lastRank := -1;
  lastWord := '';
  while (startsDeclarationPart or startsGeneric) and not diagShouldStop do
  begin
    if tok.kind = tkIdentifier then { generic function F<T>... }
    begin
      r := parseGenericRoutine(inInterface);
      if r <> nil then
        listAdd(decls, r);
      continue;
    end;
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
      tkKwConst: parseConstPart(decls, false);
      tkKwType: parseTypePart(decls, false);
      tkKwVar: parseVarPart(decls);
      tkKwProcedure, tkKwFunction, tkKwConstructor, tkKwDestructor:
        begin
          r := parseRoutineDecl(inInterface);
          if r <> nil then
            listAdd(decls, r);
        end;
      tkKwClass: { class procedure TFoo.P ... }
        begin
          consumeToken;
          if (mode = lmObjFPC) and isIdentifierNamed('operator') then
          begin
            r := parseOperatorDecl(inInterface);
            if r <> nil then
              listAdd(decls, r);
          end
          else if tok.kind in [tkKwConstructor, tkKwDestructor] then
          begin
            { class constructor TFoo.Create; ... (a static class procedure) }
            if tok.kind = tkKwConstructor then
              ci := ciConstructor
            else
              ci := ciDestructor;
            parsingClassInit := true;
            r := parseRoutineDecl(inInterface);
            parsingClassInit := false;
            if r <> nil then
            begin
              markClassInit(r, ci);
              listAdd(decls, r);
            end;
          end
          else if (tok.kind <> tkKwProcedure) and (tok.kind <> tkKwFunction) then
          begin
            diagError(tok.loc, 'expected ''procedure'', ''function'', ' +
                      '''constructor'' or ''destructor'' after ''class''');
            skipUntil([tkSemi, tkKwBegin]);
            tryConsume(tkSemi);
          end
          else
          begin
            r := parseRoutineDecl(inInterface);
            if r <> nil then
            begin
              r^.isClassMethod := true;
              listAdd(decls, r);
            end;
          end;
        end;
    else
      begin { label 1, 2, 3; }
        if mode = lmObjFPC then
          diagNoGoto;
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
begin
  parseFormalParams := parseFormalParamsClose(params, tkRParen);
end;

{ close: tkRParen, or tkRSquare for an array property's indexes }
function parseFormalParamsClose(var params: TPtrList; close: TTokenKind): boolean;
var
  lparen, start: TLoc;
  isFunction, isVar, isConst: boolean;
  name: TIdent;
  inner, names: TPtrList;
  resultSpec, ts: PTypeSpec;
  vk: TVarKind;
  i: longint;
  lastName: PIdent;
  seenDefault, untyped, defaultOK, isOut, isConstRef: boolean;
  defValue: PNode;
  eqLoc: TLoc;
  v: PDecl;

  { once a parameter has a default value, the following ones need one }
  procedure needsDefault(const n: TIdent);
  begin
    if seenDefault then
      diagErrorR(n.loc, 'parameter ''' + nameStr(n.spelling) +
                 ''' needs a default value, as a parameter before it has one',
                 identRange(n));
  end;

begin
  lparen := consumeToken; { '(' or '[' }
  seenDefault := false;
  repeat
    if tok.kind in [tkKwProcedure, tkKwFunction] then
    begin
      { procedure p(params) / function f(params): T }
      isFunction := tok.kind = tkKwFunction;
      start := consumeToken;
      if not expectIdentifier(name, 'parameter name') then
      begin
        skipUntil([tkSemi, close]);
        continue;
      end;
      listInit(inner);
      if (tok.kind = tkLParen) and not parseFormalParams(inner) then
      begin
        skipUntil([tkSemi, close]);
        continue;
      end;
      resultSpec := nil;
      if isFunction then
      begin
        if not expect(tkColon, 'before the result type of a functional parameter') then
        begin
          skipUntil([tkSemi, close]);
          continue;
        end;
        resultSpec := parseTypeIdentifier('function result type');
        if resultSpec = nil then
        begin
          skipUntil([tkSemi, close]);
          continue;
        end;
      end;
      ts := newTypeSpec(tsRoutine, start, prevTokEnd);
      ts^.rtIsFunction := isFunction;
      ts^.rtParams := inner;
      ts^.rtResult := resultSpec;
      needsDefault(name);
      listAdd(params, newVar(name, ts, vkValueParam));
      continue;
    end;
    isVar := tryConsume(tkKwVar);
    isConst := false;
    isConstRef := false;
    if not isVar and (tok.kind = tkKwConst) and (mode <> lmISO) then
    begin
      consumeToken;
      isConst := true;
    end;
    { Turbo/FPC: constref (not a reserved word), a const parameter always
      passed by reference }
    if not isVar and not isConst and (mode <> lmISO) and
       (tok.kind = tkIdentifier) and (nameStr(tok.name) = 'constref') and
       (peekKind = tkIdentifier) then
    begin
      consumeToken;
      isConst := true;
      isConstRef := true;
    end;
    { objfpc: out, not a reserved word, is a var parameter here }
    isOut := not isVar and not isConst and lx.outSwitch and (close = tkRParen) and
             (tok.kind = tkIdentifier) and (nameStr(tok.name) = 'out');
    if isOut then
    begin
      consumeToken;
      isVar := true;
    end;
    listInit(names);
    if not parseIdentList(names, 'parameter name') then
    begin
      skipUntil([tkSemi, close]);
      continue;
    end;
    { Turbo/FPC untyped parameters:  var x;  const a, b }
    untyped := false;
    if (isVar or isConst) and (mode <> lmISO) and (tok.kind <> tkColon) then
    begin
      untyped := true;
      lastName := PIdent(listGet(names, names.count - 1));
      ts := newTypeSpec(tsUntyped, lastName^.loc, identRange(lastName^).e);
    end
    else if not expect(tkColon, 'after parameter name') then
    begin
      skipUntil([tkSemi, close]);
      continue;
    end
    else
      ts := parseTypeIdentifier('parameter type');
    if ts = nil then
    begin
      skipUntil([tkSemi, close]);
      continue;
    end;
    if isVar then
      vk := vkVarParam
    else if isConst then
      vk := vkConstParam
    else
      vk := vkValueParam;
    { objfpc: a default value  b: T = value  (one name only) }
    defValue := nil;
    defaultOK := false;
    if (tok.kind = tkEqual) and (close = tkRParen) then
    begin
      eqLoc := consumeToken;
      if not lx.defaultParamsSwitch then
        diagError(eqLoc, 'default parameter values need --mode=objfpc or ' +
                  '{$modeswitch defaultparameters}')
      else if isVar or untyped then
        diagError(eqLoc, 'only value and const parameters can have a default value')
      else if names.count > 1 then
        diagErrorR(eqLoc, 'only one parameter can have a default value',
                   makeRange(PIdent(listGet(names, 0))^.loc,
                             identRange(PIdent(listGet(names, names.count - 1))^).e))
      else
        defaultOK := true;
      defValue := parseExpression;
      if defValue = nil then
      begin
        skipUntil([tkSemi, close]);
        continue;
      end;
    end;
    for i := 0 to names.count - 1 do
    begin
      if defValue = nil then
        needsDefault(PIdent(listGet(names, i))^);
      v := newVar(PIdent(listGet(names, i))^, ts, vk);
      if defaultOK then
        v^.paramDefault := defValue;
      v^.isOut := isOut;
      v^.isConstRef := isConstRef;
      listAdd(params, v);
    end;
    seenDefault := seenDefault or defaultOK;
  until not tryConsume(tkSemi);

  if not expect(close, 'after parameters') then
  begin
    if close = tkRParen then
      diagNote(lparen, 'to match this ''(''')
    else
      diagNote(lparen, 'to match this ''[''');
    exit(false);
  end;
  parseFormalParamsClose := true;
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

function classSpec(b, e: TLoc; const parentName: TIdent;
                   const members: TPtrList; isForward: boolean): PTypeSpec;
var
  ts: PTypeSpec;
begin
  ts := newTypeSpec(tsClass, b, e);
  ts^.parentName := parentName;
  ts^.memberList := members;
  ts^.isForwardClass := isForward;
  classSpec := ts;
end;

// classType = "class" ";"                                  (forward)
//           | "class" [ "(" ident ")" ] ( ";"? | { member } "end" ) .
// member    = visibility | identList ":" type ";" | methodHeading | property .
// On an error in the members, skips to the class's 'end'.
function parseClassType(start: TLoc): PTypeSpec;
var
  hf: longint;
  hm: PName;
  classLoc, lparen: TLoc;
  parentName: TIdent;
  members, names: TPtrList;
  vis: TVisibility;
  wd: string;
  ts: PTypeSpec;
  m: PDecl;
  i: longint;
  nested: TPtrList;
  failed, classVars: boolean;
  className, intfName: TIdent;
  intfNames: TPtrList;
begin
  parseClassType := nil;
  classLoc := consumeToken; { 'class' }
  listInit(members);
  parentName := noIdent;
  if tok.kind = tkSemi then { TFoo = class; }
    exit(classSpec(start, prevTokEnd, parentName, members, true));
  if tryConsume(tkKwOf) then { class of TFoo }
  begin
    if not expectIdentifier(className, 'class name after ''class of''') then
      exit;
    ts := newTypeSpec(tsClassRef, start, prevTokEnd);
    ts^.tname := className;
    exit(ts);
  end;
  listInit(intfNames); { class(TParent, I1, I2) }
  if tok.kind = tkLParen then
  begin
    lparen := consumeToken;
    if not expectIdentifier(parentName, 'parent class name') then
      exit;
    while tryConsume(tkComma) do
    begin
      if not expectIdentifier(intfName, 'interface name') then
        exit;
      listAdd(intfNames, newIdent(intfName));
    end;
    if not expect(tkRParen, 'after parent class name') then
    begin
      diagNote(lparen, 'to match this ''(''');
      exit;
    end;
    if tok.kind = tkSemi then { TFoo = class(TBar); }
    begin
      ts := classSpec(start, prevTokEnd, parentName, members, false);
      ts^.interfaceNames := intfNames;
      exit(ts);
    end;
  end;

  vis := vsPublic;
  failed := false;
  classVars := false; { in a class var section }
  while (tok.kind <> tkKwEnd) and (tok.kind <> tkEof) and not diagShouldStop do
  begin
    if tok.kind = tkIdentifier then
    begin
      wd := nameStr(tok.name);
      if (wd = 'private') or (wd = 'protected') or (wd = 'public') or
         (wd = 'published') then
      begin
        if wd = 'private' then
          vis := vsPrivate
        else if wd = 'protected' then
          vis := vsProtected
        else if wd = 'public' then
          vis := vsPublic
        else
          vis := vsPublished;
        classVars := false;
        consumeToken;
        continue;
      end;
      if wd = 'strict' then { strict private / strict protected }
      begin
        consumeToken;
        if isIdentifierNamed('private') then
          vis := vsStrictPrivate
        else if isIdentifierNamed('protected') then
          vis := vsStrictProtected
        else
        begin
          diagError(tok.loc, 'expected ''private'' or ''protected'' after ''strict''');
          failed := true;
          break;
        end;
        classVars := false;
        consumeToken;
        continue;
      end;
      listInit(names);
      if not parseIdentList(names, 'field name') or
         not expect(tkColon, 'after field name') then
      begin
        failed := true;
        break;
      end;
      ts := nestedType;
      if ts = nil then
      begin
        failed := true;
        break;
      end;
      parseHints(hf, hm);
      for i := 0 to names.count - 1 do
      begin
        if classVars then
        begin
          m := newVar(PIdent(listGet(names, i))^, ts, vkVariable);
          m^.isClassVar := true;
        end
        else
        begin
          m := newDecl(dkField, PIdent(listGet(names, i))^,
                       PIdent(listGet(names, i))^.loc);
          m^.fieldSpec := ts;
        end;
        m^.vis := vis;
        addHints(m, hf, hm);
        listAdd(members, m);
      end;
      if not expect(tkSemi, 'after field declaration') then
      begin
        failed := true;
        break;
      end;
      continue;
    end;
    if tryConsume(tkKwVar) then { instance fields again }
    begin
      classVars := false;
      continue;
    end;
    if tok.kind in [tkKwProcedure, tkKwFunction, tkKwConstructor,
                    tkKwDestructor] then
    begin
      m := parseMethodHeading;
      if m = nil then
      begin
        failed := true;
        break;
      end;
      m^.vis := vis;
      listAdd(members, m);
      classVars := false;
      continue;
    end;
    if tok.kind = tkKwProperty then
    begin
      m := parsePropertyDecl;
      if m = nil then
      begin
        failed := true;
        break;
      end;
      hf := 0;
      hm := nil;
      while parseSeparatedHint(hf, hm) do
        ;
      addHints(m, hf, hm);
      m^.vis := vis;
      listAdd(members, m);
      classVars := false;
      continue;
    end;
    if tok.kind = tkKwClass then
    begin
      consumeToken;
      if tryConsume(tkKwVar) then { class var X: T; ... }
      begin
        classVars := true;
        continue;
      end;
      if tok.kind in [tkKwProcedure, tkKwFunction] then
      begin
        m := parseMethodHeading;
        if m = nil then
        begin
          failed := true;
          break;
        end;
        m^.isClassMethod := true;
        m^.vis := vis;
        listAdd(members, m);
        classVars := false;
        continue;
      end;
      if tok.kind in [tkKwConstructor, tkKwDestructor] then
      begin
        m := parseClassInitHeading;
        if m = nil then
        begin
          failed := true;
          break;
        end;
        m^.vis := vis;
        listAdd(members, m);
        classVars := false;
        continue;
      end;
      if tok.kind = tkKwProperty then { class property P: T read ... }
      begin
        m := parsePropertyDecl;
        if m = nil then
        begin
          failed := true;
          break;
        end;
        hf := 0;
        hm := nil;
        while parseSeparatedHint(hf, hm) do
          ;
        addHints(m, hf, hm);
        m^.propIsClass := true;
        m^.vis := vis;
        listAdd(members, m);
        classVars := false;
        continue;
      end;
      if isIdentifierNamed('operator') then
        diagError(tok.loc, 'class operators can only be declared in records')
      else
        diagError(tok.loc, 'expected ''var'', ''procedure'', ''function'', ' +
                  '''constructor'', ''destructor'' or ''property'' after ''class''');
      failed := true;
      break;
    end;
    if tok.kind in [tkKwConst, tkKwType] then { nested declarations }
    begin
      listInit(nested);
      if tok.kind = tkKwConst then
        parseConstPart(nested, true)
      else
        parseTypePart(nested, true);
      for i := 0 to nested.count - 1 do
      begin
        m := PDecl(listGet(nested, i));
        m^.vis := vis;
        if m^.kind = dkVar then { a typed constant }
          m^.isClassVar := true;
        listAdd(members, m);
      end;
      classVars := false;
      continue;
    end;
    diagError(tok.loc,
              'expected a field, method or property declaration, or ''end''');
    failed := true;
    break;
  end;
  if failed then
  begin
    skipUntil([tkKwEnd]);
    tryConsume(tkKwEnd);
    exit;
  end;
  if not expect(tkKwEnd, 'at end of class type') then
  begin
    diagNote(classLoc, 'to match this ''class''');
    exit;
  end;
  ts := classSpec(start, prevTokEnd, parentName, members, false);
  ts^.interfaceNames := intfNames;
  parseClassType := ts;
end;

{ interfaceType = "interface" ";"                          (forward)
                | "interface" [ "(" ident ")" ] [ "[" string "]" ]
                  method headings "end" .                 (objfpc, CORBA) }
function parseInterfaceType(start: TLoc): PTypeSpec;
var
  intfLoc, lparen: TLoc;
  parentName: TIdent;
  members: TPtrList;
  guid: PName;
  m: PDecl;
  ts: PTypeSpec;
  corba: boolean;
begin
  parseInterfaceType := nil;
  { COM or CORBA as set by the last $interfaces before it }
  corba := lx.corbaInterfaces;
  intfLoc := consumeToken; { 'interface' }
  parentName := noIdent;
  listInit(members);
  guid := nil;
  if tok.kind = tkSemi then { IFoo = interface; }
  begin
    ts := newTypeSpec(tsInterface, start, prevTokEnd);
    ts^.intfParentName := parentName;
    ts^.intfMemberList := members;
    ts^.isForwardIntf := true;
    ts^.guid := nil;
    ts^.isCorba := corba;
    exit(ts);
  end;
  if tok.kind = tkLParen then
  begin
    lparen := consumeToken;
    if not expectIdentifier(parentName, 'parent interface name') then
      exit;
    if not expect(tkRParen, 'after parent interface name') then
    begin
      diagNote(lparen, 'to match this ''(''');
      exit;
    end;
  end;
  if tryConsume(tkLSquare) then { ['GUID'] }
  begin
    if tok.kind <> tkStringLiteral then
    begin
      diagError(tok.loc, 'expected the interface''s GUID string');
      exit;
    end;
    guid := tok.strValue;
    consumeToken;
    if not expect(tkRSquare, 'after the GUID') then
      exit;
  end;
  while (tok.kind <> tkKwEnd) and (tok.kind <> tkEof) and not diagShouldStop do
  begin
    if tok.kind in [tkKwProcedure, tkKwFunction] then
    begin
      m := parseMethodHeading;
      if m = nil then
      begin
        skipUntil([tkKwEnd]);
        tryConsume(tkKwEnd);
        exit;
      end;
      listAdd(members, m);
      continue;
    end;
    if tok.kind = tkKwProperty then
      unsupported('properties in interfaces are')
    else
      diagError(tok.loc, 'expected a method heading or ''end'' in an interface');
    skipUntil([tkKwEnd]);
    tryConsume(tkKwEnd);
    exit;
  end;
  if not expect(tkKwEnd, 'at end of interface type') then
  begin
    diagNote(intfLoc, 'to match this ''interface''');
    exit;
  end;
  ts := newTypeSpec(tsInterface, start, prevTokEnd);
  ts^.intfParentName := parentName;
  ts^.intfMemberList := members;
  ts^.isForwardIntf := false;
  ts^.guid := guid;
  ts^.isCorba := corba;
  parseInterfaceType := ts;
end;

{ typeHelper = "helper" [ "(" ident ")" ] "for" typeIdent members "end" .
  members: visibility words, method headings, class method headings,
  properties and constant parts. After `type` (objfpc, $modeswitch
  typehelpers), or after `record` for a record helper (objfpc, $modeswitch
  advancedrecords) }
{ In a class var section: identList ":" type ";" (with hints), as class
  variables of visibility vis added to members. }
function parseClassVarNames(var members: TPtrList; vis: TVisibility): boolean;
var
  names: TPtrList;
  ts: PTypeSpec;
  hf: longint;
  hm: PName;
  i: longint;
  m: PDecl;
begin
  parseClassVarNames := false;
  listInit(names);
  if not parseIdentList(names, 'class variable name') or
     not expect(tkColon, 'after class variable name') then
    exit;
  ts := parseType;
  if ts = nil then
    exit;
  parseHints(hf, hm);
  for i := 0 to names.count - 1 do
  begin
    m := newVar(PIdent(listGet(names, i))^, ts, vkVariable);
    m^.isClassVar := true;
    m^.vis := vis;
    addHints(m, hf, hm);
    listAdd(members, m);
  end;
  if not tryConsume(tkSemi) and (tok.kind <> tkKwEnd) then
  begin
    expect(tkSemi, 'after class variable declaration');
    exit;
  end;
  parseClassVarNames := true;
end;

function parseTypeHelper(start: TLoc; isRecord: boolean): PTypeSpec;
var
  hf: longint;
  hm: PName;
  helperLoc, lparen: TLoc;
  parentName: TIdent;
  extended, ts: PTypeSpec;
  members, consts: TPtrList;
  vis: TVisibility;
  wd: string;
  m: PDecl;
  i: longint;
  failed, classVars: boolean;
  kindStr: string;
begin
  parseTypeHelper := nil;
  helperLoc := consumeToken; { 'helper' }
  if isRecord then
    kindStr := 'record helper'
  else
    kindStr := 'type helper';
  if isRecord and ((mode <> lmObjFPC) or not lx.advancedRecords) then
    diagError(helperLoc, 'record helpers need --mode=objfpc and ' +
                         '{$modeswitch advancedrecords}')
  else if not isRecord and ((mode <> lmObjFPC) or not lx.typeHelpers) then
    diagError(helperLoc, 'type helpers need --mode=objfpc and ' +
                         '{$modeswitch typehelpers}');
  parentName := noIdent;
  if tok.kind = tkLParen then
  begin
    lparen := consumeToken;
    if not expectIdentifier(parentName, 'parent helper name') then
      exit;
    if not expect(tkRParen, 'after parent helper name') then
    begin
      diagNote(lparen, 'to match this ''(''');
      exit;
    end;
  end;
  if not expect(tkKwFor, 'in a ' + kindStr) then
    exit;
  extended := parseTypeIdentifier('the type a helper extends');
  if extended = nil then
    exit;
  listInit(members);
  vis := vsPublic;
  failed := false;
  classVars := false;
  while (tok.kind <> tkKwEnd) and (tok.kind <> tkEof) and not diagShouldStop do
  begin
    if tok.kind = tkIdentifier then
    begin
      wd := nameStr(tok.name);
      if (wd = 'private') or (wd = 'protected') or (wd = 'public') or
         (wd = 'published') then
      begin
        if wd = 'private' then
          vis := vsPrivate
        else if wd = 'protected' then
          vis := vsProtected
        else if wd = 'public' then
          vis := vsPublic
        else
          vis := vsPublished;
        classVars := false;
        consumeToken;
        continue;
      end;
      if wd = 'strict' then
      begin
        consumeToken;
        if not isIdentifierNamed('private') and not isIdentifierNamed('protected') then
        begin
          diagError(tok.loc, 'expected ''private'' or ''protected'' after ''strict''');
          failed := true;
          break;
        end;
        if isIdentifierNamed('private') then
          vis := vsStrictPrivate
        else
          vis := vsStrictProtected;
        classVars := false;
        consumeToken;
        continue;
      end;
      if classVars then { class var: static storage, not a field }
      begin
        if not parseClassVarNames(members, vis) then
        begin
          failed := true;
          break;
        end;
        continue;
      end;
      diagError(tok.loc, 'a ' + kindStr + ' cannot contain fields');
      failed := true;
      break;
    end;
    classVars := false; { (anything else ends a class var section) }
    if tok.kind = tkKwVar then
    begin
      diagError(tok.loc, 'a ' + kindStr + ' cannot contain fields');
      failed := true;
      break;
    end;
    if tok.kind in [tkKwConstructor, tkKwDestructor] then
    begin
      diagError(tok.loc, 'constructors and destructors in ' + kindStr +
                's are not supported yet');
      failed := true;
      break;
    end;
    if tok.kind in [tkKwProcedure, tkKwFunction] then
    begin
      m := parseMethodHeadingR(true, false);
      if m = nil then
      begin
        failed := true;
        break;
      end;
      m^.vis := vis;
      listAdd(members, m);
      continue;
    end;
    if tok.kind = tkKwClass then
    begin
      consumeToken;
      if tryConsume(tkKwVar) then { class variables follow }
      begin
        classVars := true;
        continue;
      end;
      if not (tok.kind in [tkKwProcedure, tkKwFunction]) then
      begin
        diagError(tok.loc, 'expected ''procedure'', ''function'' or ''var'' after ''class''');
        failed := true;
        break;
      end;
      m := parseMethodHeadingR(true, true);
      if m = nil then
      begin
        failed := true;
        break;
      end;
      m^.isClassMethod := true;
      m^.vis := vis;
      listAdd(members, m);
      continue;
    end;
    if tok.kind = tkKwProperty then
    begin
      m := parsePropertyDecl;
      if m = nil then
      begin
        failed := true;
        break;
      end;
      hf := 0;
      hm := nil;
      while parseSeparatedHint(hf, hm) do
        ;
      addHints(m, hf, hm);
      m^.vis := vis;
      listAdd(members, m);
      continue;
    end;
    if tok.kind = tkKwConst then
    begin
      listInit(consts);
      parseConstPart(consts, true);
      for i := 0 to consts.count - 1 do
      begin
        m := PDecl(listGet(consts, i));
        if m^.kind = dkVar then
        begin
          diagErrorR(m^.loc, 'typed constants in ' + kindStr +
                     's are not supported yet',
                     identRange(m^.ident));
          continue;
        end;
        m^.vis := vis;
        listAdd(members, m);
      end;
      continue;
    end;
    diagError(tok.loc, 'expected a method, property or constant declaration, or ''end''');
    failed := true;
    break;
  end;
  if failed then
  begin
    skipUntil([tkKwEnd]);
    tryConsume(tkKwEnd);
    exit;
  end;
  if not expect(tkKwEnd, 'at end of ' + kindStr) then
  begin
    diagNote(helperLoc, 'to match this ''helper''');
    exit;
  end;
  ts := newTypeSpec(tsHelper, start, prevTokEnd);
  ts^.helperParent := parentName;
  ts^.helperFor := extended;
  ts^.helperMembers := members;
  ts^.helperIsRecord := isRecord;
  parseTypeHelper := ts;
end;

{ type = simpleType | ["packed"] (arrayType | recordType) . }
function parseTypeImpl: PTypeSpec;
var
  start: TLoc;
  isPacked, isFunction, isSet: boolean;
  elem, resultSpec, ts: PTypeSpec;
  maxLen: PNode;
  params: TPtrList;
  target: TIdent;
  cc: longint;
begin
  parseTypeImpl := nil;
  start := tok.loc;
  isPacked := tryConsume(tkKwPacked);
  { bitpacked array / record (Free Pascal; not reserved) }
  if not isPacked and (mode <> lmISO) and isIdentifierNamed('bitpacked') and
     (peekKind in [tkKwArray, tkKwRecord]) then
  begin
    consumeToken;
    if tok.kind = tkKwArray then
      ts := parseArrayType(start, false)
    else
      ts := parseRecordType(start, false);
    if (ts <> nil) and (ts^.kind = tsArray) then
      ts^.arrBitPacked := true
    else if (ts <> nil) and (ts^.kind = tsRecord) then
      ts^.recBitPacked := true
    else if (ts <> nil) and (ts^.kind = tsHelper) then
      diagError(start, 'a record helper cannot be packed');
    exit(ts);
  end;
  case tok.kind of
    tkKwArray:
      exit(parseArrayType(start, isPacked));
    tkKwRecord:
      exit(parseRecordType(start, isPacked));
    tkKwClass:
      if not isPacked then
        exit(parseClassType(start));
    tkKwInterface: { objfpc: an interface type }
      if not isPacked and (mode = lmObjFPC) then
        exit(parseInterfaceType(start));
    tkKwSet, tkKwFile:
      begin
        isSet := tok.kind = tkKwSet;
        consumeToken;
        { Turbo/FPC: file alone, an untyped file }
        if not isSet and (tok.kind <> tkKwOf) and (mode <> lmISO) then
        begin
          ts := newTypeSpec(tsFile, start, prevTokEnd);
          ts^.elemSpec := nil;
          exit(ts);
        end;
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
        ts^.isLong := (maxLen = nil) and lx.longStrings;
        exit(ts);
      end;
    tkKwProcedure, tkKwFunction:
      { procedural type (Turbo/FPC): procedure(params) / function(params): T }
      if not isPacked and (mode <> lmISO) then
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
        ts^.rtOfObject := false;
        if (mode = lmObjFPC) and (tok.kind = tkKwOf) then { a method pointer }
        begin
          consumeToken;
          if not expect(tkKwObject, 'after ''of''') then
            exit;
          ts^.rtOfObject := true;
        end;
        { A calling convention:  procedure(x: integer) cdecl  or, after a
          semicolon,  procedure(x: integer); cdecl  (Turbo/FPC). }
        cc := ccDefault;
        while true do
        begin
          if callConvOfTok(tok) <> ccDefault then
            takeCallConv(cc)
          else if tok.kind = tkSemi then
          begin
            peekToken;
            if callConvOfTok(peekTok) = ccDefault then
              break;
            consumeToken;
            takeCallConv(cc);
          end
          else
            break;
        end;
        ts^.rtCallConv := cc;
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
  { class is not reserved without classes: TFoo = class ... end }
  if isIdentifierNamed('class') and
     (peekKind in [tkLParen, tkIdentifier, tkKwEnd, tkKwProcedure,
                   tkKwFunction]) then
  begin
    diagErrorR(tok.loc, 'classes are an Object Pascal extension; use ' +
               '--mode=objfpc', tokenRange(tok));
    skipUntil([tkKwEnd]); { skip the class body }
    tryConsume(tkKwEnd);
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
  arr: TToken;
begin
  parseTypeIdentifier := nil;
  if tok.kind = tkKwString then
  begin
    { Turbo/FPC: string names a type here }
    ts := newTypeSpec(tsString, tok.loc, tokenEnd(tok));
    ts^.maxLen := nil;
    ts^.isLong := lx.longStrings;
    consumeToken;
    exit(ts);
  end;
  if tok.kind = tkIdentifier then
    exit(parseNamedType);
  if (tok.kind = tkKwArray) and (context = 'parameter type') and (mode <> lmISO) and
     (peekKind = tkKwOf) then
  begin
    { Turbo/FPC: array of const, array of T }
    arr := tok;
    consumeToken;
    consumeToken;
    if tok.kind = tkKwConst then
    begin
      consumeToken;
      exit(newTypeSpec(tsArrayOfConst, arr.loc, prevTokEnd));
    end;
    ts := newTypeSpec(tsOpenArray, arr.loc, arr.loc);
    ts^.openSpec := parseTypeIdentifier('the elements of an open array');
    if ts^.openSpec = nil then
      exit;
    ts^.e := prevTokEnd;
    exit(ts);
  end;
  if (tok.kind in [tkKwArray, tkKwPacked]) and (context = 'parameter type') then
    unsupported('conformant array parameters are')
  else
    diagError(tok.loc, 'expected type name for ' + context +
                       ' (declare the type in a ''type'' section)');
end;

{ Tokens that can continue a constant expression after an identifier. }
function continuesConstExpr(k: TTokenKind): boolean; forward;

{ A type name, qualified by enclosing classes (objfpc): TOuter.TInner. }
function parseNamedType: PTypeSpec;
var
  name: TIdent;
  start: TLoc;
  quals: TPtrList;
  ts: PTypeSpec;
  first, n, lo, hi: PNode;
begin
  parseNamedType := nil;
  name := makeIdentifier(tok);
  start := consumeToken;
  if (mode = lmISO) or (tok.kind <> tkPeriod) or (peekKind <> tkIdentifier) then
    exit(namedSpec(name));
  { TOuter.TInner (objfpc), or a subrange whose first bound is a qualified
    constant (TColor.Green..TColor.Blue, TFoo.C + 1..9) }
  first := nameRef(name);
  listInit(quals);
  while (tok.kind = tkPeriod) and (peekKind = tkIdentifier) do
  begin
    consumeToken; { '.' }
    listAdd(quals, newIdent(name));
    name := makeIdentifier(tok);
    n := newNode(nkField, start, tokenEnd(tok));
    n^.base := first;
    n^.fieldName := name;
    first := n;
    consumeToken;
  end;
  if continuesConstExpr(tok.kind) then
  begin
    lo := parseSimpleExpressionFrom(first);
    if (lo = nil) or not expect(tkDotdot, 'in subrange type') then
      exit;
    hi := parseSimpleExpression;
    if hi = nil then
      exit;
    ts := newTypeSpec(tsSubrange, start, prevTokEnd);
    ts^.lo := lo;
    ts^.hi := hi;
    exit(ts);
  end;
  ts := newTypeSpec(tsNamed, start, prevTokEnd);
  ts^.tname := name;
  ts^.tquals := quals;
  parseNamedType := ts;
end;

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
  listed, scoped: boolean;
  i: longint;
begin
  parseSimpleType := nil;
  start := tok.loc;
  if tok.kind = tkLParen then
  begin
    scoped := lx.scopedEnums; { $scopedenums on: TColor.Red only }
    consumeToken;
    listInit(names);
    listed := parseIdentList(names, 'enumeration value name');
    if not scoped then
      for i := 0 to names.count - 1 do
        declareName(PIdent(listGet(names, i))^);
    if not listed then
      exit;
    if not expect(tkRParen, 'after enumeration values') then
      exit;
    ts := newTypeSpec(tsEnum, start, prevTokEnd);
    ts^.enumNames := names;
    ts^.enumScoped := scoped;
    exit(ts);
  end;
  if (tok.kind = tkIdentifier) and not continuesConstExpr(peekKind) then
    exit(parseNamedType);
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
  if (tok.kind = tkKwOf) and (mode <> lmISO) then { Turbo/FPC: a dynamic array }
  begin
    consumeToken;
    elem := parseType;
    if elem = nil then
      exit;
    ts := newTypeSpec(tsDynArray, start, prevTokEnd);
    ts^.dynSpec := elem;
    exit(ts);
  end;
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
function parseVariantPart(close: TTokenKind): PVariantPart; forward;

// advancedRecord = "record" { visibility | "var" | identList ":" type ";"
//                   | methodHeading | "class" methodHeading
//                   | "class" operatorHeading | property }
//                  [ variantPart ] "end" .
// (objfpc, $modeswitch advancedrecords). Visibility: private, public,
// strict private. Fields cannot follow a method or property in the same
// section (as in Free Pascal).
function parseAdvancedRecord(start, recordLoc: TLoc; packed_: boolean): PTypeSpec;
var
  hf: longint;
  hm: PName;
  fixed, members, names: TPtrList;
  vp: PVariantPart;
  vis: TVisibility;
  afterMethods, failed, classVars: boolean;
  wd: string;
  ts: PTypeSpec;
  f, m: PDecl;
  i: longint;
  fl: PFieldList;

  procedure addMember(d: PDecl);
  begin
    d^.vis := vis;
    listAdd(members, d);
    afterMethods := true;
    classVars := false;
  end;

begin
  parseAdvancedRecord := nil;
  listInit(fixed);
  listInit(members);
  vp := nil;
  vis := vsPublic;
  afterMethods := false;
  classVars := false;
  failed := false;
  while (tok.kind <> tkKwEnd) and (tok.kind <> tkEof) and not diagShouldStop do
  begin
    if tok.kind = tkIdentifier then
    begin
      wd := nameStr(tok.name);
      if (wd = 'private') or (wd = 'public') then
      begin
        if wd = 'private' then
          vis := vsPrivate
        else
          vis := vsPublic;
        afterMethods := false;
        classVars := false;
        consumeToken;
        continue;
      end;
      if (wd = 'protected') or (wd = 'published') then
      begin
        diagError(tok.loc, '''' + wd + ''' sections are not allowed in records');
        failed := true;
        break;
      end;
      if wd = 'strict' then
      begin
        consumeToken;
        if not isIdentifierNamed('private') then
        begin
          diagError(tok.loc, 'expected ''private'' after ''strict'' (records have ' +
                    'no protected members)');
          failed := true;
          break;
        end;
        consumeToken;
        vis := vsStrictPrivate;
        afterMethods := false;
        classVars := false;
        continue;
      end;
      if classVars then { class var: static storage, not a field }
      begin
        if not parseClassVarNames(members, vis) then
        begin
          failed := true;
          break;
        end;
        continue;
      end;
      if afterMethods then
      begin
        diagError(tok.loc, 'fields cannot follow a method or property; start a ' +
                  'new visibility section or ''var'' first');
        failed := true;
        break;
      end;
      listInit(names);
      if not parseIdentList(names, 'field name') or
         not expect(tkColon, 'after field name') then
      begin
        failed := true;
        break;
      end;
      ts := parseType;
      if ts = nil then
      begin
        failed := true;
        break;
      end;
      parseHints(hf, hm);
      for i := 0 to names.count - 1 do
      begin
        f := newDecl(dkField, PIdent(listGet(names, i))^, PIdent(listGet(names, i))^.loc);
        f^.fieldSpec := ts;
        f^.vis := vis;
        addHints(f, hf, hm);
        listAdd(fixed, f);
      end;
      if not tryConsume(tkSemi) and (tok.kind <> tkKwEnd) then
      begin
        expect(tkSemi, 'after field declaration');
        failed := true;
        break;
      end;
      continue;
    end;
    classVars := false; { (anything else ends a class var section) }
    if tryConsume(tkKwVar) then
    begin
      afterMethods := false;
      continue;
    end;
    if tok.kind = tkKwCase then
    begin
      vp := parseVariantPart(tkKwEnd);
      if vp = nil then
        failed := true
      else
        tryConsume(tkSemi);
      break; { the variant part is last }
    end;
    if tok.kind = tkKwDestructor then
    begin
      diagError(tok.loc, 'a record cannot have a destructor');
      failed := true;
      break;
    end;
    if tok.kind in [tkKwProcedure, tkKwFunction, tkKwConstructor] then
    begin
      m := parseMethodHeadingR(true, false);
      if m = nil then
      begin
        failed := true;
        break;
      end;
      addMember(m);
      continue;
    end;
    if tok.kind = tkKwProperty then
    begin
      m := parsePropertyDecl;
      if m = nil then
      begin
        failed := true;
        break;
      end;
      hf := 0;
      hm := nil;
      while parseSeparatedHint(hf, hm) do
        ;
      addHints(m, hf, hm);
      addMember(m);
      continue;
    end;
    if tok.kind = tkKwClass then
    begin
      consumeToken;
      if isIdentifierNamed('operator') then
      begin
        m := parseOperatorHeading;
        if m = nil then
        begin
          failed := true;
          break;
        end;
        addMember(m);
        continue;
      end;
      if tok.kind in [tkKwProcedure, tkKwFunction] then
      begin
        m := parseMethodHeadingR(true, true);
        if m = nil then
        begin
          failed := true;
          break;
        end;
        m^.isClassMethod := true;
        addMember(m);
        continue;
      end;
      if tok.kind in [tkKwConstructor, tkKwDestructor] then
      begin
        m := parseClassInitHeading;
        if m = nil then
        begin
          failed := true;
          break;
        end;
        addMember(m);
        continue;
      end;
      if tok.kind = tkKwProperty then { class property P: T read ... }
      begin
        m := parsePropertyDecl;
        if m = nil then
        begin
          failed := true;
          break;
        end;
        hf := 0;
        hm := nil;
        while parseSeparatedHint(hf, hm) do
          ;
        addHints(m, hf, hm);
        m^.propIsClass := true;
        addMember(m);
        continue;
      end;
      if tryConsume(tkKwVar) then { class variables follow }
      begin
        classVars := true;
        afterMethods := false;
        continue;
      end;
      diagError(tok.loc, 'expected ''procedure'', ''function'', ''constructor'', ' +
                '''destructor'', ''property'', ''operator'' or ''var'' after ''class''');
      failed := true;
      break;
    end;
    if tok.kind in [tkKwType, tkKwConst] then { nested declarations }
    begin
      listInit(names);
      if tok.kind = tkKwConst then
        parseConstPart(names, true)
      else
        parseTypePart(names, true);
      for i := 0 to names.count - 1 do
      begin
        m := PDecl(listGet(names, i));
        if m^.kind = dkVar then
        begin
          diagErrorR(m^.loc, 'typed constants in records are not supported yet',
                     identRange(m^.ident));
          continue;
        end;
        addMember(m);
      end;
      continue;
    end;
    diagError(tok.loc,
              'expected a field, method or property declaration, or ''end''');
    failed := true;
    break;
  end;
  if failed then
  begin
    skipUntil([tkKwEnd]);
    tryConsume(tkKwEnd);
    exit;
  end;
  if not expect(tkKwEnd, 'at end of record type') then
  begin
    diagNote(recordLoc, 'to match this ''record''');
    exit;
  end;
  fl := PFieldList(arenaAlloc(sizeof(TFieldList)));
  fl^.fixed := fixed;
  fl^.variant := vp;
  ts := newTypeSpec(tsRecord, start, prevTokEnd);
  ts^.fields := fl;
  ts^.recPacked := packed_;
  ts^.recMembers := members;
  parseAdvancedRecord := ts;
end;

{ recordType = "record" fieldList "end" . }
function parseRecordTypeBody(start: TLoc; packed_: boolean): PTypeSpec;
var
  recordLoc: TLoc;
  fields: PFieldList;
  ts: PTypeSpec;
begin
  parseRecordTypeBody := nil;
  recordLoc := consumeToken; { 'record' }
  { record helper for T (objfpc): `helper` followed by `for` or `(` cannot
    begin a field }
  if isIdentifierNamed('helper') and (peekKind in [tkKwFor, tkLParen]) then
  begin
    if packed_ then
      diagError(start, 'a record helper cannot be packed');
    exit(parseTypeHelper(recordLoc, true));
  end;
  if (mode = lmObjFPC) and lx.advancedRecords then
    exit(parseAdvancedRecord(start, recordLoc, packed_));
  fields := parseFieldList(tkKwEnd);
  if fields = nil then
  begin
    if recordBodySkipped then { (methods without the mode switch) }
      tryConsume(tkKwEnd);
    recordBodySkipped := false;
    exit;
  end;
  if not expect(tkKwEnd, 'at end of record type') then
  begin
    diagNote(recordLoc, 'to match this ''record''');
    exit;
  end;
  ts := newTypeSpec(tsRecord, start, prevTokEnd);
  ts^.fields := fields;
  ts^.recPacked := packed_;
  parseRecordTypeBody := ts;
end;

function parseRecordType(start: TLoc; packed_: boolean): PTypeSpec;
var
  pack: longint;
  ts: PTypeSpec;
begin
  pack := lx.packRecords; { $packrecords N here }
  ts := parseRecordTypeBody(start, packed_);
  if (ts <> nil) and (ts^.kind = tsRecord) then
    ts^.recPackRecords := pack;
  parseRecordType := ts;
end;

// fieldList = [ identList ":" type { ";" identList ":" type } ]
// [ ";" variantPart ] [ ";" ] .
function parseFieldList(close: TTokenKind): PFieldList;
var
  hf: longint;
  hm: PName;
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
    parseHints(hf, hm);
    for i := 0 to names.count - 1 do
    begin
      f := newDecl(dkField, PIdent(listGet(names, i))^, PIdent(listGet(names, i))^.loc);
      f^.fieldSpec := ts;
      addHints(f, hf, hm);
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
    if (mode = lmObjFPC) and (close = tkKwEnd) and
       (tok.kind in [tkKwProcedure, tkKwFunction, tkKwConstructor,
                     tkKwDestructor, tkKwProperty, tkKwClass]) then
    begin
      diagError(tok.loc, 'methods and properties in records need ' +
                '{$modeswitch advancedrecords}');
      skipUntil([tkKwEnd]); { the record's end: continue after it }
      recordBodySkipped := true;
      exit;
    end;
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

{ Parses statements separated by ';' up to (not including) terminator or
  alt (tkEof: none; the body of a try statement ends at except or
  finally). }
procedure parseStatementSequenceAlt(var body: TPtrList;
                                    terminator, alt: TTokenKind; openLoc: TLoc;
                                    const openWord: string);
var
  semiLoc: TLoc;
  expected: string;
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
    if (tok.kind = terminator) or ((alt <> tkEof) and (tok.kind = alt)) then
      exit;
    { 'end', or 'except' or 'finally' }
    expected := describe(terminator);
    if alt <> tkEof then
      expected := expected + ' or ' + describe(alt);
    if (tok.kind = tkEof) or ((alt <> tkEof) and (tok.kind = tkKwEnd)) then
    begin
      diagError(tok.loc, 'expected ' + expected);
      diagNote(openLoc, 'to match this ''' + openWord + '''');
      exit;
    end;
    if startsStatement then
    begin
      { most likely a forgotten ';': report it and carry on }
      diagError(prevTokEnd, 'expected '';'' between statements');
      continue;
    end;
    if alt <> tkEof then
      diagError(tok.loc, 'expected '';'', ' + expected + ' after statement')
    else
      diagError(tok.loc, 'expected '';'' or ' + expected + ' after statement');
    skipUntil([tkSemi, terminator, alt]);
    if tryConsume(tkSemi) then
      continue;
    if (tok.kind <> terminator) and ((alt = tkEof) or (tok.kind <> alt)) then
    begin
      diagNote(openLoc, 'to match this ''' + openWord + '''');
      exit;
    end;
  end;
end;

procedure parseStatementSequence(var body: TPtrList; terminator: TTokenKind;
                                 openLoc: TLoc; const openWord: string);
begin
  parseStatementSequenceAlt(body, terminator, tkEof, openLoc, openWord);
end;

function parseAssignmentOrCall: PNode; forward;
function makeBinary(op: TBinaryOp; opLoc: TLoc; l, r: PNode): PNode; forward;
function finishDesignatorStmt(start: TLoc; target: PNode): PNode; forward;
function parseInherited: PNode; forward;
function parseFactor: PNode; forward;
function parseIfStmt: PNode; forward;
function parseWhileStmt: PNode; forward;
function parseRepeatStmt: PNode; forward;
function parseForStmt: PNode; forward;
function parseCaseStmt: PNode; forward;
function parseWithStmt: PNode; forward;
function parseTryStmt: PNode; forward;
function parseRaiseStmt: PNode; forward;

function parseStatementImpl: PNode;
var
  gotoLoc, start, l: TLoc;
  lbl: TIdent;
  n, body: PNode;
  savedStmtToks: PPtrList;
  toks: TPtrList;
begin
  { a statement starting with a designator: its tokens are recorded }
  savedStmtToks := stmtTokens;
  listInit(toks);
  stmtTokens := @toks;
  n := nil;
  case tok.kind of
    tkIdentifier: n := parseAssignmentOrCall;
    tkLParen: { Turbo/FPC: (p + 1)^ := x; (obj as TFoo).Show; }
      if mode <> lmISO then
      begin
        start := tok.loc;
        n := parseFactor;
        if n = nil then
        begin
          skipUntil([tkSemi, tkKwEnd]);
          n := emptyStmt(start);
        end
        else
          n := finishDesignatorStmt(start, n);
      end;
    tkKwInherited:
      begin
        start := tok.loc;
        n := parseInherited;
        if n = nil then
        begin
          skipUntil([tkSemi, tkKwEnd]);
          n := emptyStmt(start);
        end
        else
          n := finishDesignatorStmt(start, n);
      end;
  else
    ;
  end;
  stmtTokens := savedStmtToks;
  if n <> nil then
    exit(n);
  case tok.kind of
    tkKwBegin: exit(parseCompoundStmt);
    tkKwIf: exit(parseIfStmt);
    tkKwWhile: exit(parseWhileStmt);
    tkKwRepeat: exit(parseRepeatStmt);
    tkKwFor: exit(parseForStmt);
    tkKwCase: exit(parseCaseStmt);
    tkKwWith: exit(parseWithStmt);
    tkKwTry: exit(parseTryStmt);
    tkKwRaise: exit(parseRaiseStmt);
    tkLParen: { not objfpc }
      diagError(tok.loc, 'expected statement');
    { the empty statement: nothing before a separator or terminator }
    tkSemi, tkKwEnd, tkKwElse, tkKwUntil, tkKwExcept, tkKwFinally, tkEof:
      exit(emptyStmt(tok.loc));
    tkKwGoto:
      begin
        if mode = lmObjFPC then
          diagNoGoto;
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

function isAssignOp: boolean;
begin
  isAssignOp := tok.kind in [tkColonEqual, tkEqual, tkPlusEqual, tkMinusEqual,
                             tkStarEqual, tkSlashEqual];
end;

{ The designator of toks (of PToken) parsed again (as a factor). }
function reparseDesignator(const toks: TPtrList): PNode;
var
  g: TGenericTokens;
  savedTok, savedPeek: TToken;
  savedHasPeek: boolean;
  savedPrev: TLoc;
  savedReplay: pointer;
  savedPos: longint;
  savedRec: PPtrList;
begin
  fillchar(g, sizeof(g), 0);
  g.toks := toks;
  savedTok := tok;
  savedPeek := peekTok;
  savedHasPeek := hasPeek;
  savedPrev := prevTokEnd;
  savedReplay := replay;
  savedPos := replayPos;
  savedRec := recording;
  recording := nil; { recorded already }
  replay := @g;
  replayPos := 0;
  hasPeek := false;
  nextToken(tok);
  reparseDesignator := parseFactor;
  tok := savedTok;
  peekTok := savedPeek;
  hasPeek := savedHasPeek;
  prevTokEnd := savedPrev;
  replay := savedReplay;
  replayPos := savedPos;
  recording := savedRec;
end;

{ target := value, or target op= value (Free Pascal's C operators: target
  := target op (value), target parsed again from stmtTokens) }
function parseAssignment(start: TLoc; target: PNode): PNode;
var
  targetToks: TPtrList;
  k: TTokenKind;
  compound: boolean;
  assignLoc: TLoc;
  value, copy, n: PNode;
  op: TBinaryOp;
begin
  { the designator's tokens (recorded up to the operator) }
  listInit(targetToks);
  if stmtTokens <> nil then
    targetToks := stmtTokens^;
  stmtTokens := nil; { the rest is not needed }
  k := tok.kind;
  compound := (k <> tkColonEqual) and (k <> tkEqual);
  assignLoc := consumeToken;
  value := parseExpression;
  if value = nil then
  begin
    skipUntil([tkSemi, tkKwEnd]);
    exit(emptyStmt(start));
  end;
  if compound then
  begin
    { x += e is x := x + (e), x read again (as in Free Pascal, whose copy
      of x evaluates its indexes and calls again) }
    copy := reparseDesignator(targetToks);
    if copy = nil then
      exit(emptyStmt(start));
    case k of
      tkPlusEqual: op := boAdd;
      tkMinusEqual: op := boSub;
      tkStarEqual: op := boMul;
    else
      op := boRealDiv;
    end;
    value := makeBinary(op, assignLoc, copy, value);
  end;
  n := newNode(nkAssign, start, prevTokEnd);
  n^.target := target;
  n^.value := value;
  n^.assignLoc := assignLoc;
  n^.assignCompound := compound;
  parseAssignment := n;
end;

// assignment = designator ":=" expression .
// procCall   = ident [ "(" actualParam { "," actualParam } ")" ] .
function parseAssignmentOrCall: PNode;
var
  name: TIdent;
  start: TLoc;
  target, call, n: PNode;
  hasParens: boolean;
  args, typeArgs: TPtrList;
begin
  if startsSpecialize then { specialize P<T>(args); }
  begin
    start := tok.loc;
    listInit(typeArgs);
    listInit(args);
    hasParens := false;
    if parseSpecialization(name, typeArgs) then
    begin
      hasParens := tok.kind = tkLParen;
      if hasParens then
        parseActualParams(args, false);
    end
    else
    begin
      skipUntil([tkSemi, tkKwEnd]);
      exit(emptyStmt(start));
    end;
    n := newNode(nkProcCall, start, prevTokEnd);
    n^.callee := name;
    n^.args := args;
    n^.hasParens := hasParens;
    n^.typeArgs := typeArgs;
    exit(n);
  end;
  name := makeIdentifier(tok);
  start := consumeToken;

  target := nameRef(name);
  if tok.kind in [tkLSquare, tkPeriod, tkCaret] then
  begin
    target := parseSelectors(target);
    if target = nil then
    begin
      skipUntil([tkSemi, tkKwEnd]);
      exit(emptyStmt(start));
    end;
    exit(finishDesignatorStmt(start, target));
  end;

  { common slip: '=' for ':='; report it, then parse an assignment anyway }
  if tok.kind = tkEqual then
    diagErrorR(tok.loc, 'expected '':='' in assignment, found ''=''',
               tokenRange(tok));

  if isAssignOp then
    exit(parseAssignment(start, target));

  listInit(args);
  hasParens := tok.kind = tkLParen;
  if hasParens then
    parseActualParams(args, true);

  { Turbo/FPC: a typecast assigned to, longint(x) := 1, or with selectors,
    TFoo(x).Show; f(1)^.n := 2; }
  if hasParens and (mode <> lmISO) and
     (tok.kind in [tkLSquare, tkPeriod, tkCaret, tkColonEqual, tkLParen,
                   tkPlusEqual, tkMinusEqual, tkStarEqual, tkSlashEqual]) then
  begin
    call := newNode(nkCall, start, prevTokEnd);
    call^.callee := name;
    call^.args := args;
    target := parseSelectors(call);
    if target = nil then
    begin
      skipUntil([tkSemi, tkKwEnd]);
      exit(emptyStmt(start));
    end;
    exit(finishDesignatorStmt(start, target));
  end;

  n := newNode(nkProcCall, start, prevTokEnd);
  n^.callee := name;
  n^.args := args;
  n^.hasParens := hasParens;
  parseAssignmentOrCall := n;
end;

{ The rest of a statement that starts with a designator with selectors: an
  assignment, or (Turbo/FPC) a call through a procedural value or a method
  call: r.p(x); a[i]; obj.Show; inherited Create(x); }
function finishDesignatorStmt(start: TLoc; target: PNode): PNode;
var
  call, n: PNode;
  none: TPtrList;
begin
  if tok.kind = tkEqual then
    diagErrorR(tok.loc, 'expected '':='' in assignment, found ''=''',
               tokenRange(tok));

  if isAssignOp then
    exit(parseAssignment(start, target));

  if (mode <> lmISO) and
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

  diagError(prevTokEnd, 'expected '':='' after variable');
  skipUntil([tkSemi, tkKwEnd]);
  finishDesignatorStmt := emptyStmt(start);
end;

// inherited [ ident ] { selector }   (objfpc)
function parseInherited: PNode;
var
  loc: TLoc;
  name: TIdent;
  n: PNode;
begin
  loc := consumeToken; { 'inherited' }
  name := noIdent;
  if tok.kind = tkIdentifier then
  begin
    name := makeIdentifier(tok);
    consumeToken;
  end;
  n := newNode(nkInherited, loc, prevTokEnd);
  n^.inhName := name;
  parseInherited := parseSelectors(n);
end;

function parseActualParams(var args: TPtrList; allowFormat: boolean): boolean;
var
  lparen: TLoc;
  e, width, precision, f: PNode;
  ok: boolean;
begin
  lparen := consumeToken; { '(' }
  if (tok.kind = tkRParen) and (mode = lmObjFPC) then { objfpc: f() }
  begin
    consumeToken;
    exit(true);
  end;
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
  forLoc, inLoc: TLoc;
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

  if tok.kind = tkKwIn then { for x in collection do (Turbo/FPC) }
  begin
    inLoc := consumeToken;
    if mode = lmISO then
      diagError(inLoc, '''for ... in'' is a Turbo/Free Pascal extension; use ' +
                       '--mode=tp or --mode=objfpc');
    init := parseExpression;
    if init = nil then
      exit(fail);
    if not expect(tkKwDo, 'in ''for'' statement') and not startsStatement then
      exit(fail);
    body := parseStatement;
    n := newNode(nkForIn, forLoc, prevTokEnd);
    n^.inVar := v;
    n^.inLoc := inLoc;
    n^.inColl := init;
    n^.inBody := body;
    n^.inKind := fikInvalid;
    exit(n);
  end;

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
        n^.intBased := t.based;
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
      n^.strWide := t.wideChar;
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
  operand, e, n: PNode;
  isName: boolean;
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
  isName := tok.kind = tkIdentifier;
  consumeToken;
  { TOuter.C (objfpc): a constant declared in a class or record; TColor.Red }
  while isName and (mode <> lmISO) and (tok.kind = tkPeriod) and
        (peekKind = tkIdentifier) do
  begin
    consumeToken; { '.' }
    n := newNode(nkField, e^.b, tokenEnd(tok));
    n^.base := e;
    n^.fieldName := makeIdentifier(tok);
    e := n;
    consumeToken;
  end;
  parseConstant := e;
end;

// with = "with" designator { "," designator } "do" statement .
function parseDesignator: PNode; forward;

{ on starts an exception handler at the start of an except part and after
  a handler (it is not reserved). }
function isOnHandler: boolean;
begin
  isOnHandler := isIdentifierNamed('on');
end;

{ tryStmt = "try" stmtSeq ( "except" exceptPart | "finally" stmtSeq ) "end" .
  exceptPart = handler [ ";" handler ]... [ ";" ] [ "else" stmtSeq ] | stmtSeq .
  handler = "on" [ ident ":" ] ident "do" statement .          (objfpc) }
function parseTryStmt: PNode;
var
  tryLoc, partLoc, elseLoc, onLoc, endLoc: TLoc;
  body, handlers, part: TPtrList;
  isFin, ok, stop: boolean;
  varName, className: TIdent;
  hbody, n: PNode;
  h: PExceptHandler;
begin
  tryLoc := consumeToken;
  listInit(body);
  parseStatementSequenceAlt(body, tkKwExcept, tkKwFinally, tryLoc, 'try');
  if (tok.kind <> tkKwExcept) and (tok.kind <> tkKwFinally) then
  begin
    skipUntil([tkKwEnd]);
    tryConsume(tkKwEnd);
    exit(emptyStmt(tryLoc));
  end;
  isFin := tok.kind = tkKwFinally;
  partLoc := consumeToken;
  listInit(handlers);
  listInit(part);
  elseLoc := noLoc;
  ok := true;
  if not isFin and isOnHandler then
  begin
    stop := false;
    while not stop and not diagShouldStop and isOnHandler do
    begin
      onLoc := consumeToken;
      varName := noIdent;
      if not expectIdentifier(className, 'exception class name after ''on''') then
      begin
        ok := false;
        skipUntil([tkSemi, tkKwEnd]);
        if not tryConsume(tkSemi) then
          stop := true;
        continue;
      end;
      if tryConsume(tkColon) then
      begin
        varName := className;
        if not expectIdentifier(className, 'exception class name after '':''') then
        begin
          ok := false;
          skipUntil([tkSemi, tkKwEnd]);
          if not tryConsume(tkSemi) then
            stop := true;
          continue;
        end;
      end;
      if not expect(tkKwDo, 'after the exception class') then
      begin
        ok := false;
        skipUntil([tkSemi, tkKwEnd]);
        if not tryConsume(tkSemi) then
          stop := true;
        continue;
      end;
      hbody := parseStatement;
      h := PExceptHandler(arenaAlloc(sizeof(TExceptHandler)));
      h^.onLoc := onLoc;
      h^.hVarName := varName;
      h^.hClassName := className;
      h^.hBody := hbody;
      h^.hVar := nil;
      h^.hClass := nil;
      listAdd(handlers, h);
      if not tryConsume(tkSemi) then
        stop := true;
    end;
    if tok.kind = tkKwElse then
    begin
      elseLoc := consumeToken;
      parseStatementSequence(part, tkKwEnd, tryLoc, 'try');
    end
    else if (tok.kind <> tkKwEnd) and ok then
    begin
      diagError(tok.loc, 'expected ''on'', ''else'' or ''end'' in the ''except'' part');
      ok := false;
      skipUntil([tkKwEnd]);
    end;
  end
  else
    parseStatementSequence(part, tkKwEnd, tryLoc, 'try');
  endLoc := tokenEnd(tok);
  if not tryConsume(tkKwEnd) then
  begin
    if tok.kind <> tkEof then { at the end of file, the sequence reported it }
      diagError(tok.loc, 'expected ''end'' at end of ''try'' statement');
    diagNote(tryLoc, 'to match this ''try''');
    exit(emptyStmt(tryLoc));
  end;
  if not ok then
    exit(emptyStmt(tryLoc));
  n := newNode(nkTry, tryLoc, endLoc);
  n^.tryBody := body;
  n^.isFinally := isFin;
  n^.partLoc := partLoc;
  n^.handlers := handlers;
  n^.tryElseLoc := elseLoc;
  n^.partBody := part;
  parseTryStmt := n;
end;

{ raiseStmt = "raise" [ expression [ "at" expression [ "," expression ] ] ] .
                                                              (objfpc)
  (at, the exception's address and frame, is not a reserved word.) }
function parseRaiseStmt: PNode;
var
  raiseLoc: TLoc;
  obj, addr, frame, n: PNode;
begin
  raiseLoc := consumeToken;
  obj := nil;
  addr := nil;
  frame := nil;
  if not (tok.kind in [tkSemi, tkKwEnd, tkKwElse, tkKwUntil, tkKwExcept,
                       tkKwFinally, tkEof]) then
  begin
    obj := parseExpression;
    if (obj <> nil) and isIdentifierNamed('at') then
    begin
      consumeToken;
      addr := parseExpression;
      if (addr <> nil) and (tok.kind = tkComma) then
      begin
        consumeToken;
        frame := parseExpression;
        if frame = nil then
          addr := nil;
      end;
      if addr = nil then
        obj := nil;
    end;
    if obj = nil then
    begin
      skipUntil([tkSemi, tkKwEnd]);
      exit(emptyStmt(raiseLoc));
    end;
  end;
  n := newNode(nkRaise, raiseLoc, prevTokEnd);
  n^.raiseObj := obj;
  n^.raiseAddr := addr;
  n^.raiseFrame := frame;
  parseRaiseStmt := n;
end;

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
    tkKwIs: op := boIs;
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
    tkKwAs: op := boAs;
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

function parseTermFrom(first: PNode): PNode; forward;

function parseTerm: PNode;
begin
  parseTerm := parseTermFrom(nil);
end;

// simpleExpr = [ "+" | "-" ] term { addOp term } .
function parseSimpleExpressionFrom(first: PNode): PNode;
var
  lhs, rhs, operand: PNode;
  uop: TUnaryOp;
  op: TBinaryOp;
  opLoc: TLoc;
begin
  parseSimpleExpressionFrom := nil;
  if first <> nil then
    lhs := parseTermFrom(first)
  else if tok.kind in [tkPlus, tkMinus] then
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
  parseSimpleExpressionFrom := lhs;
end;

function parseSimpleExpression: PNode;
begin
  parseSimpleExpression := parseSimpleExpressionFrom(nil);
end;

// term = factor { mulOp factor } .
function parseTermFrom(first: PNode): PNode;
var
  lhs, rhs: PNode;
  op: TBinaryOp;
  opLoc: TLoc;
begin
  parseTermFrom := nil;
  if first <> nil then
    lhs := first
  else
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
  parseTermFrom := lhs;
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
    if (tok.kind = tkLParen) and (mode <> lmISO) and (base^.kind <> nkNameRef) then
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
  { string(x): a typecast to the type string now means }
  if (tok.kind = tkKwString) and (mode <> lmISO) and (peekKind = tkLParen) then
  begin
    if lx.longStrings then
      name.name := intern('ansistring')
    else
      name.name := intern('shortstring');
    name.spelling := tokenSpelling(tok);
    name.loc := tok.loc;
    consumeToken;
    listInit(args);
    if not parseActualParams(args, false) and (args.count = 0) then
      exit;
    call := newNode(nkCall, name.loc, prevTokEnd);
    call^.callee := name;
    call^.args := args;
    exit(parseSelectors(call));
  end;
  case tok.kind of
    tkIntegerLiteral, tkRealLiteral, tkStringLiteral:
      begin
        n := literalNode(tok);
        consumeToken;
        if (n^.kind = nkStringLit) and lx.typeHelpers and (tok.kind = tkPeriod) then
          exit(parseSelectors(n)); { 'abc'.ToUpper }
        exit(n);
      end;
    tkIdentifier:
      begin
        if startsSpecialize then { specialize F<T>(args) }
        begin
          lparen := tok.loc;
          listInit(elems);
          if not parseSpecialization(name, elems) then
            exit;
          listInit(args);
          if (tok.kind = tkLParen) and not parseActualParams(args, false) and
             (args.count = 0) then
            exit;
          call := newNode(nkCall, lparen, prevTokEnd);
          call^.callee := name;
          call^.args := args;
          call^.typeArgs := elems;
          exit(parseSelectors(call));
        end;
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
          if mode <> lmISO then
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
        { Turbo/FPC: (p + 1)^, (obj as TFoo).Show }
        if (mode <> lmISO) and (tok.kind in [tkPeriod, tkCaret, tkLSquare]) then
          exit(parseSelectors(n));
        exit(n);
      end;
    tkKwInherited:
      exit(parseInherited);
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
