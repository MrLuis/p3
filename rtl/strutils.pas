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

{ Makes the result once and writes it through a pointer, reading s from
  its end (docs/strutils-review.md ST-7). }
function ReverseString(const s: string): string;
var
  i, n: longint;
  r: string;
  p, q: PChar;
begin
  n := length(s);
  if n = 0 then
  begin
    ReverseString := '';
    exit;
  end;
  setlength(r, n);
  p := PChar(r);
  q := PChar(s) + (n - 1);
  for i := 0 to n - 1 do
    p[i] := (q - i)^;
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

{ (no range checks in this one: i stays within both strings, whose
  lengths hold n), as in sysutils' own copy }
{$push}{$R-}{$Q-}
type
  PCompareBlock = ^QWord;

{ The index (from 0) of the first 8-byte block of a and b, both n bytes
  or longer, that differs, or the start of the last partial block: equal
  blocks are skipped a word at a time (unaligned loads), the 6 lines of
  sysutils' equalPrefix, repeated as its helpers were (ST-14). }
function equalPrefix(a, b: PChar; n: longint): longint;
var
  i: longint;
begin
  i := 0;
  while (i + 8 <= n) and (PCompareBlock(a + i)^ = PCompareBlock(b + i)^) do
    i := i + 8;
  equalPrefix := i;
end;
{$pop}

{ Whether sub is at position at of s (it fits there), compared in place:
  equal blocks of 8 first, a word at a time, then the tail by bytes, so
  that a long shared prefix is skimmed (docs/strutils-review.md ST-6,
  ST-15). }
function strAt(const sub, s: string; at: longint): boolean;
var
  i, n: longint;
  a, b: PChar;
begin
  n := length(sub);
  a := PChar(sub);
  b := PChar(s) + (at - 1);
  i := equalPrefix(a, b, n);
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

{ Scans s in place for the first character of sub in either case, then
  compares the rest from the second character, lower-folding a pair only
  where it differs, through two PChars made once: no string and no helper
  call at a candidate, whose first character has already matched
  (docs/strutils-review.md ST-10, ST-13). }
function ContainsText(const s, sub: string): boolean;
var
  i, j, last, n: longint;
  lo, up, ca, cb: char;
  ok: boolean;
  p, q: PChar;
begin
  ContainsText := false;
  n := length(sub);
  last := length(s) - n;
  if (n = 0) or (last < 0) then
    exit;
  lo := sub[1];
  if (lo >= 'A') and (lo <= 'Z') then
    lo := chr(ord(lo) + 32);
  up := lo;
  if (lo >= 'a') and (lo <= 'z') then
    up := chr(ord(lo) - 32);
  p := PChar(s);
  q := PChar(sub);
  for i := 0 to last do
    if (p[i] = lo) or (p[i] = up) then
    begin
      ok := true;
      for j := 1 to n - 1 do
      begin
        ca := q[j];
        cb := p[i + j];
        if ca <> cb then
        begin
          if (ca >= 'A') and (ca <= 'Z') then
            ca := chr(ord(ca) + 32);
          if (cb >= 'A') and (cb <= 'Z') then
            cb := chr(ord(cb) + 32);
          if ca <> cb then
          begin
            ok := false;
            break;
          end;
        end;
      end;
      if ok then
      begin
        ContainsText := true;
        exit;
      end;
    end;
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
  not empty). The same search as sysutils' own posFrom, kept in step at
  both ends (ST-1, ST-14): strutils uses sysutils, and no helper is
  exported from it than Free Pascal's names. In place through two PChars,
  as posFrom is (SU-8, of sysutils-review.md). }
function findFrom(const sub, s: string; start: longint): longint;
var
  i, j, last, m: longint;
  a, b: PChar;
