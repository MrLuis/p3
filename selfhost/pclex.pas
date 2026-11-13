unit pclex;
{ The lexer of the self-hosted compiler; a port of src/lexer/Lexer.cpp.
  Token kinds come from include/pcc/Lexer/TokenKinds.def through the
  generated tokens.inc.

  ISO 7185 tokens plus, with --mode=tp, Turbo/FPC ones: // comments,
  $hex %bin &oct numbers, #13 character codes glued to strings, '@'.
  Comments starting with '$' are directives: R+/R- and Q+/Q- switch
  runtime checks (recorded for code generation), I file includes a file.
  Conditional compilation (DEFINE, UNDEF, IFDEF, IFNDEF, IF, ELSEIF, ELSE,
  ENDIF, IFEND) is done here: the text of a branch that is not compiled is
  skipped, and only its comments, strings and conditional directives are
  recognized. }

interface

uses pcsys, pcutil, pcdiag;

{$I tokens.inc}

type
  TLangMode = (lmISO, lmTP, lmObjFPC);

  TToken = record
    kind: TTokenKind;
    loc: TLoc;
    len: longint;      { bytes of source text }
    name: PName;       { identifiers: the lower-case name }
    strValue: PName;   { string literals: the value, quotes removed }
    intValue: int64;   { integer literals: 64 bits, read as unsigned }
    based: boolean;    { integer literals written $FF, %1010 or &17 }
    wideChar: longint; { string literals: a lone code above 255, or 0 }
    realValue: double; { real literals }
  end;

  TCheckSwitch = (swRange, swOverflow);

  PIncludeFrame = ^TIncludeFrame;
  TIncludeFrame = record
    fid: longint;
    cur: int64;
  end;

  { a macro's value: [b, e) of buffer fid (in the define directive) }
  PMacro = ^TMacro;
  TMacro = record
    fid: longint;
    b, e: int64;
  end;

  { where to continue after a macro's value: buffer, offset, end }
  PMacroFrame = ^TMacroFrame;
  TMacroFrame = record
    fid: longint;
    cur, len: int64;
  end;

  TLexer = record
    mode: TLangMode;
    fid: longint;       { the buffer being read }
    data: PCharArray;
    len: int64;
    cur: int64;         { offset of the next character }
    includes: TPtrList; { of PIncludeFrame: the including buffers }
    hasPendingInclude: boolean;
    pendingInclude: TBuf; { the file named by the last include directive }
    pendingIncludeLoc: TLoc;
    { conditional compilation: the defined symbols (upper-case name id ->
      non-nil) and the open conditionals (of PConditional), innermost last }
    defines: TIntMap;
    conds: TPtrList;
    corbaInterfaces: boolean; { $interfaces corba (default: com) }
    advancedRecords: boolean; { $modeswitch advancedrecords }
    typeHelpers: boolean; { $modeswitch typehelpers }
    longStrings: boolean; { H+: string is an ansistring }
    { macros ($macro on, $define NAME := value, as in Free Pascal): upper-
      case name id -> PMacro; a use of NAME lexes its value (macroStack:
      the places to continue at, of PMacroFrame, innermost last); expanded
      tokens have the value's locations }
    macrosOn: boolean;
    inlineOn: boolean; { $inline on (the default in objfpc mode) }
    { $coperators on: += -= *= /= are tokens (the default except in ISO
      mode, as in Free Pascal, where it is on in every mode) }
    cOperators: boolean;
    { mode switches that pcc lets a program turn on or off in any mode
      ($modeswitch result|out|defaultparameters); on in objfpc mode }
    resultSwitch, outSwitch, defaultParamsSwitch: boolean;
    { $scopedenums on: the values of the enumerations declared after it are
      reached only through their type (TColor.Red) }
    scopedEnums: boolean;
    { $packrecords N / $align N / $aN: the largest alignment of the fields
      of the records declared after it (1, 2, 4, 8, 16 or 32); 0: the
      natural (C) layout }
    packRecords: longint;
    { $push: the local switches saved, of PSavedSwitches, innermost last
      ($pop restores them), as in Free Pascal (mode switches and $macro
      are not local) }
    switchStack: TPtrList;
    macros: TIntMap;
    macroStack: TPtrList;
    { declared(NAME) in $IF (Free Pascal): the names declared so far (name
      id -> non-nil), which the parser adds, innermost scope last (a
      routine's own); of PIntMap }
    nameScopes: TPtrList;
  end;

  PIntMap = ^TIntMap;
  { called before declared(NAME) is evaluated: the parser declares the used
    units' names (loading them when first needed) }
  TBeforeDeclared = procedure(var lx: TLexer);

var
  beforeDeclared: TBeforeDeclared;
  { -I directories, searched for included files after the including
    file's directory. }
  includeDirs: TPtrList; { of PName }
  { -d symbols, defined in every file }
  commandLineDefines: TPtrList; { of PName }

procedure lexInit(var lx: TLexer; fid: longint; mode: TLangMode);
procedure lexNext(var lx: TLexer; var t: TToken);

{ declared(NAME): name is declared from here on, in the current scope or
  (global) the outermost one; a routine's in a scope of its own. }
procedure lexDeclareName(var lx: TLexer; name: PName; global: boolean);
procedure lexPushNames(var lx: TLexer);
procedure lexPopNames(var lx: TLexer);
{ the predeclared names (of PName, lower case), which declared(NAME) sees }
procedure lexSetPredeclared(const names: TPtrList);
function lexIsDeclared(var lx: TLexer; name: PName): boolean;

{ The text of t as written. }
function tokenText(const t: TToken): string;
procedure bufAddTokenText(var b: TBuf; const t: TToken);
function tokenEnd(const t: TToken): TLoc;
function tokenRange(const t: TToken): TRange;

{ Is switch sw on at loc? Directives seen so far in loc's file before loc
  decide; otherwise the default from the command line. }
function checkEnabled(sw: TCheckSwitch; loc: TLoc; default: boolean): boolean;

implementation

type
  PKeyword = ^TKeywordInfo;

  PDirective = ^TDirective;
  { state: 0 off, 1 on, -1 the default (the command line's; $pop of a
    $push before any switch) }
  TDirective = record
    loc: TLoc;
    fid: longint;
    sw: TCheckSwitch;
    state: longint;
  end;

  PSavedSwitches = ^TSavedSwitches;
  TSavedSwitches = record
    longStrings, corbaInterfaces, inlineOn, scopedEnums: boolean;
    packRecords: longint;
    range, overflow: longint; { as TDirective.state }
  end;

var
  keywordMap: TIntMap; { name id -> PKeyword }
  keywordsReady: boolean;
  directives: TPtrList; { of PDirective, in source order }

type
  PConditional = ^TConditional;
  TConditional = record
    loc: TLoc;            { the opening directive }
    name: string[8];      { its directive, e.g. '$IFDEF' }
    parentActive: boolean; { the text around the conditional is compiled }
    active: boolean;      { the current branch is compiled }
    taken: boolean;       { some branch has been compiled }
    seenElse: boolean;
    depth: longint;       { include depth of the opening directive }
  end;

procedure initKeywords;
var
  i: longint;
  k: PKeyword;
begin
  mapInit(keywordMap);
  for i := 1 to numKeywords do
  begin
    k := PKeyword(arenaAlloc(sizeof(TKeywordInfo)));
    k^ := keywords[i];
    mapPut(keywordMap, intern(k^.spelling)^.id, k);
  end;
  keywordsReady := true;
end;

{ --- Character classes (ASCII, like llvm::isAlpha etc.) --- }

function isAlpha(c: char): boolean;
begin
  isAlpha := ((c >= 'a') and (c <= 'z')) or ((c >= 'A') and (c <= 'Z'));
end;

function isDigit(c: char): boolean;
begin
  isDigit := (c >= '0') and (c <= '9');
end;

function isAlnum(c: char): boolean;
begin
  isAlnum := isAlpha(c) or isDigit(c);
end;

function isHexDigit(c: char): boolean;
begin
  isHexDigit := isDigit(c) or ((c >= 'a') and (c <= 'f')) or
                ((c >= 'A') and (c <= 'F'));
end;

function isIdentStart(c: char): boolean;
begin
  isIdentStart := isAlpha(c) or (c = '_');
end;

function isIdentBody(c: char): boolean;
begin
  isIdentBody := isAlnum(c) or (c = '_');
end;

{ --- Buffer access --- }

{ The character at offset i of the current buffer; #0 at and after its
  end (buffers are NUL-terminated, so peeking one past is safe). }
function at(const lx: TLexer; i: int64): char;
begin
  if i >= lx.len then
    at := #0
  else
  begin
    {$R-}
    at := lx.data^[i];
    {$R+}
  end;
end;

{ The characters [b, e) of the current buffer (at most 255). }
function copyChars(const lx: TLexer; b, e: int64): string;
var
  s: string;
begin
  s := '';
  while (b < e) and (length(s) < 255) do
  begin
    s := s + at(lx, b);
    b := b + 1;
  end;
  copyChars := s;
end;

function locOf(const lx: TLexer; offset: int64): TLoc;
begin
  locOf := smFileStart(lx.fid) + TLoc(offset);
end;

procedure setBuffer(var lx: TLexer; fid: longint; cur: int64);
begin
  lx.fid := fid;
  lx.data := smFileData(fid);
  lx.len := smFileLen(fid);
  lx.cur := cur;
end;

function upperName(const s: string): PName;
var
  i: longint;
  u: string;
begin
  u := s;
  for i := 1 to length(u) do
    if (u[i] >= 'a') and (u[i] <= 'z') then
      u[i] := chr(ord(u[i]) - 32);
  upperName := intern(u);
end;

procedure defineSymbol(var lx: TLexer; const s: string; isOn: boolean);
begin
  if isOn then
    mapPut(lx.defines, upperName(s)^.id, upperName(s))
  else
    mapPut(lx.defines, upperName(s)^.id, nil);
end;

function isDefined(const lx: TLexer; const s: string): boolean;
begin
  isDefined := mapGet(lx.defines, upperName(s)^.id) <> nil;
end;

var
  predeclared: TIntMap;
  hasPredeclared: boolean;

procedure lexPushNames(var lx: TLexer);
var
  m: PIntMap;
begin
  m := PIntMap(arenaAlloc(sizeof(TIntMap)));
  mapInit(m^);
  listAdd(lx.nameScopes, m);
end;

procedure lexPopNames(var lx: TLexer);
var
  ignored: pointer;
begin
  if lx.nameScopes.count > 1 then
    ignored := listPop(lx.nameScopes);
end;

procedure lexDeclareName(var lx: TLexer; name: PName; global: boolean);
begin
  if global then
    mapPut(PIntMap(listGet(lx.nameScopes, 0))^, name^.id, name)
  else
    mapPut(PIntMap(listGet(lx.nameScopes, lx.nameScopes.count - 1))^, name^.id, name);
end;

procedure lexSetPredeclared(const names: TPtrList);
var
  i: longint;
begin
  mapInit(predeclared);
  for i := 0 to names.count - 1 do
    mapPut(predeclared, PName(listGet(names, i))^.id, listGet(names, i));
  hasPredeclared := true;
end;

function lexIsDeclared(var lx: TLexer; name: PName): boolean;
var
  i: longint;
begin
  if assigned(beforeDeclared) then
    beforeDeclared(lx);
  for i := 0 to lx.nameScopes.count - 1 do
    if mapGet(PIntMap(listGet(lx.nameScopes, i))^, name^.id) <> nil then
      exit(true);
  lexIsDeclared := hasPredeclared and (mapGet(predeclared, name^.id) <> nil);
end;

procedure lexInit(var lx: TLexer; fid: longint; mode: TLangMode);
var
  i: longint;
begin
  listInit(lx.nameScopes);
  lexPushNames(lx); { the file's own declarations (declared()) }
  if not keywordsReady then
    initKeywords;
  lx.mode := mode;
  setBuffer(lx, fid, 0);
  listInit(lx.includes);
  lx.hasPendingInclude := false;
  bufInit(lx.pendingInclude);
  lx.pendingIncludeLoc := noLoc;
  mapInit(lx.defines);
  listInit(lx.conds);
  lx.corbaInterfaces := false;
  lx.advancedRecords := false;
  lx.typeHelpers := false;
  lx.longStrings := false;
  lx.macrosOn := false;
  lx.inlineOn := mode = lmObjFPC;
  lx.cOperators := mode <> lmISO;
  lx.resultSwitch := mode = lmObjFPC;
  lx.outSwitch := mode = lmObjFPC;
  lx.defaultParamsSwitch := mode = lmObjFPC;
  lx.scopedEnums := false;
  lx.packRecords := 0;
  listInit(lx.switchStack);
  mapInit(lx.macros);
  listInit(lx.macroStack);
  defineSymbol(lx, 'PCC', true);
  if mode = lmISO then
    defineSymbol(lx, 'PCC_ISO', true)
  else if mode = lmTP then
    defineSymbol(lx, 'PCC_TP', true)
  else
    defineSymbol(lx, 'PCC_OBJFPC', true);
  for i := 0 to commandLineDefines.count - 1 do
    defineSymbol(lx, nameStr(PName(listGet(commandLineDefines, i))), true);
end;

procedure formToken(var lx: TLexer; var t: TToken; k: TTokenKind; start: int64);
begin
  t.kind := k;
  t.loc := locOf(lx, start);
  t.len := longint(lx.cur - start);
  t.name := nil;
  t.strValue := nil;
  t.intValue := 0;
  t.based := false;
  t.wideChar := 0;
  t.realValue := 0;
end;

function tokenText(const t: TToken): string;
var
  s: string;
  fid, i, n: longint;
  data: PCharArray;
  off: int64;
begin
  s := '';
  fid := smFileOf(t.loc);
  if fid = 0 then
    exit('');
  data := smFileData(fid);
  off := t.loc - smFileStart(fid);
  n := t.len;
  if n > 255 then
    n := 255;
  {$R-}
  for i := 0 to n - 1 do
    s := s + data^[off + i];
  {$R+}
  tokenText := s;
end;

procedure bufAddTokenText(var b: TBuf; const t: TToken);
var
  fid: longint;
begin
  fid := smFileOf(t.loc);
  if fid <> 0 then
    bufAddChars(b, smFileData(fid), t.loc - smFileStart(fid), t.len);
end;

function tokenEnd(const t: TToken): TLoc;
begin
  tokenEnd := t.loc + t.len;
end;

function tokenRange(const t: TToken): TRange;
begin
  tokenRange := makeRange(t.loc, t.loc + t.len);
end;

{ --- Directives --- }

procedure addDirectiveState(const lx: TLexer; loc: TLoc; sw: TCheckSwitch;
                            state: longint);
var
  d: PDirective;
begin
  d := PDirective(arenaAlloc(sizeof(TDirective)));
  d^.loc := loc;
  d^.fid := lx.fid;
  d^.sw := sw;
  d^.state := state;
  listAdd(directives, d);
end;

procedure addDirective(const lx: TLexer; loc: TLoc; sw: TCheckSwitch; isOn: boolean);
begin
  addDirectiveState(lx, loc, sw, ord(isOn));
end;

{ The state of switch sw at loc as the switches so far set it (in loc's
  file): -1 (the default) if none did. }
function directiveStateAt(sw: TCheckSwitch; loc: TLoc): longint;
var
  i, fid: longint;
  d: PDirective;
begin
  directiveStateAt := -1;
  fid := smFileOf(loc);
  for i := 0 to directives.count - 1 do
  begin
    d := PDirective(listGet(directives, i));
    if (d^.sw = sw) and (fid <> 0) and (d^.fid = fid) and (d^.loc <= loc) then
      directiveStateAt := d^.state;
  end;
end;

function checkEnabled(sw: TCheckSwitch; loc: TLoc; default: boolean): boolean;
var
  state: longint;
begin
  state := directiveStateAt(sw, loc);
  if state = -1 then
    checkEnabled := default
  else
    checkEnabled := state = 1;
end;

function upperChar(c: char): char;
begin
  if (c >= 'a') and (c <= 'z') then
    upperChar := chr(ord(c) - 32)
  else
    upperChar := c;
end;

function isTrimSpace(c: char): boolean;
begin
  { llvm::StringRef::trim: " \t\n\v\f\r" }
  isTrimSpace := c in [' ', #9, #10, #11, #12, #13];
end;

{ --- Conditional compilation --- }

function isActive(const lx: TLexer): boolean;
begin
  if lx.conds.count = 0 then
    isActive := true
  else
    isActive := PConditional(listGet(lx.conds, lx.conds.count - 1))^.active;
end;

{ --- $IF expressions --- }

const
  maxInt64: int64 = 9223372036854775807;
  minInt64: int64 = -9223372036854775807 - 1;

{ A value in a $IF expression. }
type
  TCondKind = (ckBool, ckInt, ckStr);
  TCondValue = record
    kind: TCondKind;
    b: boolean;
    i: int64;
    s: string;
  end;

  { A text [p, e) of a buffer being evaluated; the first error is kept. }
  TCondParser = record
    data: PCharArray;
    p, e: int64;
    depth: longint;
    hasError: boolean;
    error: string;
  end;

function condBool(v: boolean): TCondValue;
var
  c: TCondValue;
begin
  c.kind := ckBool;
  c.b := v;
  c.i := 0;
  c.s := '';
  condBool := c;
end;

function condInt(v: int64): TCondValue;
var
  c: TCondValue;
begin
  c := condBool(false);
  c.kind := ckInt;
  c.i := v;
  condInt := c;
end;

function cpAt(const cp: TCondParser; i: int64): char;
begin
  if i < cp.e then
  begin
    {$R-}
    cpAt := cp.data^[i];
    {$R+}
  end
  else
    cpAt := #0;
end;

{ The next token [ts, te): an identifier, digits, $hex digits, a string
  literal, <= >= <> .., or one character; ts = te at the end. }
procedure cpPeek(var cp: TCondParser; var ts, te: int64);
var
  c: char;
  done: boolean;
begin
  while (cp.p < cp.e) and isTrimSpace(cpAt(cp, cp.p)) do
    cp.p := cp.p + 1;
  ts := cp.p;
  te := cp.p;
  if cp.p >= cp.e then
    exit;
  c := cpAt(cp, te);
  if isIdentStart(c) then
  begin
    while (te < cp.e) and isIdentBody(cpAt(cp, te)) do
      te := te + 1;
  end
  else if isDigit(c) then
  begin
    while (te < cp.e) and isDigit(cpAt(cp, te)) do
      te := te + 1;
  end
  else if c = '$' then
  begin
    te := te + 1;
    while (te < cp.e) and isHexDigit(cpAt(cp, te)) do
      te := te + 1;
  end
  else if c = '''' then
  begin
    te := te + 1;
    done := false;
    while (te < cp.e) and not done do
    begin
      te := te + 1;
      if cpAt(cp, te - 1) = '''' then
      begin
        if (te < cp.e) and (cpAt(cp, te) = '''') then
          te := te + 1 { '' stands for ' }
        else
          done := true;
      end;
    end;
  end
  else if ((c = '<') and (te + 1 < cp.e) and
           ((cpAt(cp, te + 1) = '=') or (cpAt(cp, te + 1) = '>'))) or
          ((c = '>') and (te + 1 < cp.e) and (cpAt(cp, te + 1) = '=')) or
          ((c = '.') and (te + 1 < cp.e) and (cpAt(cp, te + 1) = '.')) then
    te := te + 2
  else
    te := te + 1;
end;

function cpText(const cp: TCondParser; ts, te: int64): string;
var
  s: string;
  i: int64;
begin
  s := '';
  i := ts;
  while (i < te) and (length(s) < 255) do
  begin
    s := s + cpAt(cp, i);
    i := i + 1;
  end;
  cpText := s;
end;

{ The next token, lower case (for keywords and operators). }
function cpPeekLower(var cp: TCondParser): string;
var
  ts, te: int64;
begin
  cpPeek(cp, ts, te);
  cpPeekLower := lowerStr(cpText(cp, ts, te));
end;

function cpAccept(var cp: TCondParser; const w: string): boolean;
var
  ts, te: int64;
begin
  cpPeek(cp, ts, te);
  if lowerStr(cpText(cp, ts, te)) <> w then
    exit(false);
  cp.p := te;
  cpAccept := true;
end;

function cpFail(var cp: TCondParser; const msg: string): TCondValue;
begin
  if not cp.hasError then
  begin
    cp.hasError := true;
    cp.error := msg;
  end;
  cpFail := condBool(false);
end;

function cpInvalid(var cp: TCondParser): TCondValue;
begin
  cpInvalid := cpFail(cp, 'invalid $IF expression (expected values, macros, ' +
                      'defined(NAME), comparisons, + - * div mod, not, and, or)');
end;

function cpIncompatible(var cp: TCondParser): TCondValue;
begin
  cpIncompatible := cpFail(cp, 'incompatible types in $IF expression');
end;

function cpOverflow(var cp: TCondParser): TCondValue;
begin
  cpOverflow := cpFail(cp, 'integer overflow in $IF expression');
end;

function cpBothAre(var cp: TCondParser; const l, r: TCondValue; k: TCondKind): boolean;
var
  ignored: TCondValue;
begin
  if (l.kind = k) and (r.kind = k) then
    exit(true);
  ignored := cpIncompatible(cp);
  cpBothAre := false;
end;

{ The macro named n (upper case), or nil. }
function findMacro(const lx: TLexer; const n: string): PMacro;
begin
  findMacro := PMacro(mapGet(lx.macros, upperName(n)^.id));
end;

function cpParseExpr(var lx: TLexer; var cp: TCondParser): TCondValue; forward;
function cpParseSimple(var lx: TLexer; var cp: TCondParser): TCondValue; forward;
function cpParseFactor(var lx: TLexer; var cp: TCondParser): TCondValue; forward;

{ Integer literal text (digits, or $ and hex digits) as int64: ok false for
  an invalid literal (more than 64 bits, or no digits), overflow true for
  one beyond int64 (getAsInteger into 64 unsigned bits). }
procedure parseCondInt(const t: string; var v: int64; var ok, overflow: boolean);
var
  digits: string;
  i, d, radix, maxLen: longint;
  maxU, maxS: string;
begin
  ok := false;
  overflow := false;
  v := 0;
  if t[1] = '$' then
  begin
    digits := copy(t, 2, 255);
    radix := 16;
  end
  else
  begin
    digits := t;
    radix := 10;
  end;
  if digits = '' then
    exit;
  while (length(digits) > 1) and (digits[1] = '0') do
    digits := copy(digits, 2, 255);
  digits := lowerStr(digits);
  if radix = 10 then
  begin
    maxU := '18446744073709551615';
    maxS := '9223372036854775807';
  end
  else
  begin
    maxU := 'ffffffffffffffff';
    maxS := '7fffffffffffffff';
  end;
  maxLen := length(maxU);
  if (length(digits) > maxLen) or ((length(digits) = maxLen) and (digits > maxU)) then
    exit; { beyond 64 bits }
  ok := true;
  if (length(digits) > length(maxS)) or
     ((length(digits) = length(maxS)) and (digits > maxS)) then
  begin
    overflow := true;
    exit;
  end;
  for i := 1 to length(digits) do
  begin
    if isDigit(digits[i]) then
      d := ord(digits[i]) - ord('0')
    else
      d := ord(digits[i]) - ord('a') + 10;
    v := v * radix + d;
  end;
end;

{ The value of macro m (see evalCondition). }
function cpMacroValue(var lx: TLexer; var cp: TCondParser; m: PMacro): TCondValue;
var
  sub: TCondParser;
  ts, te: int64;
  single: boolean;
  c: char;
  v: TCondValue;
  other: PMacro;
begin
  if cp.depth >= 16 then
    exit(cpFail(cp, 'macros in $IF expressions are nested too deeply'));
  sub.data := smFileData(m^.fid);
  sub.p := m^.b;
  sub.e := m^.e;
  sub.depth := cp.depth + 1;
  sub.hasError := false;
  sub.error := '';
  cpPeek(sub, ts, te);
  single := (ts < te) and (te = sub.e);
  other := nil;
  if single then
  begin
    c := cpAt(sub, ts);
    if isDigit(c) or (c = '$') or (c = '''') then
    begin
      v := cpParseFactor(lx, sub); { an integer or string literal }
      if not sub.hasError then
        exit(v);
    end
    else if isIdentStart(c) then
    begin
      other := findMacro(lx, cpText(sub, ts, te));
      if other <> nil then
      begin
        v := cpMacroValue(lx, sub, other); { another macro's value }
        if sub.hasError then
          exit(cpFail(cp, sub.error));
        exit(v);
      end;
    end;
  end;
  v := condBool(false); { its text }
  v.kind := ckStr;
  v.s := cpText(sub, m^.b, m^.e);
  cpMacroValue := v;
end;

{ The size in bytes of the predefined type called name in tp and objfpc
  modes, or -1. Must agree with sizeof in the generated code: $IF is
  evaluated while lexing, before any type is known. }
function predefinedSize(const name: string): int64;
var
  u: string;
  i: longint;
begin
  u := name;
  for i := 1 to length(u) do
    u[i] := upperChar(u[i]);
  predefinedSize := -1;
  if (u = 'CHAR') or (u = 'ANSICHAR') or (u = 'BOOLEAN') or (u = 'BYTE') or
     (u = 'SHORTINT') then
    predefinedSize := 1
  else if (u = 'WORD') or (u = 'SMALLINT') then
    predefinedSize := 2
  else if (u = 'INTEGER') or (u = 'LONGINT') or (u = 'CARDINAL') or
          (u = 'LONGWORD') or (u = 'DWORD') then
    predefinedSize := 4
  else if (u = 'INT64') or (u = 'REAL') or (u = 'DOUBLE') or
          (u = 'POINTER') or (u = 'PCHAR') or (u = 'ANSISTRING') or
          (u = 'SIZEINT') or (u = 'PTRINT') or (u = 'NATIVEINT') or
          (u = 'PANSICHAR') or (u = 'PGUID') or (u = 'QWORD') or
          (u = 'UINT64') or (u = 'NATIVEUINT') or (u = 'PTRUINT') or
          (u = 'SIZEUINT') then
    predefinedSize := 8
  else if u = 'TGUID' then
    predefinedSize := 16
  else if u = 'SHORTSTRING' then
    predefinedSize := 256;
end;

function cpParseFactor(var lx: TLexer; var cp: TCondParser): TCondValue;
var
  v: TCondValue;
  ts, te, i: int64;
  t, name: string;
  ok, overflow: boolean;
  n: int64;
  m: PMacro;
begin
  if cpAccept(cp, 'not') then
  begin
    v := cpParseFactor(lx, cp);
    if cp.hasError then
      exit(condBool(false));
    if v.kind <> ckBool then
      exit(cpIncompatible(cp));
    v.b := not v.b;
    exit(v);
  end;
  if cpAccept(cp, '(') then
  begin
    v := cpParseExpr(lx, cp);
    if not cp.hasError and not cpAccept(cp, ')') then
      exit(cpInvalid(cp));
    exit(v);
  end;
  cpPeek(cp, ts, te);
  if ts = te then
    exit(cpInvalid(cp));
  t := cpText(cp, ts, te);
  if isDigit(t[1]) or (t[1] = '$') then
  begin
    cp.p := te;
    parseCondInt(t, n, ok, overflow);
    if not ok then
      exit(cpInvalid(cp));
    if overflow then
      exit(cpOverflow(cp));
    exit(condInt(n));
  end;
  if t[1] = '''' then
  begin
    cp.p := te;
    if (te - ts < 2) or (cpAt(cp, te - 1) <> '''') then
      exit(cpInvalid(cp));
    v := condBool(false);
    v.kind := ckStr;
    i := ts + 1;
    while i + 1 < te do
    begin
      if length(v.s) < 255 then
        v.s := v.s + cpAt(cp, i);
      if cpAt(cp, i) = '''' then
        i := i + 1; { '' }
      i := i + 1;
    end;
    exit(v);
  end;
  if cpAccept(cp, 'true') then
    exit(condBool(true));
  if cpAccept(cp, 'false') then
    exit(condBool(false));
  if cpAccept(cp, 'defined') then
  begin
    if not cpAccept(cp, '(') then
      exit(cpInvalid(cp));
    cpPeek(cp, ts, te);
    if (ts = te) or not isIdentStart(cpAt(cp, ts)) then
      exit(cpInvalid(cp));
    name := cpText(cp, ts, te);
    cp.p := te;
    if not cpAccept(cp, ')') then
      exit(cpInvalid(cp));
    exit(condBool(isDefined(lx, name)));
  end;
  if cpAccept(cp, 'declared') then { declared(NAME): an identifier declared so far }
  begin
    if not cpAccept(cp, '(') then
      exit(cpInvalid(cp));
    cpPeek(cp, ts, te);
    if (ts = te) or not isIdentStart(cpAt(cp, ts)) then
      exit(cpInvalid(cp));
    name := cpText(cp, ts, te);
    cp.p := te;
    if not cpAccept(cp, ')') then
      exit(cpInvalid(cp));
    exit(condBool(lexIsDeclared(lx, intern(lowerStr(name)))));
  end;
  if (lx.mode <> lmISO) and (findMacro(lx, 'sizeof') = nil) and
     cpAccept(cp, 'sizeof') then
  begin
    if not cpAccept(cp, '(') then
      exit(cpInvalid(cp));
    cpPeek(cp, ts, te);
    if (ts = te) or not isIdentStart(cpAt(cp, ts)) then
      exit(cpInvalid(cp));
    name := cpText(cp, ts, te);
    cp.p := te;
    if not cpAccept(cp, ')') then
      exit(cpInvalid(cp));
    n := predefinedSize(name);
    if n < 0 then
      exit(cpFail(cp, '''sizeof'' in $IF needs a predefined type, not ''' +
                  name + ''''));
    exit(condInt(n));
  end;
  if not isIdentStart(t[1]) then
    exit(cpInvalid(cp));
  cp.p := te;
  m := findMacro(lx, t);
  if m = nil then
    exit(cpFail(cp, '''' + t + ''' has no value in $IF (a macro needs ' +
                '{$macro on} and {$define ' + t + ' := value})'));
  cpParseFactor := cpMacroValue(lx, cp, m);
end;

{$Q-}
function cpParseTerm(var lx: TLexer; var cp: TCondParser): TCondValue;
var
  v, r: TCondValue;
  op: string;
  x: int64;
begin
  v := cpParseFactor(lx, cp);
  while not cp.hasError do
  begin
    op := cpPeekLower(cp);
    if (op <> '*') and (op <> 'div') and (op <> 'mod') and (op <> 'and') then
      break;
    cpAccept(cp, op);
    r := cpParseFactor(lx, cp);
    if cp.hasError then
      exit(condBool(false));
    if op = 'and' then
    begin
      if cpBothAre(cp, v, r, ckBool) then
        v.b := v.b and r.b;
      continue;
    end;
    if not cpBothAre(cp, v, r, ckInt) then
      break;
    if op = '*' then
    begin
      if (v.i <> 0) and (r.i <> 0) then
      begin
        if ((v.i = -1) and (r.i = minInt64)) or
           ((r.i = -1) and (v.i = minInt64)) then
          exit(cpOverflow(cp));
        x := v.i * r.i;
        if x div r.i <> v.i then
          exit(cpOverflow(cp));
        v.i := x;
      end
      else
        v.i := 0;
      continue;
    end;
    if r.i = 0 then
      exit(cpFail(cp, 'division by zero in $IF expression'));
    if (v.i = minInt64) and (r.i = -1) then
      exit(cpOverflow(cp));
    if op = 'div' then
      v.i := v.i div r.i
    else
      v.i := v.i mod r.i;
  end;
  if cp.hasError then
    exit(condBool(false));
  cpParseTerm := v;
end;
{$Q+}

function cpParseSimple(var lx: TLexer; var cp: TCondParser): TCondValue;
var
  v, r: TCondValue;
  neg: boolean;
  op: string;
begin
  neg := false;
  if cpAccept(cp, '-') then
    neg := true
  else
    cpAccept(cp, '+');
  v := cpParseTerm(lx, cp);
  if cp.hasError then
    exit(condBool(false));
  if neg then
  begin
    if v.kind <> ckInt then
      exit(cpIncompatible(cp));
    if v.i = minInt64 then
      exit(cpOverflow(cp));
    v.i := -v.i;
  end;
  while not cp.hasError do
  begin
    op := cpPeekLower(cp);
    if (op <> '+') and (op <> '-') and (op <> 'or') then
      break;
    cpAccept(cp, op);
    r := cpParseTerm(lx, cp);
    if cp.hasError then
      exit(condBool(false));
    if op = 'or' then
    begin
      if cpBothAre(cp, v, r, ckBool) then
        v.b := v.b or r.b;
    end
    else if (op = '+') and (v.kind = ckStr) and (r.kind = ckStr) then
    begin
      if length(v.s) + length(r.s) <= 255 then
        v.s := v.s + r.s
      else
        v.s := copy(v.s + r.s, 1, 255);
    end
    else if cpBothAre(cp, v, r, ckInt) then
    begin
      if op = '+' then
      begin
        if ((r.i > 0) and (v.i > maxInt64 - r.i)) or
           ((r.i < 0) and (v.i < minInt64 - r.i)) then
          exit(cpOverflow(cp));
        v.i := v.i + r.i;
      end
      else
      begin
        if ((r.i < 0) and (v.i > maxInt64 + r.i)) or
           ((r.i > 0) and (v.i < minInt64 + r.i)) then
          exit(cpOverflow(cp));
        v.i := v.i - r.i;
      end;
    end;
  end;
  if cp.hasError then
    exit(condBool(false));
  cpParseSimple := v;
end;

{ -1, 0, 1: l compared with r (of the same kind) }
function condCompare(const l, r: TCondValue): longint;
begin
  if l.kind = ckStr then
  begin
    if l.s < r.s then
      exit(-1);
    if l.s > r.s then
      exit(1);
    exit(0);
  end;
  if l.kind = ckInt then
  begin
    if l.i < r.i then
      exit(-1);
    if l.i > r.i then
      exit(1);
    exit(0);
  end;
  condCompare := ord(l.b) - ord(r.b);
end;

function cpParseExpr(var lx: TLexer; var cp: TCondParser): TCondValue;
var
  l, r, lo, hi: TCondValue;
  op: string;
  c: longint;
  inSet, range_, hit: boolean;
begin
  l := cpParseSimple(lx, cp);
  if cp.hasError then
    exit(condBool(false));
  op := cpPeekLower(cp);
  if op = 'in' then { x in [a, b, lo..hi] }
  begin
    cpAccept(cp, 'in');
    if not cpAccept(cp, '[') then
      exit(cpInvalid(cp));
    inSet := false;
    if not cpAccept(cp, ']') then
    begin
      repeat
        lo := cpParseSimple(lx, cp);
        hi := lo;
        range_ := not cp.hasError and cpAccept(cp, '..');
        if range_ then
          hi := cpParseSimple(lx, cp);
        if cp.hasError then
          exit(condBool(false));
        if (lo.kind <> l.kind) or (range_ and (hi.kind <> l.kind)) then
          exit(cpIncompatible(cp));
        if range_ then
          hit := (condCompare(lo, l) <= 0) and (condCompare(l, hi) <= 0)
        else
          hit := condCompare(lo, l) = 0;
        if hit then
          inSet := true;
      until not cpAccept(cp, ',');
      if not cpAccept(cp, ']') then
        exit(cpInvalid(cp));
    end;
    exit(condBool(inSet));
  end;
  if (op <> '=') and (op <> '<>') and (op <> '<') and (op <> '>') and
     (op <> '<=') and (op <> '>=') then
    exit(l);
  cpAccept(cp, op);
  r := cpParseSimple(lx, cp);
  if cp.hasError then
    exit(condBool(false));
  if l.kind <> r.kind then
    exit(cpIncompatible(cp));
  c := condCompare(l, r);
  if op = '=' then
    cpParseExpr := condBool(c = 0)
  else if op = '<>' then
    cpParseExpr := condBool(c <> 0)
  else if op = '<' then
    cpParseExpr := condBool(c < 0)
  else if op = '>' then
    cpParseExpr := condBool(c > 0)
  else if op = '<=' then
    cpParseExpr := condBool(c <= 0)
  else
    cpParseExpr := condBool(c >= 0);
end;

{ The expression of $IF / $ELSEIF in [b, e), with Pascal's operator
  precedence: values (integers, 'strings', true, false, defined(NAME),
  macros), not, * div mod and, + - or, and one comparison = <> < > <= >=
  or in [values]. A macro's value is an integer if its text is an integer
  literal, a string if it is a string literal, the value of the macro it
  names if it is a macro's name, else its text (a string), as in Free
  Pascal. }
function evalCondition(var lx: TLexer; start, b, e: int64): boolean;
var
  cp: TCondParser;
  v: TCondValue;
  ts, te: int64;
  ignored: TCondValue;
begin
  cp.data := lx.data;
  cp.p := b;
  cp.e := e;
  cp.depth := 0;
  cp.hasError := false;
  cp.error := '';
  v := cpParseExpr(lx, cp);
  if not cp.hasError then
  begin
    cpPeek(cp, ts, te);
    if ts < te then
      ignored := cpInvalid(cp);
  end;
  if not cp.hasError and (v.kind <> ckBool) then
    ignored := cpFail(cp, 'the $IF expression is not boolean');
  if not cp.hasError then
    exit(v.b);
  diagError(locOf(lx, start), cp.error);
  evalCondition := false;
end;

{ Handles word if it is a conditional-compilation directive; its argument
  is [b, e), trimmed. }
function handleConditional(var lx: TLexer; start: int64; const word: string;
                           b, e: int64): boolean;
var
  name, sym: string;
  q: int64;
  isActiveNow, c: boolean;
  depth: longint;
  cond: PConditional;
  macro: PMacro;

  function needSymbol: boolean;
  begin
    if sym <> '' then
      exit(true);
    diagError(locOf(lx, start), 'expected a symbol name after ' + name);
    needSymbol := false;
  end;

begin
  handleConditional := true;
  if word = 'if' then name := '$IF'
  else if word = 'ifdef' then name := '$IFDEF'
  else if word = 'ifndef' then name := '$IFNDEF'
  else if word = 'ifopt' then name := '$IFOPT'
  else if word = 'elseif' then name := '$ELSEIF'
  else if word = 'else' then name := '$ELSE'
  else if word = 'endif' then name := '$ENDIF'
  else if word = 'ifend' then name := '$IFEND'
  else if word = 'define' then name := '$DEFINE'
  else if word = 'undef' then name := '$UNDEF'
  else
    exit(false);

  isActiveNow := isActive(lx);
  depth := lx.includes.count;
  { the symbol named by $DEFINE, $UNDEF, $IFDEF and $IFNDEF }
  sym := '';
  q := b;
  if (q < e) and isIdentStart(at(lx, q)) then
    while (q < e) and isIdentBody(at(lx, q)) do
    begin
      if length(sym) < 255 then
        sym := sym + at(lx, q);
      q := q + 1;
    end;

  if (name = '$DEFINE') or (name = '$UNDEF') then
  begin
    if not isActiveNow or not needSymbol then
      exit;
    mapPut(lx.macros, upperName(sym)^.id, nil);
    if name = '$UNDEF' then
    begin
      defineSymbol(lx, sym, false);
      exit;
    end;
    defineSymbol(lx, sym, true);
    while (q < e) and isTrimSpace(at(lx, q)) do
      q := q + 1;
    if (q + 1 < e) and (at(lx, q) = ':') and (at(lx, q + 1) = '=') then
    begin { a macro: $define NAME := value }
      if not lx.macrosOn then
      begin
        diagReport(sevWarning, locOf(lx, start), 'macro support is off: ''' + sym +
                   ''' is defined without a value (use {$macro on})', noRange, noRange);
        exit;
      end;
      q := q + 2;
      while (q < e) and isTrimSpace(at(lx, q)) do
        q := q + 1;
      macro := PMacro(arenaAlloc(sizeof(TMacro)));
      macro^.fid := lx.fid;
      macro^.b := q;
      macro^.e := e;
      mapPut(lx.macros, upperName(sym)^.id, macro);
    end;
    exit;
  end;

  if (name = '$IF') or (name = '$IFDEF') or (name = '$IFNDEF') or
     (name = '$IFOPT') then
  begin
    c := false;
    if isActiveNow then
    begin
      if name = '$IF' then
        c := evalCondition(lx, start, b, e)
      else if name = '$IFOPT' then
        diagError(locOf(lx, start), '$IFOPT is not supported')
      else if needSymbol then
        c := isDefined(lx, sym) = (name = '$IFDEF');
    end;
    cond := PConditional(arenaAlloc(sizeof(TConditional)));
    cond^.loc := locOf(lx, start);
    cond^.name := name;
    cond^.parentActive := isActiveNow;
    cond^.active := isActiveNow and c;
    cond^.taken := isActiveNow and c;
    cond^.seenElse := false;
    cond^.depth := depth;
    listAdd(lx.conds, cond);
    exit;
  end;

  { $ELSEIF, $ELSE, $ENDIF: they continue a conditional of the same file }
  cond := nil;
  if lx.conds.count > 0 then
    cond := PConditional(listGet(lx.conds, lx.conds.count - 1));
  if (cond = nil) or (cond^.depth <> depth) then
  begin
    diagError(locOf(lx, start), name + ' without a matching $IF or $IFDEF');
    exit;
  end;
  if (name = '$ENDIF') or (name = '$IFEND') then
  begin
    listPop(lx.conds);
    exit;
  end;
  if cond^.seenElse then
  begin
    diagError(locOf(lx, start), name + ' after $ELSE');
    cond^.active := false;
    exit;
  end;
  if (name = '$ELSEIF') and (cond^.name <> '$IF') then
  begin
    diagError(locOf(lx, start), '$ELSEIF cannot continue ' + cond^.name +
              '; use $ELSE');
    cond^.active := false;
    exit;
  end;
  if name = '$ELSE' then
  begin
    cond^.seenElse := true;
    cond^.active := cond^.parentActive and not cond^.taken;
  end
  else if cond^.parentActive and not cond^.taken then
    cond^.active := evalCondition(lx, start, b, e)
  else
    cond^.active := false;
  cond^.taken := cond^.taken or cond^.active;
end;

{ At the end of a buffer: reports the conditionals it left open. }
procedure closeConditionals(var lx: TLexer);
var
  cond: PConditional;
begin
  while lx.conds.count > 0 do
  begin
    cond := PConditional(listGet(lx.conds, lx.conds.count - 1));
    if cond^.depth <> lx.includes.count then
      exit;
    diagError(cond^.loc, cond^.name + ' is not terminated; expected $ENDIF ' +
              'before the end of the file');
    listPop(lx.conds);
  end;
end;

{ Directive text [textStart, textEnd) (after the '$', up to the comment
  closer); start is the offset of the comment opener. The text can be long
  and span lines, so it is handled as offsets, not as a string. }
{ A record alignment in $packrecords, $align and $aN: 1, 2, 4, 8, 16 or 32
  (decimal digits), else -1. }
function packValue(const s: string): longint;
var
  i: longint;
  v: int64;
begin
  packValue := -1;
  if (length(s) = 0) or (length(s) > 18) then
    exit;
  v := 0;
  for i := 1 to length(s) do
  begin
    if (s[i] < '0') or (s[i] > '9') then
      exit;
    v := v * 10 + (ord(s[i]) - ord('0'));
  end;
  if (v = 1) or (v = 2) or (v = 4) or (v = 8) or (v = 16) or (v = 32) then
    packValue := longint(v);
end;

{ $modeswitch NAME[+|-| ON| OFF] with Free Pascal 3.2.2's names (text
  [b, e) after the word): advancedrecords and typehelpers (objfpc),
  result, out and defaultparameters (any mode) turn their features on or
  off; the others pcc's mode decides (class, objpas, properties,
  exceptions: objfpc; cvar, initfinal, hintdirective, allowinline: always)
  or pcc lacks: a request for what pcc does is accepted, any other is
  reported and ignored. }
procedure handleModeSwitch(var lx: TLexer; start, b, e: int64);
var
  q: int64;
  n, rest, kind: string;
  isOn, objfpc: boolean;
begin
  q := b;
  while (q < e) and isIdentBody(at(lx, q)) do
    q := q + 1;
  n := lowerStr(copyChars(lx, b, q));
  rest := lowerStr(copyChars(lx, q, e));
  while (length(rest) > 0) and isTrimSpace(rest[1]) do
    rest := copy(rest, 2, length(rest) - 1);
  if (rest = '') or (rest = '+') or (rest = 'on') then
    isOn := true
  else if (rest = '-') or (rest = 'off') then
    isOn := false
  else
  begin
    diagReport(sevWarning, locOf(lx, start),
               'expected {$modeswitch NAME} (with +, -, ON or OFF); ignored',
               noRange, noRange);
    exit;
  end;
  objfpc := lx.mode = lmObjFPC;
  if n = 'advancedrecords' then
  begin
    lx.advancedRecords := isOn;
    exit;
  end;
  if n = 'typehelpers' then
  begin
    lx.typeHelpers := isOn;
    exit;
  end;
  if n = 'result' then
  begin
    lx.resultSwitch := isOn;
    exit;
  end;
  if n = 'out' then
  begin
    lx.outSwitch := isOn;
    exit;
  end;
  if n = 'defaultparameters' then
  begin
    lx.defaultParamsSwitch := isOn;
    exit;
  end;
  { the others: what pcc's mode decides, what it always does, or a feature
    it lacks }
  if (n = 'class') or (n = 'objpas') or (n = 'properties') or (n = 'exceptions') then
    kind := 'mode'
  else if (n = 'cvar') or (n = 'initfinal') or (n = 'hintdirective') or
          (n = 'allowinline') then
    kind := 'always'
  else if (n = 'pchartostring') or (n = 'nestedcomments') or
          (n = 'classicprocvars') or (n = 'macprocvars') or
          (n = 'repeatforward') or (n = 'pointertoprocvar') or
          (n = 'autoderef') or (n = 'ansistrings') or (n = 'duplicatelocals') or
          (n = 'objectivec1') or (n = 'objectivec2') or (n = 'nestedprocvars') or
          (n = 'nonlocalgoto') or (n = 'isounaryminus') or
          (n = 'systemcodepage') or (n = 'finalfields') or
          (n = 'unicodestrings') or (n = 'cblocks') or (n = 'isoio') or
          (n = 'isoprogramparas') or (n = 'isomod') or (n = 'arrayoperators') then
    kind := 'lacking'
  else
  begin
    diagReport(sevWarning, locOf(lx, start),
               'unknown mode switch ''' + copyChars(lx, b, q) + '''; ignored',
               noRange, noRange);
    exit;
  end;
  if (kind = 'mode') and (isOn <> objfpc) then
  begin
    if isOn then
      diagReport(sevWarning, locOf(lx, start), 'mode switch ''' + n +
                 ''' cannot be turned on in --mode=tp; use --mode=objfpc (ignored)',
                 noRange, noRange)
    else
      diagReport(sevWarning, locOf(lx, start), 'mode switch ''' + n +
                 ''' cannot be turned off in --mode=objfpc; use --mode=tp (ignored)',
                 noRange, noRange);
  end
  else if (kind = 'always') and not isOn then
    diagReport(sevWarning, locOf(lx, start), 'mode switch ''' + n +
               ''' cannot be turned off; ignored', noRange, noRange)
  else if (kind = 'lacking') and isOn then
    diagReport(sevWarning, locOf(lx, start), 'mode switch ''' + n +
               ''' is not supported; ignored', noRange, noRange);
end;

procedure handleDirective(var lx: TLexer; start: int64; textStart, textEnd: int64);
var
  p, b, e, partEnd: int64;
  word, arg: string;
  msg: TBuf;
  c: char;
  saved: PSavedSwitches;
begin
  p := textStart;
  word := '';
  while (p < textEnd) and isAlpha(at(lx, p)) do
  begin
    if length(word) < 20 then
      word := word + lowerChar(at(lx, p));
    p := p + 1;
  end;
  b := p;
  e := textEnd;
  while (b < e) and isTrimSpace(at(lx, b)) do
    b := b + 1;
  while (e > b) and isTrimSpace(at(lx, e - 1)) do
    e := e - 1;
  if handleConditional(lx, start, word, b, e) then
    exit;
  if not isActive(lx) then
    exit; { in a branch that is not compiled }
  { $I file / $INCLUDE file }
  if ((word = 'i') or (word = 'include')) and (e > b) and
     not ((e - b = 1) and ((at(lx, b) = '+') or (at(lx, b) = '-'))) then
  begin
    if (e - b >= 2) and (at(lx, b) = '''') and (at(lx, e - 1) = '''') then
    begin
      b := b + 1;
      e := e - 1;
    end;
    bufClear(lx.pendingInclude);
    bufAddChars(lx.pendingInclude, lx.data, b, e - b);
    lx.hasPendingInclude := true;
    lx.pendingIncludeLoc := locOf(lx, start);
    exit;
  end;
  { $interfaces com / corba: the kind of the interfaces declared after it }
  if word = 'interfaces' then
  begin
    arg := '';
    while (b < e) and (length(arg) < 20) do
    begin
      arg := arg + lowerChar(at(lx, b));
      b := b + 1;
    end;
    if (arg = 'corba') and (b >= e) then
      lx.corbaInterfaces := true
    else if (arg = 'com') and (b >= e) then
      lx.corbaInterfaces := false
    else
      diagError(locOf(lx, start), 'expected {$interfaces com} or ' +
                '{$interfaces corba}');
    exit;
  end;
  { $inline on / off: whether the inline directive inlines (default: on in
    objfpc mode, as in Free Pascal) }
  { $push / $pop: save and restore the local switches (at most 21 levels,
    as in FPC) }
  if (word = 'push') and (e = b) then
  begin
    if lx.switchStack.count >= 21 then
    begin
      diagError(locOf(lx, start), 'too many levels of {$push} (at most 21)');
      exit;
    end;
    saved := PSavedSwitches(arenaAlloc(sizeof(TSavedSwitches)));
    saved^.longStrings := lx.longStrings;
    saved^.corbaInterfaces := lx.corbaInterfaces;
    saved^.inlineOn := lx.inlineOn;
    saved^.scopedEnums := lx.scopedEnums;
    saved^.packRecords := lx.packRecords;
    saved^.range := directiveStateAt(swRange, locOf(lx, start));
    saved^.overflow := directiveStateAt(swOverflow, locOf(lx, start));
    listAdd(lx.switchStack, saved);
    exit;
  end;
  if (word = 'pop') and (e = b) then
  begin
    if lx.switchStack.count = 0 then
    begin
      diagError(locOf(lx, start), '{$pop} without a preceding {$push}');
      exit;
    end;
    saved := PSavedSwitches(listPop(lx.switchStack));
    lx.longStrings := saved^.longStrings;
    lx.corbaInterfaces := saved^.corbaInterfaces;
    lx.inlineOn := saved^.inlineOn;
    lx.scopedEnums := saved^.scopedEnums;
    lx.packRecords := saved^.packRecords;
    addDirectiveState(lx, locOf(lx, start), swRange, saved^.range);
    addDirectiveState(lx, locOf(lx, start), swOverflow, saved^.overflow);
    exit;
  end;
  { $packrecords N|C|DEFAULT|NORMAL, $align N|ON|OFF, $aN, $a+|-: how the
    fields of the records declared after it are aligned (as in FPC, where
    ON and + mean 4, OFF and - mean 1) }
  if word = 'packrecords' then
  begin
    arg := lowerStr(copyChars(lx, b, e));
    if (arg = 'c') or (arg = 'default') or (arg = 'normal') then
      lx.packRecords := 0
    else if packValue(arg) > 0 then
      lx.packRecords := packValue(arg)
    else
      diagError(locOf(lx, start), 'illegal record alignment ''' +
                copyChars(lx, b, e) + ''': use 1, 2, 4, 8, 16, 32, C, ' +
                'DEFAULT or NORMAL');
    exit;
  end;
  if word = 'align' then
  begin
    arg := lowerStr(copyChars(lx, b, e));
    if (arg = 'on') or (arg = '+') then
      lx.packRecords := 4
    else if (arg = 'off') or (arg = '-') then
      lx.packRecords := 1
    else if packValue(arg) > 0 then
      lx.packRecords := packValue(arg)
    else
      diagError(locOf(lx, start), 'illegal record alignment ''' +
                copyChars(lx, b, e) + ''': use 1, 2, 4, 8, 16, 32, ON or OFF');
    exit;
  end;
  if (word = 'a') and (packValue(copyChars(lx, b, e)) > 0) then { $A4 }
  begin
    lx.packRecords := packValue(copyChars(lx, b, e));
    exit;
  end;
  if word = 'scopedenums' then
  begin
    arg := lowerStr(copyChars(lx, b, e));
    if (arg = 'on') or (arg = '+') then
      lx.scopedEnums := true
    else if (arg = 'off') or (arg = '-') then
      lx.scopedEnums := false
    else
      diagError(locOf(lx, start),
                'expected {$scopedenums on} or {$scopedenums off}');
    exit;
  end;
  if word = 'inline' then
  begin
    arg := lowerStr(copyChars(lx, b, e));
    if (arg = 'on') or (arg = '+') then
      lx.inlineOn := true
    else if (arg = 'off') or (arg = '-') then
      lx.inlineOn := false
    else
      diagError(locOf(lx, start), 'expected {$inline on} or {$inline off}');
    exit;
  end;
  { $coperators on / off: whether += -= *= /= are assignments (default: on
    except in ISO mode; as in Free Pascal 3.2, $pop does not restore it) }
  if word = 'coperators' then
  begin
    arg := lowerStr(copyChars(lx, b, e));
    if (arg = 'on') or (arg = '+') then
      lx.cOperators := true
    else if (arg = 'off') or (arg = '-') then
      lx.cOperators := false
    else
      diagError(locOf(lx, start),
                'expected {$coperators on} or {$coperators off}');
    exit;
  end;
  { $macro on / off: whether macros ($define NAME := value) are defined and
    expanded }
  if word = 'macro' then
  begin
    arg := lowerStr(copyChars(lx, b, e));
    if (arg = 'on') or (arg = '+') then
      lx.macrosOn := true
    else if (arg = 'off') or (arg = '-') then
      lx.macrosOn := false
    else
      diagError(locOf(lx, start), 'expected {$macro on} or {$macro off}');
    exit;
  end;
  { $longstrings on / off: the long form of H+ and H- }
  if word = 'longstrings' then
  begin
    arg := lowerStr(copyChars(lx, b, e));
    if (arg = 'on') or (arg = 'off') then
    begin
      lx.longStrings := arg = 'on';
      exit;
    end;
  end;
  if word = 'modeswitch' then
  begin
    handleModeSwitch(lx, start, b, e);
    exit;
  end;
  { a switch list: R-,Q+ }
  p := textStart;
  while true do
  begin
    partEnd := p;
    while (partEnd < textEnd) and (at(lx, partEnd) <> ',') do
      partEnd := partEnd + 1;
    b := p;
    e := partEnd;
    while (b < e) and isTrimSpace(at(lx, b)) do
      b := b + 1;
    while (e > b) and isTrimSpace(at(lx, e - 1)) do
      e := e - 1;
    c := at(lx, b + 1);
    if (e - b = 2) and isAlpha(at(lx, b)) and ((c = '+') or (c = '-')) then
    begin
      { other switches (listing, boolean evaluation, ...) are ignored }
      if upperChar(at(lx, b)) = 'H' then
        lx.longStrings := c = '+';
      if upperChar(at(lx, b)) = 'A' then { $A+: as $A4 (FPC); $A-: none }
      begin
        if c = '+' then
          lx.packRecords := 4
        else
          lx.packRecords := 1;
      end;
      if upperChar(at(lx, b)) = 'R' then
        addDirective(lx, locOf(lx, start), swRange, c = '+')
      else if upperChar(at(lx, b)) = 'Q' then
        addDirective(lx, locOf(lx, start), swOverflow, c = '+');
    end
    else if e > b then
    begin
      bufInit(msg);
      bufAdd(msg, 'compiler directive ''$');
      bufAddChars(msg, lx.data, b, e - b);
      bufAdd(msg, ''' is not supported; ignored');
      diagReportBuf(sevWarning, locOf(lx, start), msg, noRange, noRange);
      bufFree(msg);
      exit;
    end;
    if partEnd >= textEnd then
      exit;
    p := partEnd + 1;
  end;
end;

{ The file name as a C string sees it: up to a NUL byte (the C++ compiler
  passes it to the file system the same way). }
function includeName(const b: TBuf): string;
var
  s: string;
  i: int64;
begin
  s := '';
  i := 0;
  while (i < b.len) and (i < 255) and (bufCharAt(b, i) <> #0) do
  begin
    s := s + bufCharAt(b, i);
    i := i + 1;
  end;
  includeName := s;
end;

procedure enterPendingInclude(var lx: TLexer);
var
  name, found, candidate: string;
  i: longint;
  frame: PIncludeFrame;
  fid: longint;
  msg: TBuf;
begin
  name := includeName(lx.pendingInclude);
  lx.hasPendingInclude := false;
  if lx.includes.count >= 16 then
  begin
    diagError(lx.pendingIncludeLoc, 'include files are nested too deeply');
    exit;
  end;
  { next to the including file, then the -I directories }
  found := '';
  candidate := pathJoin(pathDirName(smFileName(lx.fid)), name);
  if sysIsRegularFile(candidate) then
    found := candidate;
  i := 0;
  while (found = '') and (i < includeDirs.count) do
  begin
    candidate := pathJoin(nameStr(PName(listGet(includeDirs, i))), name);
    if sysIsRegularFile(candidate) then
      found := candidate;
    i := i + 1;
  end;
  if found = '' then
  begin
    bufInit(msg);
    bufAdd(msg, 'cannot find include file ''');
    bufAddChars(msg, lx.pendingInclude.data, 0, lx.pendingInclude.len);
    bufAdd(msg, ''' (looked next to this file and in -I directories)');
    diagReportBuf(sevError, lx.pendingIncludeLoc, msg, noRange, noRange);
    bufFree(msg);
    exit;
  end;
  if not smLoadFile(found, fid) then
  begin
    diagError(lx.pendingIncludeLoc, 'cannot open include file ''' + found + '''');
    exit;
  end;
  frame := PIncludeFrame(arenaAlloc(sizeof(TIncludeFrame)));
  frame^.fid := lx.fid;
  frame^.cur := lx.cur;
  listAdd(lx.includes, frame);
  setBuffer(lx, fid, 0);
end;

{ --- Whitespace and comments --- }

{ Offset of the first occurrence of close at or after from, or -1. }
function findText(const lx: TLexer; from: int64; const close: string): int64;
var
  i: int64;
begin
  i := from;
  while i + length(close) <= lx.len do
  begin
    if (at(lx, i) = close[1]) and
       ((length(close) = 1) or (at(lx, i + 1) = close[2])) then
      exit(i);
    i := i + 1;
  end;
  findText := -1;
end;

procedure skipBlockComment(var lx: TLexer; openLen: longint; close: string);
var
  start, rest, p, other: int64;
  otherClose: string;
begin
  start := lx.cur;
  rest := lx.cur + openLen;
  p := findText(lx, rest, close);
  if lx.mode = lmISO then
  begin
    { ISO 7185 6.1.8: the two comment brackets are equivalent, so either
      closer ends either kind of comment }
    if close = '}' then
      otherClose := '*)'
    else
      otherClose := '}';
    other := findText(lx, rest, otherClose);
    if (other >= 0) and ((p < 0) or (other < p)) then
    begin
      p := other;
      close := otherClose;
    end;
  end;
  if p < 0 then
  begin
    diagErrorR(locOf(lx, start), 'unterminated comment',
               makeRange(locOf(lx, start), locOf(lx, start + openLen)));
    lx.cur := lx.len;
    exit;
  end;
  if at(lx, rest) = '$' then
    handleDirective(lx, start, rest + 1, p);
  lx.cur := p + length(close);
  if lx.hasPendingInclude then
    enterPendingInclude(lx); { the included text follows the comment }
end;

{ Skips the text of a branch that is not compiled: only comments (which
  may be conditional directives), strings and // comments matter. }
procedure skipInactive(var lx: TLexer);
var
  c: char;
begin
  while (lx.cur < lx.len) and not isActive(lx) do
  begin
    c := at(lx, lx.cur);
    if c = '{' then
      skipBlockComment(lx, 1, '}')
    else if (c = '(') and (at(lx, lx.cur + 1) = '*') then
      skipBlockComment(lx, 2, '*)')
    else if (c = '/') and (at(lx, lx.cur + 1) = '/') and (lx.mode <> lmISO) then
    begin
      while (lx.cur < lx.len) and (at(lx, lx.cur) <> #10) do
        lx.cur := lx.cur + 1;
    end
    else if c = '''' then
    begin
      lx.cur := lx.cur + 1;
      while (lx.cur < lx.len) and (at(lx, lx.cur) <> '''') and
            (at(lx, lx.cur) <> #10) do
        lx.cur := lx.cur + 1;
      if (lx.cur < lx.len) and (at(lx, lx.cur) = '''') then
        lx.cur := lx.cur + 1;
    end
    else
      lx.cur := lx.cur + 1;
  end;
end;

procedure skipWhitespaceAndComments(var lx: TLexer);
var
  c: char;
begin
  while lx.cur < lx.len do
  begin
    c := at(lx, lx.cur);
    case c of
      ' ', #9, #13, #10, #12, #11:
        lx.cur := lx.cur + 1;
      '{':
        begin
          skipBlockComment(lx, 1, '}');
          if not isActive(lx) then
            skipInactive(lx);
        end;
      '(':
        if at(lx, lx.cur + 1) = '*' then
        begin
          skipBlockComment(lx, 2, '*)');
          if not isActive(lx) then
            skipInactive(lx);
        end
        else
          exit;
      '/':
        if (lx.mode <> lmISO) and (at(lx, lx.cur + 1) = '/') then
          while (lx.cur < lx.len) and (at(lx, lx.cur) <> #10) do
            lx.cur := lx.cur + 1
        else
          exit;
    else
      exit;
    end;
  end;
end;

{ --- Identifiers and keywords --- }

procedure lexIdentifier(var lx: TLexer; var t: TToken);
var
  start, i: int64;
  lower: TBuf;
  name: PName;
  k: PKeyword;
  kind: TTokenKind;
begin
  start := lx.cur;
  while isIdentBody(at(lx, lx.cur)) do
    lx.cur := lx.cur + 1;
  bufInit(lower);
  for i := start to lx.cur - 1 do
    bufAddChar(lower, lowerChar(at(lx, i)));
  name := internChars(lower.data, 0, lower.len);
  bufFree(lower);
  kind := tkIdentifier;
  k := PKeyword(mapGet(keywordMap, name^.id));
  if (k <> nil) and (ord(lx.mode) >= k^.level) then
    kind := k^.kind;
  formToken(lx, t, kind, start);
  if kind = tkIdentifier then
    t.name := name;
end;

{ --- Numbers --- }

{ The value of the digits [start, stop) in radix (2, 8, 10 or 16), as 64
  unsigned bits. ok is false for an invalid digit or more than 64 bits
  (llvm::StringRef::getAsInteger). }
function parseUnsigned(const lx: TLexer; start, stop: int64; radix: longint;
                       var ok: boolean): int64;
var
  i: int64;
  d, bits, digitBits: longint;
  v: int64;
  c: char;
  significant: boolean;
  dec: string;
begin
  ok := stop > start;
  v := 0;
  { decimal: at most 2^64 - 1 = 18446744073709551615 }
  if radix = 10 then
  begin
    dec := '';
    for i := start to stop - 1 do
    begin
      c := at(lx, i);
      if not isDigit(c) then
        ok := false
      else if (dec <> '') or (c <> '0') then
        if length(dec) < 30 then
          dec := dec + c;
    end;
    if (length(dec) > 20) or
       ((length(dec) = 20) and (dec > '18446744073709551615')) then
      ok := false;
    {$Q-}{$R-}
    for i := 1 to length(dec) do
      v := v * 10 + (ord(dec[i]) - ord('0'));
    {$Q+}{$R+}
    if not ok then
      v := 0;
    exit(v);
  end;
  if radix = 16 then
    digitBits := 4
  else if radix = 8 then
    digitBits := 3
  else
    digitBits := 1;
  bits := 0;
  significant := false;
  for i := start to stop - 1 do
  begin
    c := at(lx, i);
    if isDigit(c) then
      d := ord(c) - ord('0')
    else if (c >= 'a') and (c <= 'z') then
      d := ord(c) - ord('a') + 10
    else if (c >= 'A') and (c <= 'Z') then
      d := ord(c) - ord('A') + 10
    else
      d := 99;
    if d >= radix then
    begin
      ok := false;
      continue;
    end;
    if significant then
      bits := bits + digitBits
    else if d <> 0 then
    begin
      significant := true;
      { bits of the leading digit }
      bits := 1;
      while (d shr bits) <> 0 do
        bits := bits + 1;
    end;
    v := (v shl digitBits) or d;
  end;
  if bits > 64 then
    ok := false;
  if not ok then
    v := 0;
  parseUnsigned := v;
end;

{ Does the real literal text (digits [. digits] [e [sign] digits]) round
  to infinity? Decided here because strtod's overflow traps when the
  compiler is built by Free Pascal (FPU overflow exceptions are on). The
  boundary is 2^1024 - 2^970, halfway between the largest double and 2^1024
  (the first 60 of its 309 digits are compared). }
function realOverflows(const text: string): boolean;
const
  limit = '179769313486231580793728971405303415079934132710037826936173';
var
  i, e, expo, intDigits: longint;
  digits: string;
  seenPoint, neg: boolean;
begin
  digits := '';
  intDigits := 0;
  seenPoint := false;
  i := 1;
  while (i <= length(text)) and (text[i] <> 'e') and (text[i] <> 'E') do
  begin
    if text[i] = '.' then
      seenPoint := true
    else if (digits <> '') or (text[i] <> '0') then
    begin
      if length(digits) < 80 then
        digits := digits + text[i];
      if not seenPoint then
        intDigits := intDigits + 1;
    end
    else if seenPoint then
      intDigits := intDigits - 1; { 0.00ddd: leading fraction zeros }
    i := i + 1;
  end;
  if digits = '' then
    exit(false);
  expo := 0;
  neg := false;
  if i <= length(text) then
  begin
    i := i + 1;
    if (i <= length(text)) and ((text[i] = '+') or (text[i] = '-')) then
    begin
      neg := text[i] = '-';
      i := i + 1;
    end;
    while i <= length(text) do
    begin
      if expo < 100000 then
        expo := expo * 10 + ord(text[i]) - ord('0');
      i := i + 1;
    end;
  end;
  if neg then
    expo := -expo;
  { the value is 0.digits * 10^e }
  e := intDigits + expo;
  if e <> 309 then
    exit(e > 309);
  while length(digits) < length(limit) do
    digits := digits + '0';
  realOverflows := copy(digits, 1, length(limit)) >= limit;
end;

procedure lexNumber(var lx: TLexer; var t: TToken);
var
  start, p: int64;
  isReal, hasSign, ok: boolean;
  v: int64;
  text: string;
  i: int64;
  r: double;
begin
  start := lx.cur;
  isReal := false;
  while isDigit(at(lx, lx.cur)) do
    lx.cur := lx.cur + 1;
  if (at(lx, lx.cur) = '.') and isDigit(at(lx, lx.cur + 1)) then
  begin
    isReal := true;
    lx.cur := lx.cur + 1;
    while isDigit(at(lx, lx.cur)) do
      lx.cur := lx.cur + 1;
  end;
  if (at(lx, lx.cur) = 'e') or (at(lx, lx.cur) = 'E') then
  begin
    p := lx.cur + 1;
    hasSign := (at(lx, p) = '+') or (at(lx, p) = '-');
    if hasSign then
      p := p + 1;
    if isDigit(at(lx, p)) then
    begin
      isReal := true;
      lx.cur := p;
      while isDigit(at(lx, lx.cur)) do
        lx.cur := lx.cur + 1;
    end
    else if hasSign then
    begin
      { "1e+": a broken exponent rather than "1 e + ..." }
      diagError(locOf(lx, p), 'expected digits in exponent');
      isReal := true;
      lx.cur := p;
    end;
    { otherwise "e" begins the next token }
  end;

  if not isReal then
  begin
    formToken(lx, t, tkIntegerLiteral, start);
    v := parseUnsigned(lx, start, lx.cur, 10, ok);
    if not ok then
      diagErrorR(locOf(lx, start), 'integer literal is too large', tokenRange(t));
    t.intValue := v;
    exit;
  end;

  formToken(lx, t, tkRealLiteral, start);
  text := '';
  for i := start to lx.cur - 1 do
    if length(text) < 255 then
      text := text + at(lx, i);
  { a malformed exponent ("1e+", reported above) is ignored, as by
    llvm::APFloat: the value of "1e+" is 1 }
  if not isDigit(text[length(text)]) then
    text := copy(text, 1, length(text) - 2);
  if realOverflows(text) then
  begin
    diagErrorR(locOf(lx, start), 'real literal is too large', tokenRange(t));
    r := sysInfinity;
  end
  else
    r := sysStrToReal(text, ok);
  t.realValue := r;
end;

{ $FF, %1010, &17 (Turbo/FPC). cur is after the prefix character. }
procedure lexBasedNumber(var lx: TLexer; var t: TToken; start: int64;
                         radix: longint);
var
  ok: boolean;
  v: int64;
begin
  while isAlnum(at(lx, lx.cur)) do
    lx.cur := lx.cur + 1;
  formToken(lx, t, tkIntegerLiteral, start);
  v := parseUnsigned(lx, start + 1, lx.cur, radix, ok);
  if not ok then
    diagErrorR(locOf(lx, start),
               'invalid base-' + intToStr(radix) + ' number', tokenRange(t));
  t.intValue := v;
  t.based := true;
end;

{ --- Strings: 'text' with '' for a quote; no line breaks. --mode=tp also
  takes character codes #13 / #$0D glued to quoted parts. --- }

procedure lexString(var lx: TLexer; var t: TToken);
var
  start, codeStart, digits: int64;
  value: TBuf;
  terminated, tp, ok: boolean;
  radix, parts, wideCode: longint;
  code: int64;
  c: char;
begin
  start := lx.cur;
  parts := 0;
  wideCode := 0;
  bufInit(value);
  terminated := true;
  tp := lx.mode <> lmISO;
  while terminated do
  begin
    if at(lx, lx.cur) = '''' then
    begin
      parts := parts + 1;
      lx.cur := lx.cur + 1;
      terminated := false;
      while lx.cur < lx.len do
      begin
        c := at(lx, lx.cur);
        if (c = #10) or (c = #13) then
          break;
        if c = '''' then
        begin
          if at(lx, lx.cur + 1) = '''' then
          begin
            bufAddChar(value, '''');
            lx.cur := lx.cur + 2;
            continue;
          end;
          lx.cur := lx.cur + 1;
          terminated := true;
          break;
        end;
        bufAddChar(value, c);
        lx.cur := lx.cur + 1;
      end;
    end
    else if tp and (at(lx, lx.cur) = '#') then
    begin
      codeStart := lx.cur;
      lx.cur := lx.cur + 1;
      parts := parts + 1;
      radix := 10;
      if at(lx, lx.cur) = '$' then
      begin
        radix := 16;
        lx.cur := lx.cur + 1;
      end;
      digits := lx.cur;
      while isAlnum(at(lx, lx.cur)) do
        lx.cur := lx.cur + 1;
      code := parseUnsigned(lx, digits, lx.cur, radix, ok);
      if not ok or (code > 65535) or (code < 0) then
      begin
        diagErrorR(locOf(lx, codeStart), 'invalid character code',
                   makeRange(locOf(lx, codeStart), locOf(lx, lx.cur)));
        code := 0;
      end;
      { A code above 255 (a widechar, Free Pascal): '?' in a string, as
        FPC's conversion to its one-byte strings gives. }
      if code > 255 then
      begin
        if parts = 1 then
          wideCode := longint(code)
        else
          wideCode := 0;
        bufAddChar(value, '?');
      end
      else
        bufAddChar(value, chr(longint(code)));
    end
    else
      break;
    if not tp then
      break; { ISO: one quoted part per token }
  end;

  formToken(lx, t, tkStringLiteral, start);
  t.strValue := internChars(value.data, 0, value.len);
  if parts = 1 then { a lone #code above 255 }
    t.wideChar := wideCode;
  if not terminated then
    diagError(locOf(lx, start), 'unterminated string literal')
  else if (value.len = 0) and (lx.mode = lmISO) then
    diagErrorR(locOf(lx, start), 'empty string literal is not allowed in ISO Pascal',
               tokenRange(t));
  bufFree(value);
end;

{ --- Main entry point --- }

function hexDigit(v: longint): char;
begin
  if v < 10 then
    hexDigit := chr(ord('0') + v)
  else
    hexDigit := chr(ord('A') + v - 10);
end;

procedure popMacroFrame(var lx: TLexer);
var
  f: PMacroFrame;
begin
  f := PMacroFrame(listPop(lx.macroStack));
  setBuffer(lx, f^.fid, f^.cur);
  lx.len := f^.len;
end;

{ A use of the macro named by the identifier at [start, lx.cur): lexing
  continues in its value; false if it is not a macro. }
function expandMacro(var lx: TLexer; start: int64): boolean;
var
  m: PMacro;
  f: PMacroFrame;
begin
  expandMacro := false;
  m := PMacro(mapGet(lx.macros, upperName(copyChars(lx, start, lx.cur))^.id));
  if m = nil then
    exit;
  { a macro used at the end of another: that one is done }
  while (lx.macroStack.count > 0) and (lx.cur >= lx.len) do
    popMacroFrame(lx);
  if lx.macroStack.count >= 16 then
  begin
    diagReport(sevWarning, locOf(lx, start), 'expanding macros exceeds a depth of 16',
               noRange, noRange);
    exit;
  end;
  f := PMacroFrame(arenaAlloc(sizeof(TMacroFrame)));
  f^.fid := lx.fid;
  f^.cur := lx.cur;
  f^.len := lx.len;
  listAdd(lx.macroStack, f);
  setBuffer(lx, m^.fid, m^.b);
  lx.len := m^.e;
  expandMacro := true;
end;

procedure lexNext(var lx: TLexer; var t: TToken);
var
  start: int64;
  c: char;
  k: TTokenKind;
  tp: boolean;
  frame: PIncludeFrame;
  shown: string;
begin
  tp := lx.mode <> lmISO;
  while true do
  begin
    skipWhitespaceAndComments(lx);
    start := lx.cur;
    if lx.cur >= lx.len then
    begin
      if lx.macroStack.count > 0 then { the end of a macro's value }
      begin
        popMacroFrame(lx);
        continue;
      end;
      closeConditionals(lx);
      if lx.includes.count > 0 then
      begin
        { back to the including file }
        frame := PIncludeFrame(listPop(lx.includes));
        setBuffer(lx, frame^.fid, frame^.cur);
        continue;
      end;
      formToken(lx, t, tkEof, start);
      exit;
    end;

    c := at(lx, lx.cur);
    if isIdentStart(c) then
    begin
      lexIdentifier(lx, t);
      if lx.macrosOn and (t.kind = tkIdentifier) and expandMacro(lx, start) then
        continue; { the macro's value instead }
      exit;
    end;
    if isDigit(c) then
    begin
      lexNumber(lx, t);
      exit;
    end;
    if (c = '''') or (tp and (c = '#')) then
    begin
      lexString(lx, t);
      exit;
    end;
    if tp and (((c = '$') and isHexDigit(at(lx, lx.cur + 1))) or
               ((c = '%') and ((at(lx, lx.cur + 1) = '0') or (at(lx, lx.cur + 1) = '1'))) or
               ((c = '&') and (at(lx, lx.cur + 1) >= '0') and (at(lx, lx.cur + 1) <= '7'))) then
    begin
      lx.cur := lx.cur + 1;
      if c = '$' then
        lexBasedNumber(lx, t, start, 16)
      else if c = '%' then
        lexBasedNumber(lx, t, start, 2)
      else
        lexBasedNumber(lx, t, start, 8);
      exit;
    end;

    lx.cur := lx.cur + 1;
    k := tkUnknown;
    case c of
      '+': k := tkPlus;
      '-': k := tkMinus;
      '*': k := tkStar;
      '/': k := tkSlash;
      '=': k := tkEqual;
      ';': k := tkSemi;
      ',': k := tkComma;
      ')': k := tkRParen;
      '[': k := tkLSquare;
      ']': k := tkRSquare;
      '^': k := tkCaret;
      '@':
        { ISO 7185 allows '@' as another spelling of '^' }
        if tp then
          k := tkAt
        else
          k := tkCaret;
      ':':
        begin
          k := tkColon;
          if at(lx, lx.cur) = '=' then
          begin
            lx.cur := lx.cur + 1;
            k := tkColonEqual;
          end;
        end;
      '<':
        begin
          k := tkLess;
          if at(lx, lx.cur) = '=' then
          begin
            lx.cur := lx.cur + 1;
            k := tkLessEqual;
          end
          else if at(lx, lx.cur) = '>' then
          begin
            lx.cur := lx.cur + 1;
            k := tkNotEqual;
          end;
        end;
      '>':
        begin
          k := tkGreater;
          if at(lx, lx.cur) = '=' then
          begin
            lx.cur := lx.cur + 1;
            k := tkGreaterEqual;
          end;
        end;
      '.':
        begin
          k := tkPeriod;
          if at(lx, lx.cur) = '.' then
          begin
            lx.cur := lx.cur + 1;
            k := tkDotdot;
          end
          else if at(lx, lx.cur) = ')' then
          begin
            lx.cur := lx.cur + 1;
            k := tkRSquare; { ".)" = "]" }
          end;
        end;
      '(':
        begin
          k := tkLParen;
          if at(lx, lx.cur) = '.' then
          begin
            lx.cur := lx.cur + 1;
            k := tkLSquare; { "(." = "[" }
          end;
        end;
    else
      ;
    end;

    { $coperators on: += -= *= /= (Free Pascal's C-style assignments) }
    if lx.cOperators and (at(lx, lx.cur) = '=') and
       (k in [tkPlus, tkMinus, tkStar, tkSlash]) then
    begin
      lx.cur := lx.cur + 1;
      case k of
        tkPlus: k := tkPlusEqual;
        tkMinus: k := tkMinusEqual;
        tkStar: k := tkStarEqual;
      else
        k := tkSlashEqual;
      end;
    end;

    if k <> tkUnknown then
    begin
      formToken(lx, t, k, start);
      exit;
    end;

    { an invalid character: report it, skip it, keep going }
    if (c >= ' ') and (c <= '~') then
      shown := c
    else
      shown := '\x' + hexDigit(ord(c) shr 4) + hexDigit(ord(c) and 15);
    diagErrorR(locOf(lx, start), 'invalid character ''' + shown + ''' in source',
               makeRange(locOf(lx, start), locOf(lx, lx.cur)));
  end;
end;

end.
