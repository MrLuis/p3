{ TSeq.Map (docs/sequences-review.md SQ-6) reads each source value into
  one variable kept for the sequence instead of a new one per call. For a
  managed type (string, record with a string, dynamic array) every value
  must still come out right, also with Maps chained and over a Filter, and
  a sequence used twice (once, then asked again after its end).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/seqmap.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/seqmap.expected
}
{$H+}
program seqmap;
uses SysUtils, sequences;

type
  TRec = record
    Name: string;
    Tags: specialize TArray<string>;
    N: Integer;
  end;

var
  Names: specialize TArray<string>;
  Recs: specialize TArray<TRec>;
  Lens: specialize TArray<Integer>;
  Up: specialize TArray<string>;
  I: Integer;
  S: specialize TSeq<string>;
  X: string;
begin
  Names := nil;
  SetLength(Names, 5);
  Names[0] := 'ann'; Names[1] := ''; Names[2] := 'cy' + 'd';
  Names[3] := 'a longer name than the others'; Names[4] := 'e';

  Lens := Names.Seq.specialize Map<Integer>(function(const x: string): Integer => Length(x)).ToArray;
  for I := 0 to High(Lens) do
    write(Lens[I], ' ');
  writeln;

  Up := Names.Seq.specialize Map<string>(function(const x: string): string => '<' + UpperCase(x) + '>').ToArray;
  for I := 0 to High(Up) do
    write(Up[I], ' ');
  writeln;

  { chained, and over a Filter }
  Up := Names.Seq.Filter(function(const x: string): Boolean => Length(x) > 0)
          .specialize Map<string>(function(const x: string): string => x + x)
          .specialize Map<string>(function(const x: string): string => x + '!').ToArray;
  for I := 0 to High(Up) do
    write(Up[I], ' ');
  writeln;

  { records with a string and an array inside }
  Recs := nil;
  SetLength(Recs, 3);
  for I := 0 to 2 do
  begin
    Recs[I].Name := 'rec' + IntToStr(I);
    Recs[I].N := I * 10;
    SetLength(Recs[I].Tags, I);
    if I > 0 then
      Recs[I].Tags[I - 1] := 'tag' + IntToStr(I);
  end;
  Up := Recs.Seq.specialize Map<string>(function(const r: TRec): string =>
          r.Name + ':' + IntToStr(r.N) + ':' + IntToStr(Length(r.Tags))).ToArray;
  for I := 0 to High(Up) do
    write(Up[I], ' ');
  writeln;

  { the values the sequence read are not changed, and asking again after
    the end gives the end again }
  S := Names.Seq.specialize Map<string>(function(const x: string): string => x + '?');
  while S.Next(X) do
    write('[', X, ']');
  writeln;
  writeln(S.Next(X), ' ', Names[0], ' ', Names[4]);
  S.Next := nil;
end.
