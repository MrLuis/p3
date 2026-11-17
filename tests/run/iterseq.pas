{ Iterators with unit sequences: an iterator's helper (TIterSeq) gives it
  MoveNext / Current and the sequence operations; the proposal's example
  (Fibonacci.Take(10)).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/iterseq.expected
}
{$H+}
program iterseq;
uses SysUtils, sequences;

function Fibonacci: iterator UInt64;
begin
  var a: UInt64 := 0;
  var b: UInt64 := 1;
  while True do
  begin
    yield a;
    (a, b) := (b, a + b);
  end;
end;

function Lines: iterator string;
begin
  try
    yield 'one';
    yield 'two';
    yield 'three';
  finally
    Writeln('[closed]');
  end;
end;

procedure Test;
var it: iterator UInt64;
begin
  for var n in Fibonacci.Take(10) do Write(n, ' ');
  Writeln;
  Writeln(Fibonacci.Map(function(x) => IntToStr(x)).Skip(5).Take(3)
            .Fold('', function(a, s) => a + s + ';'));
  Writeln(Length(Lines.Filter(function(s) => Length(s) = 3).ToArray));
  it := Fibonacci;
  while it.MoveNext and (it.Current < 20) do Write(it.Current, ' ');
  Writeln;
end;

begin
  Test;
end.