begin
  findFrom := 0;
  m := length(sub);
  if m = 0 then
    exit;
  last := length(s) - m + 1;
  a := PChar(sub);
  b := PChar(s);
  i := start;
  while i <= last do
  begin
    if b[i - 1] = a[0] then
    begin
      j := 1;
      while (j < m) and (b[i - 1 + j] = a[j]) do
        j := j + 1;
      if j = m then
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

{ copy(s, 1, start - 1) + sub + copy(s, start + count, ...), made once:
  the two parts of s are clamped as copy clamps them and moved into place
  with sub between (docs/strutils-review.md ST-8). The head arrives
  already in place: the result borrows s and setlength keeps s's own
  first characters, so only sub and the tail move, and setlength clears
  at most what the tail moves over (docs/strutils-review.md ST-11). }
function StuffString(const s: string; start, count: longint; const sub: string): string;
var
  n, head, from, tail, m: longint;
  r: string;
  ps, pd: PChar;
begin
  n := length(s);
  head := start - 1;
  if head < 0 then
    head := 0
  else if head > n then
    head := n;
  from := start + count;
  if from < 1 then
    from := 1;
  if from > n then
    tail := 0
  else
    tail := n - from + 1;
  m := length(sub);
  n := head + m + tail;
  if n = 0 then
  begin
    StuffString := '';
    exit;
  end;
  r := s;
  setlength(r, n);
  ps := PChar(s);
  pd := PChar(r);
  if m > 0 then
    move(PChar(sub)^, (pd + head)^, m);
  if tail > 0 then
    move((ps + from - 1)^, (pd + head + m)^, tail);
  StuffString := r;
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

{ The value of s as hexadecimal, made without a '$' + s string: when s
  starts with $ it is StrToInt's as it is; otherwise its digits are taken
  in place, on the negative side, as StrToInt's parse builds them, and the
  answer is TryStrToInt's same longint range. Anything the digits loop
  refuses goes to StrToInt('$' + s), which fails with the same '$' + s
  named in its message as before (docs/strutils-review.md ST-12). }
function Hex2Dec(const s: string): longint;
const
  lowest = -9223372036854775807 - 1;
var
  i: longint;
  c: char;
  d: longint;
  ok: boolean;
  v: int64;
begin
  if (s <> '') and (s[1] = '$') then
  begin
    Hex2Dec := StrToInt(s);
    exit;
  end;
  if s = '' then
  begin
    Hex2Dec := StrToInt('$' + s);
    exit;
  end;
  v := 0;
  ok := true;
  for i := 1 to length(s) do
  begin
    c := s[i];
    if (c >= '0') and (c <= '9') then
      d := ord(c) - ord('0')
    else if (c >= 'a') and (c <= 'f') then
      d := ord(c) - (ord('a') - 10)
    else if (c >= 'A') and (c <= 'F') then
      d := ord(c) - (ord('A') - 10)
    else
    begin
      ok := false;
      break;
    end;
    if v < (lowest + d) div 16 then
    begin
      ok := false;
      break;
    end;
    v := v * 16 - d;
  end;
  if ok and (v <> lowest) and (-v >= -2147483647 - 1) and (-v <= 2147483647) then
    Hex2Dec := -v
  else
    Hex2Dec := StrToInt('$' + s);
end;

function Dec2Numb(n: int64; len, base: byte): string;
const
  digits: string[36] = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';
var
  buf: array[0..63] of char;
  at, k, total: longint;
  r: string;
begin
  { the digits from the end of buf (base 2 needs 63), then the result made
    once with the zero padding in front (docs/strutils-review.md ST-9) }
  at := 64;
  while (n > 0) and (at > 0) do
  begin
    at := at - 1;
    buf[at] := digits[n mod base + 1];
    n := n div base;
  end;
  k := 64 - at;
  total := len;
  if total < k then
    total := k;
  if total = 0 then
  begin
    Dec2Numb := '';
    exit;
  end;
  setlength(r, total);
  if total > k then
    fillchar(r[1], total - k, '0');
  if k > 0 then
    move(buf[at], r[total - k + 1], k);
  Dec2Numb := r;
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
