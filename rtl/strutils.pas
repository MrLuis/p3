{ p3's StrUtils: Free Pascal's names and results, on ansistrings. Written
  in the Turbo dialect so that every mode with units can use it. PosEx from
  the start of the string (two parameters) needs --mode=objfpc (overloading). }
unit strutils;

{$H+}

interface

function ReverseString(const s: string): string;
function LeftStr(const s: string; count: longint): string;
function RightStr(const s: string; count: longint): string;
function MidStr(const s: string; start, count: longint): string;
function DupeString(const s: string; count: longint): string;
function PadLeft(const s: string; n: longint): string;
function PadRight(const s: string; n: longint): string;
function AddChar(c: char; const s: string; n: longint): string;
function AddCharR(c: char; const s: string; n: longint): string;
function StartsStr(const sub, s: string): boolean;
function EndsStr(const sub, s: string): boolean;
function StartsText(const sub, s: string): boolean;
function EndsText(const sub, s: string): boolean;
function ContainsStr(const s, sub: string): boolean;
function ContainsText(const s, sub: string): boolean;
function AnsiStartsStr(const sub, s: string): boolean;
function AnsiEndsStr(const sub, s: string): boolean;
function AnsiContainsStr(const s, sub: string): boolean;
function PosEx(const sub, s: string; offset: longint): longint;
function ReplaceStr(const s, from, into: string): string;
function ReplaceText(const s, from, into: string): string;
function StuffString(const s: string; start, count: longint; const sub: string): string;
function Copy2Symb(const s: string; symb: char): string;
function Hex2Dec(const s: string): longint;
function Dec2Numb(n: int64; len, base: byte): string;
function IntToBin(value: int64; digits: longint): string;
function IndexStr(const text: string; const values: array of string): longint;
function IndexText(const text: string; const values: array of string): longint;
function MatchStr(const text: string; const values: array of string): boolean;
function MatchText(const text: string; const values: array of string): boolean;
function AnsiIndexStr(const text: string; const values: array of string): longint;
function AnsiIndexText(const text: string; const values: array of string): longint;
function AnsiMatchStr(const text: string; const values: array of string): boolean;
function AnsiMatchText(const text: string; const values: array of string): boolean;
{$IFDEF P3_OBJFPC}
function PosEx(const sub, s: string): longint;
{$ENDIF}

implementation

uses sysutils;

function ReverseString(const s: string): string;
var
  i: longint;
  r: string;
begin
  r := s;
  for i := 1 to length(s) do
    r[i] := s[length(s) - i + 1];
  ReverseString := r;
end;

function LeftStr(const s: string; count: longint): string;
begin
  if count <= 0 then
    LeftStr := ''
  else
    LeftStr := copy(s, 1, count);
end;

function RightStr(const s: string; count: longint): string;
begin
  if count <= 0 then
    RightStr := ''
  else if count >= length(s) then
    RightStr := s
  else
    RightStr := copy(s, length(s) - count + 1, count);
end;

function MidStr(const s: string; start, count: longint): string;
begin
  if (count <= 0) or (start > length(s)) then
    MidStr := ''
  else
  begin
    if start < 1 then
      start := 1;
    MidStr := copy(s, start, count);
  end;
end;

{ Makes the result once at its full length, moves s into the start, then
  doubles the filled part each step: log2(count) moves
  (docs/strutils-review.md ST-4). }
function DupeString(const s: string; count: longint): string;
var
  n, total, done, k: longint;
  r: string;
  p: PChar;
begin
  n := length(s);
  if (count <= 0) or (n = 0) then
  begin
    DupeString := '';
    exit;
  end;
  if count = 1 then
  begin
    DupeString := s;
    exit;
  end;
  total := n * count;
  setlength(r, total);
  p := PChar(r);
  move(s[1], p^, n);
  done := n;
  while done < total do
  begin
    k := done;
    if k > total - done then
      k := total - done;
    move(p^, p[done], k);
    done := done + k;
  end;
  DupeString := r;
end;

