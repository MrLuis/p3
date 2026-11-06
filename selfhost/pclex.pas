unit pclex;
{ The lexer of the self-hosted compiler; a port of src/lexer/Lexer.cpp.
  Token kinds come from include/pcc/Lexer/TokenKinds.def through the
  generated tokens.inc.

  ISO 7185 tokens plus, with --mode=tp, Turbo/FPC ones: // comments,
  $hex %bin &oct numbers, #13 character codes glued to strings, '@'.
  Comments starting with '$' are directives: R+/R- and Q+/Q- switch
  runtime checks (recorded for code generation), I file includes a file. }

interface

uses pcsys, pcutil, pcdiag;

{$I tokens.inc}

type
  TLangMode = (lmISO, lmTP);

  TToken = record
    kind: TTokenKind;
    loc: TLoc;
    len: longint;      { bytes of source text }
    name: PName;       { identifiers: the lower-case name }
    strValue: PName;   { string literals: the value, quotes removed }
    intValue: int64;   { integer literals: 64 bits, read as unsigned }
    realValue: double; { real literals }
  end;

  TCheckSwitch = (swRange, swOverflow);

  PIncludeFrame = ^TIncludeFrame;
  TIncludeFrame = record
    fid: longint;
    cur: int64;
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
  end;

var
  { -I directories, searched for included files after the including
    file's directory. }
  includeDirs: TPtrList; { of PName }

procedure lexInit(var lx: TLexer; fid: longint; mode: TLangMode);
procedure lexNext(var lx: TLexer; var t: TToken);

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
  TDirective = record
    loc: TLoc;
    fid: longint;
    sw: TCheckSwitch;
    isOn: boolean;
  end;

var
  keywordMap: TIntMap; { name id -> PKeyword }
  keywordsReady: boolean;
  directives: TPtrList; { of PDirective, in source order }

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

procedure lexInit(var lx: TLexer; fid: longint; mode: TLangMode);
begin
  if not keywordsReady then
    initKeywords;
  lx.mode := mode;
  setBuffer(lx, fid, 0);
  listInit(lx.includes);
  lx.hasPendingInclude := false;
  bufInit(lx.pendingInclude);
  lx.pendingIncludeLoc := noLoc;
end;

procedure formToken(var lx: TLexer; var t: TToken; k: TTokenKind; start: int64);
begin
  t.kind := k;
  t.loc := locOf(lx, start);
  t.len := longint(lx.cur - start);
  t.name := nil;
  t.strValue := nil;
  t.intValue := 0;
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

procedure addDirective(const lx: TLexer; loc: TLoc; sw: TCheckSwitch; isOn: boolean);
var
  d: PDirective;
begin
  d := PDirective(arenaAlloc(sizeof(TDirective)));
  d^.loc := loc;
  d^.fid := lx.fid;
  d^.sw := sw;
  d^.isOn := isOn;
  listAdd(directives, d);
end;

function checkEnabled(sw: TCheckSwitch; loc: TLoc; default: boolean): boolean;
var
  i, fid: longint;
  d: PDirective;
  isOn: boolean;
begin
  isOn := default;
  fid := smFileOf(loc);
  for i := 0 to directives.count - 1 do
  begin
    d := PDirective(listGet(directives, i));
    if (d^.sw = sw) and (fid <> 0) and (d^.fid = fid) and (d^.loc <= loc) then
      isOn := d^.isOn;
  end;
  checkEnabled := isOn;
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

{ Directive text [textStart, textEnd) (after the '$', up to the comment
  closer); start is the offset of the comment opener. The text can be long
  and span lines, so it is handled as offsets, not as a string. }
procedure handleDirective(var lx: TLexer; start: int64; textStart, textEnd: int64);
var
  p, b, e, partEnd: int64;
  word: string;
  msg: TBuf;
  c: char;
begin
  { $I file / $INCLUDE file }
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
        skipBlockComment(lx, 1, '}');
      '(':
        if at(lx, lx.cur + 1) = '*' then
          skipBlockComment(lx, 2, '*)')
        else
          exit;
      '/':
        if (lx.mode = lmTP) and (at(lx, lx.cur + 1) = '/') then
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
  if (k <> nil) and (not k^.tpOnly or (lx.mode = lmTP)) then
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
end;

{ --- Strings: 'text' with '' for a quote; no line breaks. --mode=tp also
  takes character codes #13 / #$0D glued to quoted parts. --- }

procedure lexString(var lx: TLexer; var t: TToken);
var
  start, codeStart, digits: int64;
  value: TBuf;
  terminated, tp, ok: boolean;
  radix: longint;
  code: int64;
  c: char;
begin
  start := lx.cur;
  bufInit(value);
  terminated := true;
  tp := lx.mode = lmTP;
  while terminated do
  begin
    if at(lx, lx.cur) = '''' then
    begin
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
      if not ok or (code > 255) or (code < 0) then
      begin
        diagErrorR(locOf(lx, codeStart), 'invalid character code',
                   makeRange(locOf(lx, codeStart), locOf(lx, lx.cur)));
        code := 0;
      end;
      bufAddChar(value, chr(longint(code)));
    end
    else
      break;
    if not tp then
      break; { ISO: one quoted part per token }
  end;

  formToken(lx, t, tkStringLiteral, start);
  t.strValue := internChars(value.data, 0, value.len);
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

procedure lexNext(var lx: TLexer; var t: TToken);
var
  start: int64;
  c: char;
  k: TTokenKind;
  tp: boolean;
  frame: PIncludeFrame;
  shown: string;
begin
  tp := lx.mode = lmTP;
  while true do
  begin
    skipWhitespaceAndComments(lx);
    start := lx.cur;
    if lx.cur >= lx.len then
    begin
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
