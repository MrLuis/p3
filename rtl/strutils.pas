{ pcc's StrUtils: Free Pascal's names and results, on ansistrings. Written
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
{$IFDEF PCC_OBJFPC}
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

function DupeString(const s: string; count: longint): string;
var
  r: string;
begin
  r := '';
  while count > 0 do
  begin
    r := r + s;
    count := count - 1;
  end;
  DupeString := r;
end;

function padding(c: char; n: longint): string;
var
  r: string;
begin
  r := '';
  while n > 0 do
  begin
    r := r + c;
    n := n - 1;
  end;
  padding := r;
end;

function AddChar(c: char; const s: string; n: longint): string;
begin
  AddChar := padding(c, n - length(s)) + s;
end;

function AddCharR(c: char; const s: string; n: longint): string;
begin
  AddCharR := s + padding(c, n - length(s));
end;

function PadLeft(const s: string; n: longint): string;
begin
  PadLeft := AddChar(' ', s, n);
end;

function PadRight(const s: string; n: longint): string;
begin
  PadRight := AddCharR(' ', s, n);
end;

function StartsStr(const sub, s: string): boolean;
begin
  StartsStr := (length(sub) <= length(s)) and (copy(s, 1, length(sub)) = sub);
end;

function EndsStr(const sub, s: string): boolean;
begin
  EndsStr := (length(sub) <= length(s)) and (RightStr(s, length(sub)) = sub);
end;

function StartsText(const sub, s: string): boolean;
begin
  StartsText := StartsStr(LowerCase(sub), LowerCase(s));
end;

function EndsText(const sub, s: string): boolean;
begin
  EndsText := EndsStr(LowerCase(sub), LowerCase(s));
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

function PosEx(const sub, s: string; offset: longint): longint;
var
  p: longint;
begin
  PosEx := 0;
  if (offset < 1) or (offset > length(s)) or (sub = '') then
    exit;
  p := pos(sub, copy(s, offset, high(longint)));
  if p > 0 then
    PosEx := p + offset - 1;
end;

{$IFDEF PCC_OBJFPC}
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

function IntToBin(value: int64; digits: longint): string;
var
  i: longint;
  r: string;
begin
  r := '';
  for i := digits - 1 downto 0 do
    if (value shr i) and 1 = 1 then
      r := r + '1'
    else
      r := r + '0';
  IntToBin := r;
end;

end.