{ AddChar and AddCharR make the result once at its full length, fill the
  pad and move s into place; s itself when there is nothing to add
  (docs/strutils-review.md ST-2). }
function AddChar(c: char; const s: string; n: longint): string;
var
  k: longint;
  r: string;
begin
  k := n - length(s);
  if k <= 0 then
  begin
    AddChar := s;
    exit;
  end;
  setlength(r, n);
  fillchar(r[1], k, c);
  if length(s) > 0 then
    move(s[1], r[k + 1], length(s));
  AddChar := r;
end;

function AddCharR(c: char; const s: string; n: longint): string;
var
  k: longint;
  r: string;
begin
  k := n - length(s);
  if k <= 0 then
  begin
    AddCharR := s;
    exit;
  end;
  setlength(r, n);
  if length(s) > 0 then
    move(s[1], r[1], length(s));
  fillchar(r[length(s) + 1], k, c);
  AddCharR := r;
end;

function PadLeft(const s: string; n: longint): string;
begin
  PadLeft := AddChar(' ', s, n);
end;

function PadRight(const s: string; n: longint): string;
begin
  PadRight := AddCharR(' ', s, n);
end;

{ Whether sub is at position at of s (it fits there), compared in place
  (docs/strutils-review.md ST-6). }
function strAt(const sub, s: string; at: longint): boolean;
var
  i, n: longint;
  a, b: PChar;
begin
  n := length(sub);
  a := PChar(sub);
  b := PChar(s) + (at - 1);
  i := 0;
  while (i < n) and (a[i] = b[i]) do
    i := i + 1;
  strAt := i = n;
end;

{ strAt without case (ASCII): a pair is lowercased only where it differs,
  as in CompareText; no string is made (docs/strutils-review.md ST-5). }
function textAt(const sub, s: string; at: longint): boolean;
var
  i, n: longint;
  a, b: PChar;
  ca, cb: char;
begin
  textAt := false;
  n := length(sub);
  a := PChar(sub);
  b := PChar(s) + (at - 1);
  for i := 0 to n - 1 do
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
        exit;
    end;
  end;
  textAt := true;
end;

function StartsStr(const sub, s: string): boolean;
begin
  StartsStr := (length(sub) <= length(s)) and strAt(sub, s, 1);
end;

function EndsStr(const sub, s: string): boolean;
begin
  EndsStr := (length(sub) <= length(s)) and
    strAt(sub, s, length(s) - length(sub) + 1);
end;

function StartsText(const sub, s: string): boolean;
begin
  StartsText := (length(sub) <= length(s)) and textAt(sub, s, 1);
end;

function EndsText(const sub, s: string): boolean;
begin
  EndsText := (length(sub) <= length(s)) and
    textAt(sub, s, length(s) - length(sub) + 1);
end;

function ContainsStr(const s, sub: string): boolean;
begin
  ContainsStr := (sub <> '') and (pos(sub, s) > 0);
end;

function ContainsText(const s, sub: string): boolean;
begin
  ContainsText := ContainsStr(LowerCase(s), LowerCase(sub));
end;

function AnsiStartsStr(const sub, s: string): boolean;
begin
  AnsiStartsStr := StartsStr(sub, s);
end;

function AnsiEndsStr(const sub, s: string): boolean;
begin
  AnsiEndsStr := EndsStr(sub, s);
end;

function AnsiContainsStr(const s, sub: string): boolean;
begin
  AnsiContainsStr := ContainsStr(s, sub);
end;

{ The position of the first sub in s at or after start; 0 if none (sub is
  not empty). The same search as sysutils' own posFrom, which is not
  exported. }
function findFrom(const sub, s: string; start: longint): longint;
var
  i, j, last, m: longint;
begin
  findFrom := 0;
  m := length(sub);
  last := length(s) - m + 1;
  i := start;
  while i <= last do
  begin
    if s[i] = sub[1] then
    begin
      j := 2;
      while (j <= m) and (s[i + j - 1] = sub[j]) do
        j := j + 1;
      if j > m then
      begin
        findFrom := i;
        exit;
      end;
    end;
    i := i + 1;
  end;
