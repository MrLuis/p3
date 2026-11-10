{ Short strings: assignment, concatenation, comparison, copy, pos, value
  and const parameters. }
program shortstr;

function Pad(s: string; width: longint): string;
begin
  while length(s) < width do
    s := ' ' + s;
  Pad := s;
end;

function Digits(n: longint): string;
var s: string;
begin
  s := '';
  repeat
    s := chr(ord('0') + n mod 10) + s;
    n := n div 10;
  until n = 0;
  Digits := s;
end;

function IsKeyword(const s: string): boolean;
begin
  IsKeyword := (s = 'begin') or (s = 'end') or (s = 'if') or (s = 'then') or
               (s = 'else') or (s = 'while') or (s = 'do') or (s = 'for');
end;

function Hash(const s: string): longint;
var i, h: longint;
begin
  h := 0;
  for i := 1 to length(s) do
    h := (h * 31 + ord(s[i])) and $FFFFFF;
  Hash := h;
end;

var
  i, sum, keywords: longint;
  line, word: string;
  words: array[0..7] of string[10];
begin
  words[0] := 'begin'; words[1] := 'count'; words[2] := 'while';
  words[3] := 'value'; words[4] := 'end'; words[5] := 'x';
  words[6] := 'then'; words[7] := 'index';
  sum := 0;
  keywords := 0;
  for i := 1 to 1500000 do
  begin
    word := words[i and 7];
    if IsKeyword(word) then
      keywords := keywords + 1;
    line := Pad(Digits(i), 8) + ': ' + word;
    if pos('7', line) > 0 then
      line := copy(line, 1, 8);
    sum := (sum + Hash(line)) and $FFFFFF;
  end;
  writeln(sum, ' ', keywords);
end.
