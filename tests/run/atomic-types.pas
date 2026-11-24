{ TAtomic<T>: Integer and Int64 use atomic operations, other numeric types
  (Double, Cardinal, Word) a lock; every method gives the same answers for all.
  Many tasks adding to one counter lose nothing.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/atomic-types.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/atomic-types.expected
}
{$H+}
program atomictypes;

uses SysUtils, threading;

type
  TInt = specialize TAtomic<Integer>;
  TBig = specialize TAtomic<Int64>;
  TCard = specialize TAtomic<Cardinal>;
  TReal = specialize TAtomic<Double>;
  TWord = specialize TAtomic<Word>;

function BumpInt(c: TInt; times: Integer): async;
var
  i: Integer;
begin
  for i := 1 to times do
    c.Increment;
end;

function BumpBig(c: TBig; times: Integer): async;
var
  i: Integer;
begin
  for i := 1 to times do
    c.Add(3000000000);
end;

var
  I: TInt;
  B: TBig;
  S: TCard;
  D: TReal;
  F: TWord;
  t: array[0..3] of async;
  k: Integer;
begin
  I := TInt.Create(5);
  Writeln('int: ', I.Load, ' ', I.Exchange(9), ' ', I.Load, ' ', I.CompareExchange(9, 1), ' ',
          I.CompareExchange(9, 2), ' ', I.Load, ' ', I.Add(10), ' ', I.Increment, ' ', I.Decrement);
  I.Store(-4);
  Writeln('int store: ', I.Load, ' ', I.Add(-6));

  B := TBig.Create(5000000000);
  Writeln('int64: ', B.Load, ' ', B.Exchange(-9000000000), ' ', B.Load, ' ',
          B.CompareExchange(-9000000000, 1), ' ', B.CompareExchange(-9000000000, 2), ' ', B.Load, ' ',
          B.Add(4000000000), ' ', B.Increment, ' ', B.Decrement);
  B.Store(9223372036854775807);
  Writeln('int64 store: ', B.Load);

  S := TCard.Create(4000000000);
  Writeln('cardinal: ', S.Load, ' ', S.Exchange(7), ' ', S.Load, ' ', S.CompareExchange(7, 8), ' ',
          S.CompareExchange(7, 9), ' ', S.Load, ' ', S.Add(2), ' ', S.Increment, ' ', S.Decrement);
  S.Store(4294967295);
  Writeln('cardinal store: ', S.Load);

  D := TReal.Create(1.5);
  Writeln('double: ', D.Load:0:2, ' ', D.Exchange(2.5):0:2, ' ', D.Add(0.25):0:2, ' ', D.Increment:0:2,
          ' ', D.CompareExchange(3.75, 1), ' ', D.Load:0:2);

  F := TWord.Create(65535);
  Writeln('word: ', F.Load, ' ', F.Exchange(3), ' ', F.Load, ' ', F.CompareExchange(3, 9), ' ', F.Load, ' ', F.Add(1));

  I.Store(0);
  B.Store(0);
  for k := 0 to 3 do
    t[k] := Pool.Run(BumpInt(I, 50000));
  for k := 0 to 3 do
    Wait(t[k]);
  for k := 0 to 3 do
    t[k] := Pool.Run(BumpBig(B, 20000));
  for k := 0 to 3 do
    Wait(t[k]);
  Writeln('tasks: ', I.Load, ' ', B.Load);
  I.Free; B.Free; S.Free; D.Free; F.Free;
end.