end;

{ Searches s in place from offset: no copy of the rest of the string, so a
  scan that goes on from each match is linear (docs/strutils-review.md
  ST-1). }
function PosEx(const sub, s: string; offset: longint): longint;
begin
  PosEx := 0;
  if (offset < 1) or (offset > length(s)) or (sub = '') then
    exit;
  PosEx := findFrom(sub, s, offset);
end;

{$IFDEF P3_OBJFPC}
function PosEx(const sub, s: string): longint;
begin
  PosEx := PosEx(sub, s, 1);
end;
{$ENDIF}

function ReplaceStr(const s, from, into: string): string;
begin
  ReplaceStr := StringReplace(s, from, into, [rfReplaceAll]);
end;

function ReplaceText(const s, from, into: string): string;
begin
  ReplaceText := StringReplace(s, from, into, [rfReplaceAll, rfIgnoreCase]);
end;

function StuffString(const s: string; start, count: longint; const sub: string): string;
begin
  StuffString := copy(s, 1, start - 1) + sub + copy(s, start + count, high(longint));
end;

function Copy2Symb(const s: string; symb: char): string;
var
  p: longint;
begin
  p := pos(symb, s);
  if p = 0 then
    Copy2Symb := s
  else
    Copy2Symb := copy(s, 1, p - 1);
end;

function IndexStr(const text: string; const values: array of string): longint;
var
  i: longint;
begin
  IndexStr := -1;
  for i := 0 to high(values) do
    if values[i] = text then
    begin
      IndexStr := i;
      exit;
    end;
end;

function IndexText(const text: string; const values: array of string): longint;
var
  i: longint;
begin
  IndexText := -1;
  for i := 0 to high(values) do
    if SameText(values[i], text) then
    begin
      IndexText := i;
      exit;
    end;
end;

function MatchStr(const text: string; const values: array of string): boolean;
begin
  MatchStr := IndexStr(text, values) >= 0;
end;

function MatchText(const text: string; const values: array of string): boolean;
begin
  MatchText := IndexText(text, values) >= 0;
end;

function AnsiIndexStr(const text: string; const values: array of string): longint;
begin
  AnsiIndexStr := IndexStr(text, values);
end;

function AnsiIndexText(const text: string; const values: array of string): longint;
begin
  AnsiIndexText := IndexText(text, values);
end;

function AnsiMatchStr(const text: string; const values: array of string): boolean;
begin
  AnsiMatchStr := MatchStr(text, values);
end;

function AnsiMatchText(const text: string; const values: array of string): boolean;
begin
  AnsiMatchText := MatchText(text, values);
end;

function Hex2Dec(const s: string): longint;
begin
  if (s <> '') and (s[1] = '$') then
    Hex2Dec := StrToInt(s)
  else
    Hex2Dec := StrToInt('$' + s);
end;

function Dec2Numb(n: int64; len, base: byte): string;
const
  digits: string[36] = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';
var
  r: string;
begin
  r := '';
  while n > 0 do
  begin
    r := digits[n mod base + 1] + r;
    n := n div base;
  end;
  Dec2Numb := AddChar('0', r, len);
end;

{ Makes the result once and writes the bits from the last digit back,
  through a pointer; digits past the 64th are '0', as in Free Pascal
  (docs/strutils-review.md ST-3). }
function IntToBin(value: int64; digits: longint): string;
var
  i, bits: longint;
  r: string;
  p: PChar;
begin
  if digits <= 0 then
  begin
    IntToBin := '';
    exit;
  end;
  setlength(r, digits);
  p := PChar(r);
  bits := digits;
  if bits > 64 then
    bits := 64;
  for i := digits - 1 downto digits - bits do
  begin
    if value and 1 = 1 then
      p[i] := '1'
    else
      p[i] := '0';
    value := value shr 1;
  end;
  for i := 0 to digits - bits - 1 do
    p[i] := '0';
  IntToBin := r;
end;

end.
