{ p3's SysUtils: Free Pascal's names and results, on ansistrings. Written
  in the Turbo dialect so that every mode with units can use it. In --mode=objfpc conversion failures
  raise EConvertError (predeclared, as Exception is); in --mode=tp, where
  there are no exceptions, they are runtime errors. The two-parameter
  BoolToStr needs --mode=objfpc (overloading). What differs between
  operating systems is in the runtime (runtime/sys.c). }
unit sysutils;

{$H+}

interface

const
{$IFDEF MSWINDOWS}
  PathDelim = '\';
  DirectorySeparator = '\';
  DriveDelim = ':';
  DriveSeparator = ':';
  PathSep = ';';
  PathSeparator = ';';
  LineEnding = #13#10;
  sLineBreak = #13#10;
{$ELSE}
  PathDelim = '/';
  DirectorySeparator = '/';
  DriveDelim = '';
  DriveSeparator = '';
  PathSep = ':';
  PathSeparator = ':';
  LineEnding = #10;
  sLineBreak = #10;
{$ENDIF}

type
  TSysCharSet = set of char;
  TReplaceFlag = (rfReplaceAll, rfIgnoreCase);
  TReplaceFlags = set of TReplaceFlag;
  TDateTime = double;

{ integers }
function IntToStr(value: int64): string;
{$IFDEF P3_OBJFPC}
function IntToStr(value: qword): string;
{$ENDIF}
function UIntToStr(value: qword): string;
function IntToHex(value: int64; digits: longint): string;
{$IFDEF P3_OBJFPC}
function IntToHex(value: longint; digits: longint): string;
{$ENDIF}
function StrToInt(const s: string): longint;
function StrToInt64(const s: string): int64;
function StrToIntDef(const s: string; def: longint): longint;
function StrToInt64Def(const s: string; def: int64): int64;
function TryStrToInt(const s: string; var value: longint): boolean;
function TryStrToInt64(const s: string; var value: int64): boolean;
function StrToQWord(const s: string): qword;
function StrToQWordDef(const s: string; def: qword): qword;
function TryStrToQWord(const s: string; var value: qword): boolean;

{ floating point }
function FloatToStr(value: double): string;
function StrToFloat(const s: string): double;
function StrToFloatDef(const s: string; def: double): double;
function TryStrToFloat(const s: string; var value: double): boolean;

{ booleans }
function BoolToStr(b: boolean): string;
{$IFDEF P3_OBJFPC}
function BoolToStr(b, useBoolStrs: boolean): string;
{$ENDIF}

{ strings }
function Trim(const s: string): string;
function TrimLeft(const s: string): string;
function TrimRight(const s: string): string;
function UpperCase(const s: string): string;
function LowerCase(const s: string): string;
function CompareStr(const a, b: string): longint;
function CompareText(const a, b: string): longint;
function SameText(const a, b: string): boolean;
function QuotedStr(const s: string): string;
function StringReplace(const s, from, into: string; flags: TReplaceFlags): string;
function IsDelimiter(const delimiters, s: string; index: longint): boolean;
function LastDelimiter(const delimiters, s: string): longint;
function StrPas(p: pchar): string;
function Format(const fmt: string; const args: array of const): string;

{ file names }
function ExtractFilePath(const name: string): string;
function ExtractFileDir(const name: string): string;
function ExtractFileName(const name: string): string;
function ExtractFileExt(const name: string): string;
function ChangeFileExt(const name, ext: string): string;
function IncludeTrailingPathDelimiter(const path: string): string;
function ExcludeTrailingPathDelimiter(const path: string): string;

{ files and directories }
function FileExists(const name: string): boolean;
function DirectoryExists(const name: string): boolean;
function DeleteFile(const name: string): boolean;
function RenameFile(const oldName, newName: string): boolean;
function CreateDir(const name: string): boolean;
function RemoveDir(const name: string): boolean;
function ForceDirectories(const name: string): boolean;
function GetCurrentDir: string;
function SetCurrentDir(const name: string): boolean;
function GetEnvironmentVariable(const name: string): string;

