{ Distinct types (T = type X, not in --mode=iso), as in Free Pascal: here
  an alias of X, compatible with it everywhere, also as a var argument.
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/distinct.expected
  RUN: %pcc --mode=tp %s -o %t.tp
  RUN: %t.tp | diff - %S/distinct.expected
}
program distinct;
{$H+}
Type TFileName = type string;
  TMyInt = type longint;
  TName = type shortstring;
  TDay = (mon, tue, wed);
  TWeekday = type TDay;

procedure Shout(var s: string);
begin
  s := s + '!';
end;

function Twice(n: longint): longint;
begin
  Twice := n * 2;
end;

var
  f: TFileName;
  s: string;
  i: TMyInt;
  j: longint;
  n: TName;
  d: TWeekday;
begin
  f := 'a.txt';
  s := f;
  f := s + '.bak';
  writeln(f, ' ', s, ' ', length(f));
  Shout(f);
  writeln(f);
  i := 3;
  j := i + 1;
  i := Twice(j);
  writeln(i, ' ', j, ' ', sizeof(i));
  n := 'short';
  writeln(n);
  d := tue;
  writeln(ord(d), ' ', ord(succ(d)));
end.
