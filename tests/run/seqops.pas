{ Unit sequences (item 15, LINQ-style operations): lazy, chainable
  operations on TSeq<T> and on arrays, with lambdas whose types come from
  the sequence; the proposal's example first.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/seqops.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/seqops.expected
}
{$H+}
program seqops;
uses SysUtils, sequences;

type
  TCustomer = record
    Name: string;
    Active: Boolean;
    Balance: Integer;
    Region: string;
  end;

function Customer(const N: string; A: Boolean; B: Integer; const R: string): TCustomer;
begin
  Result.Name := N;
  Result.Active := A;
  Result.Balance := B;
  Result.Region := R;
end;

{ First of an empty sequence raises }
procedure Empty;
begin
  try
    Writeln(Range(1, 0).First);
  except
    on E: ESequenceEmpty do
      Writeln('empty: ', E.Message);
  end;
end;

{ nothing is computed until the values are asked for }
procedure Lazy;
var
  calls: Integer;
  s: specialize TSeq<Integer>;
begin
  calls := 0;
  s := Range(1, 1000000).Map(function(x) => begin
                                 Inc(calls);
                                 Result := x * x;
                               end);
  Writeln('calls before: ', calls);
  Writeln('first three: ', s.Take(3).Fold('', function(a, x) => a + IntToStr(x) + ' '));
  Writeln('calls after: ', calls);
  { s (in this frame) holds a lambda that uses calls (in this frame): a
    cycle, broken by hand }
  s.Next := nil;
end;

var
  customers: array of TCustomer;
  names: array of string;
  s: string;
begin
  customers := [Customer('Zoe', True, 120, 'north'), Customer('Al', True, 0, 'south'),
                Customer('Bea', False, 50, 'north'), Customer('Cy', True, 75, 'south'),
                Customer('Dan', True, 10, 'north')];
  { the proposal's example }
  names := customers
    .Filter(function(c) => c.Active and (c.Balance > 0))
    .OrderBy(function(c) => c.Name)
    .Map(function(c) => c.Name)
    .ToArray;
  for s in names do
    Write(s, ' ');
  Writeln;
  { lazy and endless }
  for var n in Iterate(1, function(x) => x * 2).TakeWhile(function(x) => x < 100) do
    Write(n, ' ');
  Writeln;
  Writeln(Range(1, 10).Filter(function(x) => Odd(x)).Fold(0, function(acc, x) => acc + x));
  for var g in Seq(customers).GroupBy(function(c) => c.Region) do
    Writeln(g.Key, ': ', Length(g.Items));
  for var p in Range(1, 3).Zip(Seq(['a', 'b', 'c', 'd'])) do
  begin
    var (i, t) := p;
    Write(i, t, ' ');
  end;
  Writeln;
  Writeln(Range(1, 5).Skip(2).Take(2).Map(function(x) => IntToStr(x)).Fold('', function(a, x) => a + x));
  Writeln(Seq([3, 1, 3, 2, 1]).DistinctBy(function(x) => x).Count, ' ',
          Range(1, 4).OrderByDescending(function(x) => x).First, ' ',
          Range(1, 4).Last, ' ',
          Range(1, 4).Any(function(x) => x > 3), ' ',
          Range(1, 4).All(function(x) => x > 3));
  Range(1, 3).Concat(Replicate(9, 2)).ForEach(procedure(const x: Integer) => Write(x, ' '));
  Writeln;
  Empty;
  Lazy;
end.
