{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
unit pcdiag;
{ Source files and diagnostics of the self-hosted compiler; the Pascal
  counterpart of SourceManager.h and Diagnostics.h in include/p3/Basic.

  Locations: every loaded file occupies a range of one global location
  space (like clang): file k covers start(k) .. start(k) + len(k), the last
  value being its end-of-file position. Location 0 is "no location".

  Output must match the C++ compiler byte for byte, which prints through
  llvm::SMDiagnostic (clang style):

    hello.pas:3:5: error: undeclared identifier 'x'
        x := 1;
        ^

  Tabs in the source line expand to 8 columns; a line with non-ASCII bytes
  is printed without a caret line. Diagnostics without a location print as
  "p3: error: ...". Colors are not supported yet. }

interface

uses pcsys, pcutil;

type
  TLoc = longint;

  TRange = record
    b, e: TLoc; { [b, e); both 0 for no range }
  end;

  TSeverity = (sevNote, sevRemark, sevWarning, sevError, sevFatal);

const
  noLoc = 0;

var
  { Where diagnostics go (default: standard error). }
  diagStream: pointer;
  diagProgName: string;

{ Source manager }
function smAddBuffer(const name: string; data: PCharArray; len: int64): longint;
function smLoadFile(const path: string; var fid: longint): boolean;
function smNumFiles: longint;
function smFileName(fid: longint): string;
function smFileData(fid: longint): PCharArray;
function smFileLen(fid: longint): int64;
function smFileStart(fid: longint): TLoc;
{ The file containing loc, or 0. }
function smFileOf(loc: TLoc): longint;
{ 1-based line and column of loc (as clang: the column counts bytes from
  the last line break); line = 0 for no location. }
procedure smPresumed(loc: TLoc; var fid, line, col: longint);

function makeRange(b, e: TLoc): TRange;
function noRange: TRange;

{ Diagnostics }
procedure diagInit;
procedure diagSetErrorLimit(n: longint);
procedure diagSetWarningsAsErrors(enabled: boolean);
procedure diagDisableWarning(const group: string);
function diagWarningEnabled(const group: string): boolean;

procedure diagReport(sev: TSeverity; loc: TLoc; const msg: string;
                     r1, r2: TRange);
procedure diagReportBuf(sev: TSeverity; loc: TLoc; const msg: TBuf;
                        r1, r2: TRange);
procedure diagError(loc: TLoc; const msg: string);
procedure diagErrorR(loc: TLoc; const msg: string; r: TRange);
procedure diagErrorRR(loc: TLoc; const msg: string; r1, r2: TRange);
procedure diagWarning(loc: TLoc; const msg: string);
procedure diagWarningR(loc: TLoc; const msg: string; r: TRange);
{ A warning in a named group, "... [-W<group>]", unless the group is off. }
procedure diagWarningGroupR(const group: string; loc: TLoc; const msg: string;
                            r: TRange);
procedure diagWarningGroupBufR(const group: string; loc: TLoc; var msg: TBuf;
                               r: TRange);
procedure diagNote(loc: TLoc; const msg: string);
procedure diagNoteR(loc: TLoc; const msg: string; r: TRange);
procedure diagFatal(loc: TLoc; const msg: string);

{ Quiet mode: diagnostics are dropped (and not counted) until diagQuietEnd,
  which returns how many errors were dropped. A file parsed quietly can be
  parsed again, to be reported on. }
procedure diagQuietBegin;
function diagQuietEnd: longint;

{ Drop a warning given already (same file, line, column and message): the
  compiler parses again after computing compiletime constants. }
procedure diagSetSkipRepeatedWarnings(enabled: boolean);

function diagNumErrors: longint;
function diagNumWarnings: longint;
function diagHasErrors: boolean;
{ True once a fatal error occurred or the error limit was reached. }
function diagShouldStop: boolean;

implementation

const
  tabStop = 8;

type
  TLineArray = array[0..$FFFFFFF] of int64;
  PLineArray = ^TLineArray;

  PSourceFile = ^TSourceFile;
  TSourceFile = record
    name: string;
    data: PCharArray;   { len bytes followed by #0 }
    len: int64;
    start: TLoc;
    lineStarts: PLineArray; { offset of each line, computed on first use }
    lineCount: longint;
  end;

var
  files: TPtrList;
  nextStart: TLoc;
  errorLimit: longint;
  warningsAsErrors: boolean;
  disabledGroups: TPtrList; { of PName }
  numErrors, numWarnings: longint;
  fatalOccurred: boolean;
  lastDiagSuppressed: boolean;
  quietMode: boolean;
  quietErrors: longint;
  warningsSeen: TIntMap; { name ids of file:line:col:message }
  skipRepeatedWarnings: boolean;

{ --- Source manager --- }

function getFile(fid: longint): PSourceFile;
begin
  getFile := PSourceFile(listGet(files, fid - 1));
end;

function smAddBuffer(const name: string; data: PCharArray; len: int64): longint;
var
  f: PSourceFile;
begin
  if files.cap = 0 then
    listInit(files);
  if nextStart = 0 then
    nextStart := 1;
  f := PSourceFile(arenaAlloc(sizeof(TSourceFile)));
  f^.name := name;
  f^.data := data;
  f^.len := len;
  f^.start := nextStart;
  f^.lineStarts := nil;
  f^.lineCount := 0;
  nextStart := nextStart + TLoc(len) + 1;
  listAdd(files, f);
  smAddBuffer := files.count;
end;

function smLoadFile(const path: string; var fid: longint): boolean;
var
  data: PCharArray;
  len: int64;
begin
  fid := 0;
  if not sysReadFile(path, data, len) then
    exit(false);
  fid := smAddBuffer(path, data, len);
  smLoadFile := true;
end;

function smNumFiles: longint;
begin
  smNumFiles := files.count;
end;

function smFileName(fid: longint): string;
begin
  smFileName := getFile(fid)^.name;
end;

function smFileData(fid: longint): PCharArray;
begin
  smFileData := getFile(fid)^.data;
end;

function smFileLen(fid: longint): int64;
begin
  smFileLen := getFile(fid)^.len;
end;

function smFileStart(fid: longint): TLoc;
begin
  smFileStart := getFile(fid)^.start;
end;

function smFileOf(loc: TLoc): longint;
var
  lo, hi, mid: longint;
  f: PSourceFile;
begin
  smFileOf := 0;
  if loc <= 0 then
    exit;
  { files are in increasing start order: binary search }
  lo := 1;
  hi := files.count;
  while lo <= hi do
  begin
    mid := (lo + hi) div 2;
    f := getFile(mid);
    if loc < f^.start then
      hi := mid - 1
    else if loc > f^.start + f^.len then
      lo := mid + 1
    else
      exit(mid);
  end;
end;

procedure computeLines(f: PSourceFile);
var
  count, cap: longint;
  i: int64;
begin
  cap := 1024;
  getmem(f^.lineStarts, int64(cap) * sizeof(int64));
  count := 1;
  {$R-}
  f^.lineStarts^[0] := 0;
  for i := 0 to f^.len - 1 do
    if f^.data^[i] = #10 then
    begin
      if count = cap then
      begin
        cap := cap * 2;
        reallocmem(f^.lineStarts, int64(cap) * sizeof(int64));
      end;
      f^.lineStarts^[count] := i + 1;
      count := count + 1;
    end;
  {$R+}
  f^.lineCount := count;
end;

function isLineBreak(c: char): boolean;
begin
  isLineBreak := (c = #10) or (c = #13);
end;

procedure smPresumed(loc: TLoc; var fid, line, col: longint);
var
  f: PSourceFile;
  off, p: int64;
  lo, hi, mid: longint;
begin
  fid := smFileOf(loc);
  line := 0;
  col := 0;
  if fid = 0 then
    exit;
  f := getFile(fid);
  if f^.lineStarts = nil then
    computeLines(f);
  off := loc - f^.start;
  { line: the last line start <= off ('\n' ends a line) }
  lo := 0;
  hi := f^.lineCount - 1;
  {$R-}
  while lo < hi do
  begin
    mid := (lo + hi + 1) div 2;
    if f^.lineStarts^[mid] <= off then
      lo := mid
    else
      hi := mid - 1;
  end;
  {$R+}
  line := lo + 1;
  { column: bytes since the last '\n', as LLVM 23's SourceMgr counts it }
  p := off;
  {$R-}
  while (p > 0) and (f^.data^[p - 1] <> #10) do
    p := p - 1;
  {$R+}
  col := longint(off - p) + 1;
end;

function makeRange(b, e: TLoc): TRange;
var
  r: TRange;
begin
  r.b := b;
  r.e := e;
  makeRange := r;
end;

function noRange: TRange;
begin
  noRange := makeRange(0, 0);
end;

{ --- Diagnostics --- }

procedure diagInit;
begin
  diagStream := sysErr;
  diagProgName := 'p3';
  errorLimit := 0;
  warningsAsErrors := false;
  listInit(disabledGroups);
  numErrors := 0;
  numWarnings := 0;
  fatalOccurred := false;
  lastDiagSuppressed := false;
  mapInit(warningsSeen);
  skipRepeatedWarnings := false;
end;

procedure diagSetErrorLimit(n: longint);
begin
  errorLimit := n;
end;

procedure diagSetWarningsAsErrors(enabled: boolean);
begin
  warningsAsErrors := enabled;
end;

procedure diagDisableWarning(const group: string);
begin
  listAdd(disabledGroups, intern(group));
end;

function diagWarningEnabled(const group: string): boolean;
var
  n: PName;
  i: longint;
begin
  n := intern(group);
  for i := 0 to disabledGroups.count - 1 do
    if listGet(disabledGroups, i) = pointer(n) then
      exit(false);
  diagWarningEnabled := true;
end;

{ Appends line [lineStart, lineEnd) of data to outText, tabs expanded. }
procedure addSourceLine(var outText: TBuf; data: PCharArray;
                        lineStart, lineEnd: int64);
var
  i: int64;
  outCol: longint;
  c: char;
begin
  outCol := 0;
  {$R-}
  for i := lineStart to lineEnd - 1 do
  begin
    c := data^[i];
    if c = #9 then
      repeat
        bufAddChar(outText, ' ');
        outCol := outCol + 1;
      until outCol mod tabStop = 0
    else
    begin
      bufAddChar(outText, c);
      outCol := outCol + 1;
    end;
  end;
  {$R+}
  bufAddChar(outText, #10);
end;

{ The source line of loc and a caret line under it (llvm::SMDiagnostic). }
procedure addExcerpt(var outText: TBuf; loc: TLoc; r1, r2: TRange);
var
  f: PSourceFile;
  data: PCharArray;
  off, lineStart, lineEnd, numColumns, i, colNo: int64;
  caret: TBuf;
  outCol: longint;
  rb, re: TLoc;
  k: longint;
  r: TRange;
begin
  f := getFile(smFileOf(loc));
  data := f^.data;
  off := loc - f^.start;
  lineStart := off;
  {$R-}
  while (lineStart > 0) and not isLineBreak(data^[lineStart - 1]) do
    lineStart := lineStart - 1;
  lineEnd := off;
  while (lineEnd < f^.len) and not isLineBreak(data^[lineEnd]) do
    lineEnd := lineEnd + 1;
  { lines with non-ASCII bytes get no caret line }
  for i := lineStart to lineEnd - 1 do
    if ord(data^[i]) >= 128 then
    begin
      addSourceLine(outText, data, lineStart, lineEnd);
      exit;
    end;
  {$R+}
  numColumns := lineEnd - lineStart;
  bufInit(caret);
  for i := 0 to numColumns do
    bufAddChar(caret, ' ');
  { ranges, clipped to this line }
  for k := 1 to 2 do
  begin
    if k = 1 then
      r := r1
    else
      r := r2;
    if (r.b = 0) or (r.e = 0) then
      continue;
    rb := r.b - f^.start;
    re := r.e - f^.start;
    if (rb > lineEnd) or (re < lineStart) then
      continue;
    if rb < lineStart then
      rb := TLoc(lineStart);
    if re > lineEnd then
      re := TLoc(lineEnd);
    {$R-}
    for i := rb - lineStart to re - lineStart - 1 do
      if i < caret.len then
        caret.data^[i] := '~';
    {$R+}
  end;
  { the caret is at the column of the message's header, counted from the
    last '\n' even when a bare '\r' starts the line shown (as LLVM 23) }
  colNo := off;
  {$R-}
  while (colNo > 0) and (data^[colNo - 1] <> #10) do
    colNo := colNo - 1;
  colNo := off - colNo;
  if colNo <= numColumns then
    caret.data^[colNo] := '^'
  else
    caret.data^[numColumns] := '^';
  {$R+}
  { no trailing blanks }
  while (caret.len > 0) and (bufCharAt(caret, caret.len - 1) = ' ') do
    caret.len := caret.len - 1;
  addSourceLine(outText, data, lineStart, lineEnd);
  { the caret line, with a tab where the source line has one }
  outCol := 0;
  {$R-}
  for i := 0 to caret.len - 1 do
    if (i >= numColumns) or (data^[lineStart + i] <> #9) then
    begin
      bufAddChar(outText, caret.data^[i]);
      outCol := outCol + 1;
    end
    else
      repeat
        bufAddChar(outText, caret.data^[i]);
        outCol := outCol + 1;
      until outCol mod tabStop = 0;
  {$R+}
  bufAddChar(outText, #10);
  bufFree(caret);
end;

{ "file:line:col: ". SMDiagnostic shows standard input ("-") as <stdin>;
  fatal errors print the name as it is. }
procedure addPosition(var outText: TBuf; loc: TLoc; mapStdin: boolean);
var
  fid, line, col: longint;
  name: string;
begin
  smPresumed(loc, fid, line, col);
  name := smFileName(fid);
  if mapStdin and (name = '-') then
    name := '<stdin>';
  bufAdd(outText, name);
  bufAddChar(outText, ':');
  bufAddInt(outText, line);
  bufAddChar(outText, ':');
  bufAddInt(outText, col);
  bufAdd(outText, ': ');
end;

procedure emit(sev: TSeverity; loc: TLoc; const msg: TBuf; r1, r2: TRange);
var
  outText: TBuf;
  valid: boolean;
begin
  bufInit(outText);
  valid := smFileOf(loc) <> 0;
  if valid then
    addPosition(outText, loc, sev <> sevFatal)
  else
  begin
    bufAdd(outText, diagProgName);
    bufAdd(outText, ': ');
  end;
  case sev of
    sevNote: bufAdd(outText, 'note: ');
    sevRemark: bufAdd(outText, 'remark: ');
    sevWarning: bufAdd(outText, 'warning: ');
    sevError: bufAdd(outText, 'error: ');
    sevFatal: bufAdd(outText, 'fatal error: ');
  end;
  bufAddChars(outText, msg.data, 0, msg.len);
  bufAddChar(outText, #10);
  if valid and (sev <> sevFatal) then
    addExcerpt(outText, loc, r1, r2);
  bufWrite(outText, diagStream);
  bufFree(outText);
end;

procedure emitTooManyErrors;
var
  msg: TBuf;
begin
  fatalOccurred := true;
  bufInit(msg);
  bufAdd(msg, 'too many errors emitted, stopping now [-ferror-limit=]');
  emit(sevFatal, noLoc, msg, noRange, noRange);
  bufFree(msg);
end;

{ Records warning msg at loc: false if it was given already. }
function noteWarning(loc: TLoc; const msg: TBuf): boolean;
var
  key: string;
  fid, line, col, i: longint;
  k: PName;
begin
  key := '';
  smPresumed(loc, fid, line, col);
  if line > 0 then
    key := smFileName(fid) + ':' + intToStr(line) + ':' + intToStr(col) + ':';
  for i := 0 to msg.len - 1 do
    key := key + msg.data^[i];
  k := intern(key);
  if mapHas(warningsSeen, k^.id) then
    exit(false);
  mapPut(warningsSeen, k^.id, k);
  noteWarning := true;
end;

procedure diagSetSkipRepeatedWarnings(enabled: boolean);
begin
  skipRepeatedWarnings := enabled;
end;

procedure diagReportBuf(sev: TSeverity; loc: TLoc; const msg: TBuf;
                        r1, r2: TRange);
begin
  if quietMode then
  begin
    if (sev = sevError) or (sev = sevFatal) then
      quietErrors := quietErrors + 1;
    exit;
  end;
  { after a fatal error or the error limit, stay quiet }
  if fatalOccurred then
  begin
    lastDiagSuppressed := true;
    exit;
  end;
  { notes belong to the diagnostic before them }
  if sev = sevNote then
  begin
    if not lastDiagSuppressed then
      emit(sev, loc, msg, r1, r2);
    exit;
  end;
  if sev = sevWarning then
    if not noteWarning(loc, msg) and skipRepeatedWarnings then
    begin
      lastDiagSuppressed := true;
      exit;
    end;
  if (sev = sevWarning) and warningsAsErrors then
    sev := sevError;
  lastDiagSuppressed := false;
  case sev of
    sevWarning:
      numWarnings := numWarnings + 1;
    sevError:
      begin
        if (errorLimit > 0) and (numErrors >= errorLimit) then
        begin
          emitTooManyErrors;
          lastDiagSuppressed := true;
          exit;
        end;
        numErrors := numErrors + 1;
      end;
    sevFatal:
      begin
        numErrors := numErrors + 1;
        fatalOccurred := true;
      end;
  else
    ;
  end;
  emit(sev, loc, msg, r1, r2);
end;

procedure diagQuietBegin;
begin
  quietMode := true;
  quietErrors := 0;
end;

function diagQuietEnd: longint;
begin
  quietMode := false;
  diagQuietEnd := quietErrors;
end;

procedure diagReport(sev: TSeverity; loc: TLoc; const msg: string;
                     r1, r2: TRange);
var
  b: TBuf;
begin
  bufInit(b);
  bufAdd(b, msg);
  diagReportBuf(sev, loc, b, r1, r2);
  bufFree(b);
end;

procedure diagError(loc: TLoc; const msg: string);
begin
  diagReport(sevError, loc, msg, noRange, noRange);
end;

procedure diagErrorR(loc: TLoc; const msg: string; r: TRange);
begin
  diagReport(sevError, loc, msg, r, noRange);
end;

procedure diagErrorRR(loc: TLoc; const msg: string; r1, r2: TRange);
begin
  diagReport(sevError, loc, msg, r1, r2);
end;

procedure diagWarning(loc: TLoc; const msg: string);
begin
  diagReport(sevWarning, loc, msg, noRange, noRange);
end;

procedure diagWarningR(loc: TLoc; const msg: string; r: TRange);
begin
  diagReport(sevWarning, loc, msg, r, noRange);
end;

procedure diagWarningGroupR(const group: string; loc: TLoc; const msg: string;
                            r: TRange);
var
  b: TBuf;
begin
  if not diagWarningEnabled(group) then
    exit;
  bufInit(b);
  bufAdd(b, msg);
  bufAdd(b, ' [-W' + group + ']');
  diagReportBuf(sevWarning, loc, b, r, noRange);
  bufFree(b);
end;

procedure diagWarningGroupBufR(const group: string; loc: TLoc; var msg: TBuf;
                               r: TRange);
begin
  if not diagWarningEnabled(group) then
    exit;
  bufAdd(msg, ' [-W' + group + ']');
  diagReportBuf(sevWarning, loc, msg, r, noRange);
end;

procedure diagNote(loc: TLoc; const msg: string);
begin
  diagReport(sevNote, loc, msg, noRange, noRange);
end;

procedure diagNoteR(loc: TLoc; const msg: string; r: TRange);
begin
  diagReport(sevNote, loc, msg, r, noRange);
end;

procedure diagFatal(loc: TLoc; const msg: string);
begin
  diagReport(sevFatal, loc, msg, noRange, noRange);
end;

function diagNumErrors: longint;
begin
  diagNumErrors := numErrors;
end;

function diagNumWarnings: longint;
begin
  diagNumWarnings := numWarnings;
end;

function diagHasErrors: boolean;
begin
  diagHasErrors := (numErrors <> 0) or fatalOccurred;
end;

function diagShouldStop: boolean;
begin
  diagShouldStop := fatalOccurred;
end;

end.
