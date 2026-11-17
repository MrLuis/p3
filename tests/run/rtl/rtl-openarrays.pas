{ The unit library's routines taking open arrays. Output identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/rtl-openarrays.expected %t.out
  RUN: %p3 --mode=objfpc -fseparate-units %s -o %t.sep
  RUN:  %t.sep > %t.sep.out
  RUN: diff %S/rtl-openarrays.expected %t.sep.out
}
program rtlopenarrays;
{$H+}
uses sysutils, strutils, math, classes;
var
  d: array[1..4] of double = (1.5, -2.25, 8, 0.5);
  n: array[0..3] of longint = (4, -7, 9, 2);
  v: array[1..2] of string;
  s: TStringList;
  i: longint;
begin
  writeln(Sum(d):0:3, ' ', Mean(d):0:3, ' ', MinValue(d):0:3, ' ', MaxValue(d):0:3);
  writeln(SumInt(n), ' ', MinIntValue(n), ' ', MaxIntValue(n), ' ', MinValue(n), ' ', MaxValue(n));
  v[1] := 'Beta'; v[2] := 'gamma';
  writeln(IndexStr('gamma', v), IndexStr('GAMMA', v), IndexText('GAMMA', v), IndexText('zz', v), IndexStr('b', ['a', 'b']), IndexStr('x', []));
  writeln(MatchStr('Beta', v), MatchStr('beta', v), MatchText('beta', v), MatchText('q', v));
  writeln(AnsiIndexStr('Beta', v), AnsiIndexText('BETA', v), AnsiMatchStr('Beta', v), AnsiMatchText('BETA', v));
  s := TStringList.Create;
  s.AddStrings(['p', 'q', 'r']); s.AddStrings(v); s.AddStrings([]);
  for i := 0 to s.Count - 1 do write(s[i], ' '); writeln(s.Count);
  s.Free;
end.
