{ Ansistrings: building, concatenation, parameters, results, comparison. }
program ansistr;
{$H+}

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

function Join(const a, b: string; sep: char): string;
begin
  Join := a + sep + b;
end;

function Hash(const s: string): longint;
var i, h: longint;
begin
  h := 0;
  for i := 1 to length(s) do
    h := (h * 31 + ord(s[i])) and $FFFFFF;
  Hash := h;
end;

function Longest(a, b: string): string;
begin
  if length(a) >= length(b) then
    Longest := a
  else
    Longest := b;
end;

var
  i, sum: longint;
  line, best, all: string;
begin
  sum := 0;
  best := '';
  for i := 1 to 300000 do
  begin
    line := Join(Digits(i), Digits(i * 7), ',');
    best := Longest(best, line);
    if line = best then
      sum := sum + 1;
    sum := (sum + Hash(line)) and $FFFFFF;
  end;
  { building a long string piece by piece }
  all := '';
  for i := 1 to 40000 do
    all := all + Digits(i) + ' ';
  writeln(sum, ' ', length(all), ' ', Hash(all));
end.