{ dates and times }
function EncodeDate(year, month, day: word): TDateTime;
function EncodeTime(hour, minute, second, ms: word): TDateTime;
procedure DecodeDate(d: TDateTime; var year, month, day: word);
procedure DecodeTime(d: TDateTime; var hour, minute, second, ms: word);
function IsLeapYear(year: word): boolean;
function DayOfWeek(d: TDateTime): longint;
function Now: TDateTime;
function Date: TDateTime;
function Time: TDateTime;
function GetTickCount64: int64;
procedure Sleep(ms: longint);

implementation

type
  TTimeFields = array[0..6] of longint;

function c_getenv(name: pchar): pchar; cdecl; external name 'getenv';
function c_rename(a, b: pchar): longint; cdecl; external name 'rename';
procedure rt_error(msg: pchar); cdecl; external name '__pas_runtime_error';
function sys_file_exists(p: pchar): longint; cdecl; external name '__pas_sys_file_exists';
function sys_dir_exists(p: pchar): longint; cdecl; external name '__pas_sys_dir_exists';
function sys_mkdir(p: pchar): longint; cdecl; external name '__pas_sys_mkdir';
function sys_rmdir(p: pchar): longint; cdecl; external name '__pas_sys_rmdir';
function sys_chdir(p: pchar): longint; cdecl; external name '__pas_sys_chdir';
function sys_unlink(p: pchar): longint; cdecl; external name '__pas_sys_unlink';
function sys_getcwd(buf: pchar; size: longint): longint; cdecl; external name '__pas_sys_getcwd';
procedure sys_format_double(buf: pchar; size: longint; value: double; precision: longint); cdecl; external name '__pas_sys_format_double';
function sys_parse_double(s: pchar; var value: double): longint; cdecl; external name '__pas_sys_parse_double';
procedure sys_localtime(var fields: TTimeFields); cdecl; external name '__pas_sys_localtime';
function sys_tick_ms: int64; cdecl; external name '__pas_sys_tick_ms';
procedure sys_sleep(ms: longint); cdecl; external name '__pas_sys_sleep';

procedure convertError(const msg: string);
{$IFDEF P3_OBJFPC}
begin
  raise EConvertError.Create(msg);
end;
{$ELSE}
begin
  rt_error(pchar(msg));
end;
{$ENDIF}

function IntToStr(value: int64): string;
var
  r: string;
  digit: longint;
  negative: boolean;
begin
  negative := value < 0;
  r := '';
  repeat
    { on the negative side, so that the lowest int64 works }
    digit := value mod 10;
    if digit < 0 then
      digit := -digit;
    r := chr(ord('0') + digit) + r;
    value := value div 10;
  until value = 0;
  if negative then
    r := '-' + r;
  IntToStr := r;
end;

function UIntToStr(value: qword): string;
var
  r: string;
begin
  r := '';
  repeat
    r := chr(ord('0') + value mod 10) + r;
    value := value div 10;
  until value = 0;
  UIntToStr := r;
end;

{$IFDEF P3_OBJFPC}
function IntToStr(value: qword): string;
begin
  IntToStr := UIntToStr(value);
end;
{$ENDIF}

