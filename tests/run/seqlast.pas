{ TSeq.Last (docs/sequences-review.md SQ-5) reads into two variables in
  turn and copies only the last value. A source may leave junk in its
  variable when it reports the end (Filter leaves the last value it
  rejected), so the value before the end must be the one returned: sequences
  of every length from 1 to 6, filters whose tail is rejected, strings, an
  empty sequence and each value read once.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/seqlast.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/seqlast.expected
}
{$H+}
program seqlast;
uses SysUtils, sequences;

var
  N, Reads: Integer;
  Names: specialize TArray<string>;
  S: specialize TSeq<Integer>;
begin
  for N := 1 to 6 do
    write(Range(1, N).Last, ' ');
  writeln;
  for N := 1 to 6 do
    write(Range(1, N).Filter(function(const x: Integer): Boolean => x mod 3 = 1).Last, ' ');
  writeln;
  for N := 1 to 6 do
    write(Range(1, 100).Take(N).Last, ' ');
  writeln;
  Names := nil;
  SetLength(Names, 3);
  Names[0] := 'ann';
  Names[1] := 'bob';
  Names[2] := 'cy' + 'd';
  writeln(Names.Seq.Last, ' ', Names.Seq.Skip(1).Take(1).Last,
    ' ', Names.Seq.Filter(function(const x: string): Boolean => x <> 'cyd').Last);

  Reads := 0;
  S := Range(1, 7).Map(function(x) => begin
                          Inc(Reads);
                          Result := x * 10;
                        end);
  writeln(S.Last, ' read ', Reads, ' times');
  S.Next := nil;

  try
    writeln(Range(1, 0).Last);
  except
    on E: ESequenceEmpty do
      writeln('empty: ', E.Message);
  end;
  try
    writeln(Range(1, 5).Filter(function(const x: Integer): Boolean => x > 9).Last);
  except
    on E: ESequenceEmpty do
      writeln('none match: ', E.Message);
  end;
end.
