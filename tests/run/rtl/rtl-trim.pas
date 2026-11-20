{ Trim, TrimLeft and TrimRight find what to cut and make one string, or
  return the string itself when nothing is cut (docs/sysutils-review.md
  SU-6): empty, all blanks, nothing to trim, one side, both sides,
  control characters (everything up to ' ' goes), inner blanks kept, a
  single character, and a change to a result not reaching the original
  (the string may be shared). Output identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-trim.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-trim.expected
}
program rtltrim;
{$H+}
uses sysutils;

var
  seed: longint;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := (seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := (seed shr 8) mod n;
end;
{$Q+}{$R+}

procedure Show(const s: string);
begin
  writeln('[', s, '] ', length(s), ': [', Trim(s), '] [', TrimLeft(s), '] [', TrimRight(s), '] ',
    length(Trim(s)), ' ', length(TrimLeft(s)), ' ', length(TrimRight(s)));
end;

var
  s, t: string;
  i, n, bad: longint;
begin
  Show('');
  Show(' ');
  Show('     ');
  Show('abc');
  Show('a');
  Show(' a');
  Show('a ');
  Show(' a ');
  Show('  two words  ');
  Show('  a b  c  ');
  Show(#9 + 'tab' + #9);
  Show(#13#10 + 'line' + #13#10);
  Show(#1 + 'ctl' + #31);
  Show('x' + #127 + 'y');
  Show('  ' + #200 + 'hi' + #200 + '  ');

  { a result that is the original string itself must not change it }
  s := 'untouched';
  t := Trim(s);
  t[1] := 'U';
  writeln(s, ' ', t);
  t := TrimLeft(s);
  t[2] := 'N';
  writeln(s, ' ', t);
  t := TrimRight(s);
  t[3] := 'T';
  writeln(s, ' ', t);
  s := '  padded  ';
  t := Trim(s);
  t[1] := 'P';
  writeln('[', s, '] [', t, ']');

  { random strings against a plain scan }
  seed := 3;
  bad := 0;
  for i := 1 to 2000 do
  begin
    s := '';
    t := '';
    n := Rnd(12);
    while length(s) < n do
    begin
      case Rnd(4) of
        0: s := s + ' ';
        1: s := s + #9;
        2: s := s + 'a';
        3: s := s + 'b';
      end;
    end;
    { reference: the part from the first to the last character above ' ' }
    t := s;
    while (t <> '') and (t[1] <= ' ') do
      delete(t, 1, 1);
    while (t <> '') and (t[length(t)] <= ' ') do
      delete(t, length(t), 1);
    if Trim(s) <> t then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