{ digits is a minimum: the value's own digits are never cut }
function IntToHex(value: int64; digits: longint): string;
const
  hex: string[16] = '0123456789ABCDEF';
var
  r: string;
  i, used: longint;
begin
  used := 16;
  while (used > 1) and (((value shr (4 * (used - 1))) and 15) = 0) do
    used := used - 1;
  r := '';
  for i := used + 1 to digits do
    r := r + '0';
  for i := used - 1 downto 0 do
    r := r + hex[((value shr (4 * i)) and 15) + 1];
  IntToHex := r;
end;

{$IFDEF P3_OBJFPC}
function IntToHex(value: longint; digits: longint): string;
var
  v: int64;
begin
  v := value;
  IntToHex := IntToHex(v and $FFFFFFFF, digits);
end;
{$ENDIF}

function digitValue(c: char): longint;
begin
  if (c >= '0') and (c <= '9') then
    digitValue := ord(c) - ord('0')
  else if (c >= 'a') and (c <= 'f') then
    digitValue := ord(c) - ord('a') + 10
  else if (c >= 'A') and (c <= 'F') then
    digitValue := ord(c) - ord('A') + 10
  else
    digitValue := 99;
end;

{ As Free Pascal's Val: leading blanks, a sign, then decimal digits or a
  $ / 0x (hexadecimal), & (octal) or % (binary) prefix. The value is built
  on the negative side, so that the lowest int64 works. }
function parseInt(const s: string; var value: int64): boolean;
const
  lowest = -9223372036854775807 - 1;
var
  i, base, d: longint;
  negative: boolean;
  v: int64;
begin
  parseInt := false;
  i := 1;
  while (i <= length(s)) and (s[i] = ' ') do
    i := i + 1;
  negative := false;
  if (i <= length(s)) and ((s[i] = '-') or (s[i] = '+')) then
  begin
    negative := s[i] = '-';
    i := i + 1;
  end;
  base := 10;
  if i <= length(s) then
    if s[i] = '$' then
    begin
      base := 16;
      i := i + 1;
    end
    else if s[i] = '&' then
    begin
      base := 8;
      i := i + 1;
    end
    else if s[i] = '%' then
    begin
      base := 2;
      i := i + 1;
    end
    else if (s[i] = '0') and (i < length(s)) and ((s[i + 1] = 'x') or (s[i + 1] = 'X')) then
    begin
      base := 16;
      i := i + 2;
    end;
  if i > length(s) then
    exit;
  v := 0;
  while i <= length(s) do
  begin
    d := digitValue(s[i]);
    if d >= base then
      exit;
    if v < (lowest + d) div base then
      exit;
    v := v * base - d;
    i := i + 1;
  end;
  if negative then
    value := v
  else
  begin
    if v = lowest then
      exit;
    value := -v;
  end;
  parseInt := true;
end;

{ As in Free Pascal a failed conversion leaves 0. Unlike Free Pascal,
  TryStrToInt rejects values outside longint instead of wrapping them. }
function TryStrToInt64(const s: string; var value: int64): boolean;
begin
  value := 0;
  TryStrToInt64 := parseInt(s, value);
end;

{ 0 .. 2^64-1, with the prefixes of parseInt and no '-' }
function parseUInt(const s: string; var value: qword): boolean;
var
  i, base, d: longint;
  v: qword;
begin
  parseUInt := false;
  i := 1;
  while (i <= length(s)) and (s[i] = ' ') do
    i := i + 1;
  if (i <= length(s)) and (s[i] = '+') then
    i := i + 1;
  base := 10;
  if i <= length(s) then
    if s[i] = '$' then
    begin
      base := 16;
      i := i + 1;
    end
    else if s[i] = '&' then
    begin
      base := 8;
      i := i + 1;
    end
    else if s[i] = '%' then
    begin
      base := 2;
      i := i + 1;
    end
    else if (s[i] = '0') and (i < length(s)) and ((s[i + 1] = 'x') or (s[i + 1] = 'X')) then
    begin
      base := 16;
      i := i + 2;
    end;
  if i > length(s) then
    exit;
  v := 0;
  while i <= length(s) do
  begin
    d := digitValue(s[i]);
    if d >= base then
      exit;
    if v > (High(qword) - d) div base then
      exit;
    v := v * base + d;
    i := i + 1;
  end;
  value := v;
  parseUInt := true;
end;

function TryStrToQWord(const s: string; var value: qword): boolean;
begin
  value := 0;
  TryStrToQWord := parseUInt(s, value);
end;

function StrToQWord(const s: string): qword;
var
  v: qword;
begin
  v := 0;
  if not TryStrToQWord(s, v) then
    convertError('"' + s + '" is an invalid integer');
  StrToQWord := v;
end;

function StrToQWordDef(const s: string; def: qword): qword;
var
  v: qword;
begin
  v := 0;
  if TryStrToQWord(s, v) then
    StrToQWordDef := v
  else
    StrToQWordDef := def;
end;

function TryStrToInt(const s: string; var value: longint): boolean;
var
  v: int64;
begin
  TryStrToInt := false;
  value := 0;
  if not parseInt(s, v) or (v < -2147483647 - 1) or (v > 2147483647) then
    exit;
  value := v;
  TryStrToInt := true;
end;

function StrToInt(const s: string): longint;
var
  v: longint;
begin
  v := 0;
  if not TryStrToInt(s, v) then
    convertError('"' + s + '" is an invalid integer');
  StrToInt := v;
end;

function StrToInt64(const s: string): int64;
var
  v: int64;
begin
  v := 0;
  if not TryStrToInt64(s, v) then
    convertError('"' + s + '" is an invalid integer');
  StrToInt64 := v;
end;

function StrToIntDef(const s: string; def: longint): longint;
var
  v: longint;
begin
  v := 0;
  if TryStrToInt(s, v) then
    StrToIntDef := v
  else
    StrToIntDef := def;
end;

function StrToInt64Def(const s: string; def: int64): int64;
var
  v: int64;
begin
  v := 0;
  if TryStrToInt64(s, v) then
    StrToInt64Def := v
  else
    StrToInt64Def := def;
end;

{ Free Pascal's general format with 15 significant digits: fixed notation
  unless the decimal exponent is below -5 or at least 15. }
function FloatToStr(value: double): string;
const
  precision = 15;
var
  buf: array[0..63] of char;
  text, digits, r: string;
  p, e, i, last: longint;
  negative: boolean;
begin
  if value <> value then
  begin
    FloatToStr := 'Nan';
    exit;
  end;
  if (value <> 0.0) and (value / 2.0 = value) then
  begin
    if value > 0.0 then
      FloatToStr := '+Inf'
    else
      FloatToStr := '-Inf';
    exit;
  end;
  if value = 0.0 then
  begin
    FloatToStr := '0';
    exit;
  end;
  sys_format_double(@buf[0], 64, value, precision);
  text := StrPas(@buf[0]);
  negative := text[1] = '-';
  if negative then
    delete(text, 1, 1);
  { d.ddddde[+-]xx }
  p := pos('e', text);
  e := StrToInt(copy(text, p + 1, high(longint)));
  digits := text[1] + copy(text, 3, p - 3);
  last := length(digits);
  while (last > 1) and (digits[last] = '0') do
    last := last - 1;
  digits := copy(digits, 1, last);
  if (e < -5) or (e >= precision) then
  begin
    r := digits[1];
    if length(digits) > 1 then
      r := r + '.' + copy(digits, 2, high(longint));
    r := r + 'E' + IntToStr(e);
  end
  else if e < 0 then
  begin
    r := '0.';
    for i := 1 to -e - 1 do
      r := r + '0';
    r := r + digits;
  end
  else if length(digits) <= e + 1 then
  begin
    r := digits;
    for i := length(digits) + 1 to e + 1 do
      r := r + '0';
  end
  else
    r := copy(digits, 1, e + 1) + '.' + copy(digits, e + 2, high(longint));
  if negative then
    r := '-' + r;
  FloatToStr := r;
end;

function TryStrToFloat(const s: string; var value: double): boolean;
var
  tmp, word: string;
  i: longint;
  v: double;
begin
  TryStrToFloat := false;
  value := 0.0;
  tmp := Trim(s);
  if tmp = '' then
    exit;
  { a decimal number, Inf or NaN: not C's "infinity" and "0x1p3" }
  word := LowerCase(tmp);
  if (word[1] = '-') or (word[1] = '+') then
    delete(word, 1, 1);
  if (word <> 'inf') and (word <> 'nan') then
    for i := 1 to length(tmp) do
      if not (tmp[i] in ['0'..'9', '.', 'e', 'E', '+', '-']) then
        exit;
  if sys_parse_double(pchar(tmp), v) = 0 then
    exit;
  value := v;
  TryStrToFloat := true;
end;

function StrToFloat(const s: string): double;
var
  v: double;
begin
  v := 0.0;
  if not TryStrToFloat(s, v) then
    convertError('"' + s + '" is an invalid float');
  StrToFloat := v;
end;

function StrToFloatDef(const s: string; def: double): double;
var
  v: double;
begin
  v := 0.0;
  if TryStrToFloat(s, v) then
    StrToFloatDef := v
  else
    StrToFloatDef := def;
end;

function BoolToStr(b: boolean): string;
begin
  if b then
    BoolToStr := '-1'
  else
    BoolToStr := '0';
end;

{$IFDEF P3_OBJFPC}
function BoolToStr(b, useBoolStrs: boolean): string;
begin
  if not useBoolStrs then
    BoolToStr := BoolToStr(b)
  else if b then
    BoolToStr := 'True'
  else
    BoolToStr := 'False';
end;
{$ENDIF}

function TrimLeft(const s: string): string;
var
  i: longint;
begin
  i := 1;
  while (i <= length(s)) and (s[i] <= ' ') do
    i := i + 1;
  TrimLeft := copy(s, i, high(longint));
end;

function TrimRight(const s: string): string;
var
  n: longint;
begin
  n := length(s);
  while (n > 0) and (s[n] <= ' ') do
    n := n - 1;
  TrimRight := copy(s, 1, n);
end;

function Trim(const s: string): string;
begin
  Trim := TrimLeft(TrimRight(s));
end;

function UpperCase(const s: string): string;
var
  i: longint;
  r: string;
begin
  r := s;
  for i := 1 to length(r) do
    if (r[i] >= 'a') and (r[i] <= 'z') then
      r[i] := chr(ord(r[i]) - 32);
  UpperCase := r;
end;

function LowerCase(const s: string): string;
var
  i: longint;
  r: string;
begin
  r := s;
  for i := 1 to length(r) do
    if (r[i] >= 'A') and (r[i] <= 'Z') then
      r[i] := chr(ord(r[i]) + 32);
  LowerCase := r;
end;

{ (no range checks in these two: i stays within both strings) }
{$push}{$R-}
function CompareStr(const a, b: string): longint;
var
  i, n: longint;
begin
  n := length(a);
  if length(b) < n then
    n := length(b);
  for i := 1 to n do
    if a[i] <> b[i] then
    begin
      CompareStr := ord(a[i]) - ord(b[i]);
      exit;
    end;
  CompareStr := length(a) - length(b);
end;

{ CompareStr of the two lowercased (ASCII), without making them: a
  character pair is lowercased only where it differs. }
function CompareText(const a, b: string): longint;
var
  i, n: longint;
  ca, cb: char;
begin
  n := length(a);
  if length(b) < n then
    n := length(b);
  for i := 1 to n do
  begin
    ca := a[i];
    cb := b[i];
    if ca <> cb then
    begin
      if (ca >= 'A') and (ca <= 'Z') then
        ca := chr(ord(ca) + 32);
      if (cb >= 'A') and (cb <= 'Z') then
        cb := chr(ord(cb) + 32);
      if ca <> cb then
      begin
        CompareText := ord(ca) - ord(cb);
        exit;
      end;
    end;
  end;
  CompareText := length(a) - length(b);
end;
{$pop}

function SameText(const a, b: string): boolean;
begin
  SameText := CompareText(a, b) = 0;
end;

function QuotedStr(const s: string): string;
var
  i: longint;
  r: string;
begin
  r := '''';
  for i := 1 to length(s) do
  begin
    if s[i] = '''' then
      r := r + '''';
    r := r + s[i];
  end;
  QuotedStr := r + '''';
end;

function StringReplace(const s, from, into: string; flags: TReplaceFlags): string;
var
  rest, restCmp, fromCmp, r: string;
  p: longint;
begin
  if from = '' then
  begin
    StringReplace := s;
    exit;
  end;
  rest := s;
  if rfIgnoreCase in flags then
  begin
    restCmp := LowerCase(s);
    fromCmp := LowerCase(from);
  end
  else
  begin
    restCmp := s;
    fromCmp := from;
  end;
  r := '';
  repeat
    p := pos(fromCmp, restCmp);
    if p = 0 then
      break;
    r := r + copy(rest, 1, p - 1) + into;
    delete(rest, 1, p - 1 + length(from));
    delete(restCmp, 1, p - 1 + length(from));
  until not (rfReplaceAll in flags);
  StringReplace := r + rest;
end;

function IsDelimiter(const delimiters, s: string; index: longint): boolean;
begin
  IsDelimiter := (index >= 1) and (index <= length(s)) and (pos(s[index], delimiters) > 0);
end;

function LastDelimiter(const delimiters, s: string): longint;
var
  i: longint;
begin
  i := length(s);
  while (i > 0) and (pos(s[i], delimiters) = 0) do
    i := i - 1;
  LastDelimiter := i;
end;

function StrPas(p: pchar): string;
begin
  StrPas := p;
end;

procedure pas_format(dst, fmt, args: pointer); cdecl; external name '__pas_format_to';

function Format(const fmt: string; const args: array of const): string;
var
  s: string;
begin
  pas_format(@s, pointer(fmt), pointer(args));
  Format := s;
end;

{ The directory separators, and on Windows the drive separator, that end
  the directory part of a file name. Both slashes count on every system,
  as in Free Pascal. }
function isNameDelim(c: char): boolean;
begin
  isNameDelim := (c = '/') or (c = '\')
  {$IFDEF MSWINDOWS} or (c = ':') {$ENDIF};
end;

function isPathDelim(c: char): boolean;
begin
  isPathDelim := (c = '/') or (c = '\');
end;

{ the position of the last character of the directory part; 0 if none }
function dirEnd(const name: string): longint;
var
  i: longint;
begin
  i := length(name);
  while (i > 0) and not isNameDelim(name[i]) do
    i := i - 1;
  dirEnd := i;
end;

function ExtractFilePath(const name: string): string;
begin
  ExtractFilePath := copy(name, 1, dirEnd(name));
end;

function ExtractFileDir(const name: string): string;
var
  n: longint;
begin
  n := dirEnd(name);
  { the separators after the last directory, but not a root's }
  while (n > 1) and isPathDelim(name[n]) and not isNameDelim(name[n - 1]) do
    n := n - 1;
  ExtractFileDir := copy(name, 1, n);
end;

function ExtractFileName(const name: string): string;
begin
  ExtractFileName := copy(name, dirEnd(name) + 1, high(longint));
end;

{ the position of the '.' that starts the extension; 0 if none }
function extStart(const name: string): longint;
var
  i: longint;
begin
  extStart := 0;
  i := length(name);
  while (i > 0) and not isNameDelim(name[i]) do
  begin
    if name[i] = '.' then
    begin
      { a name's first dot does not start an extension: .bashrc }
      if (i > 1) and not isNameDelim(name[i - 1]) then
        extStart := i;
      exit;
    end;
    i := i - 1;
  end;
end;

function ExtractFileExt(const name: string): string;
var
  p: longint;
begin
  p := extStart(name);
  if p = 0 then
    ExtractFileExt := ''
  else
    ExtractFileExt := copy(name, p, high(longint));
end;

function ChangeFileExt(const name, ext: string): string;
var
  p: longint;
begin
  p := extStart(name);
  if p = 0 then
    ChangeFileExt := name + ext
  else
    ChangeFileExt := copy(name, 1, p - 1) + ext;
end;

function IncludeTrailingPathDelimiter(const path: string): string;
begin
  if (path <> '') and isPathDelim(path[length(path)]) then
    IncludeTrailingPathDelimiter := path
  else
    IncludeTrailingPathDelimiter := path + PathDelim;
end;

function ExcludeTrailingPathDelimiter(const path: string): string;
begin
  if (path <> '') and isPathDelim(path[length(path)]) then
    ExcludeTrailingPathDelimiter := copy(path, 1, length(path) - 1)
  else
    ExcludeTrailingPathDelimiter := path;
end;

function FileExists(const name: string): boolean;
begin
  FileExists := sys_file_exists(pchar(name)) <> 0;
end;

function DirectoryExists(const name: string): boolean;
begin
  DirectoryExists := sys_dir_exists(pchar(name)) <> 0;
end;

function DeleteFile(const name: string): boolean;
begin
  DeleteFile := sys_unlink(pchar(name)) <> 0;
end;

function RenameFile(const oldName, newName: string): boolean;
begin
  RenameFile := c_rename(pchar(oldName), pchar(newName)) = 0;
end;

function CreateDir(const name: string): boolean;
begin
  CreateDir := sys_mkdir(pchar(name)) <> 0;
end;

function RemoveDir(const name: string): boolean;
begin
  RemoveDir := sys_rmdir(pchar(name)) <> 0;
end;

function ForceDirectories(const name: string): boolean;
var
  dir: string;
begin
  dir := ExcludeTrailingPathDelimiter(name);
  if (dir = '') or DirectoryExists(dir) then
  begin
    ForceDirectories := dir <> '';
    exit;
  end;
  if (ExtractFileDir(dir) <> dir) and (ExtractFileDir(dir) <> '') then
    if not ForceDirectories(ExtractFileDir(dir)) then
    begin
      ForceDirectories := false;
      exit;
    end;
  ForceDirectories := CreateDir(dir);
end;

function GetCurrentDir: string;
var
  buf: array[0..4095] of char;
begin
  if sys_getcwd(@buf[0], 4096) <> 0 then
    GetCurrentDir := StrPas(@buf[0])
  else
    GetCurrentDir := '';
end;

function SetCurrentDir(const name: string): boolean;
begin
  SetCurrentDir := sys_chdir(pchar(name)) <> 0;
end;

function GetEnvironmentVariable(const name: string): string;
begin
  GetEnvironmentVariable := StrPas(c_getenv(pchar(name)));
end;

function IsLeapYear(year: word): boolean;
begin
  IsLeapYear := (year mod 4 = 0) and ((year mod 100 <> 0) or (year mod 400 = 0));
end;

function daysInMonth(year, month: word): longint;
const
  days: array[1..12] of longint = (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31);
begin
  if (month = 2) and IsLeapYear(year) then
    daysInMonth := 29
  else
    daysInMonth := days[month];
end;

function EncodeDate(year, month, day: word): TDateTime;
var
  y, m, era, yoe, doy, doe: int64;
begin
  if (year < 1) or (year > 9999) or (month < 1) or (month > 12) or (day < 1)
     or (day > daysInMonth(year, month)) then
  begin
    convertError('Invalid date');
    EncodeDate := 0.0;
    exit;
  end;
  { days from 1 March of year 0, then from TDateTime's 30 December 1899 }
  y := year;
  m := month;
  if m <= 2 then
    y := y - 1;
  era := y div 400;
  yoe := y - era * 400;
  if m > 2 then
    doy := (153 * (m - 3) + 2) div 5 + day - 1
  else
    doy := (153 * (m + 9) + 2) div 5 + day - 1;
  doe := yoe * 365 + yoe div 4 - yoe div 100 + doy;
  EncodeDate := era * 146097 + doe - 693899;
end;

function EncodeTime(hour, minute, second, ms: word): TDateTime;
begin
  if (hour > 23) or (minute > 59) or (second > 59) or (ms > 999) then
  begin
    convertError('Invalid time');
    EncodeTime := 0.0;
    exit;
  end;
  EncodeTime := (hour * 3600000.0 + minute * 60000.0 + second * 1000.0 + ms) / 86400000.0;
end;

procedure DecodeDate(d: TDateTime; var year, month, day: word);
var
  z, era, doe, yoe, doy, mp, y: int64;
begin
  z := trunc(d) + 693899;
  era := z div 146097;
  doe := z - era * 146097;
  yoe := (doe - doe div 1460 + doe div 36524 - doe div 146096) div 365;
  y := yoe + era * 400;
  doy := doe - (365 * yoe + yoe div 4 - yoe div 100);
  mp := (5 * doy + 2) div 153;
  day := doy - (153 * mp + 2) div 5 + 1;
  if mp < 10 then
    month := mp + 3
  else
    month := mp - 9;
  if month <= 2 then
    y := y + 1;
  year := y;
end;

procedure DecodeTime(d: TDateTime; var hour, minute, second, ms: word);
var
  total: int64;
begin
  total := round(abs(d - trunc(d)) * 86400000.0);
  if total >= 86400000 then
    total := 86399999;
  hour := total div 3600000;
  minute := (total div 60000) mod 60;
  second := (total div 1000) mod 60;
  ms := total mod 1000;
end;

function DayOfWeek(d: TDateTime): longint;
begin
  DayOfWeek := 1 + abs(trunc(d) - 1) mod 7;
end;

function Now: TDateTime;
var
  f: TTimeFields;
begin
  sys_localtime(f);
  Now := EncodeDate(f[0], f[1], f[2]) + EncodeTime(f[3], f[4], f[5], f[6]);
end;

function Date: TDateTime;
var
  f: TTimeFields;
begin
  sys_localtime(f);
  Date := EncodeDate(f[0], f[1], f[2]);
end;

function Time: TDateTime;
var
  f: TTimeFields;
begin
  sys_localtime(f);
  Time := EncodeTime(f[3], f[4], f[5], f[6]);
end;

function GetTickCount64: int64;
begin
  GetTickCount64 := sys_tick_ms;
end;

procedure Sleep(ms: longint);
begin
  sys_sleep(ms);
end;

end.
