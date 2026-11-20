{ Seq(open array) copies the array (docs/sequences-review.md SQ-7): with one
  move for the element types that hold no reference, item by item for the
  others (string, record, enumeration, ...). Each kind: the values, an
  empty array, a later change to the source not seen, and a large array.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/seqcopy.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/seqcopy.expected
}
{$H+}
program seqcopy;
uses SysUtils, sequences;

type
  TColor = (cRed, cGreen, cBlue);
  TRec = record
    Name: string;
    N: Integer;
  end;

var
  I: Integer;
  Ints: specialize TArray<Integer>;
  Big: specialize TArray<Int64>;
  Strs: specialize TArray<string>;
  Recs: specialize TArray<TRec>;
  S1: specialize TSeq<Integer>;
  Total: Int64;

procedure Show(const What: string; const S: specialize TSeq<Integer>);
var
  A: specialize TArray<Integer>;
  K: Integer;
begin
  A := S.ToArray;
  write(What, ' (', Length(A), '):');
  for K := 0 to High(A) do
    write(' ', A[K]);
  writeln;
end;

begin
  Show('integers', specialize Seq<Integer>([3, 1, 4, 1, 5]));
  Show('empty', specialize Seq<Integer>([]));
  Show('bytes', specialize Seq<Byte>([200, 7, 255]).specialize Map<Integer>(function(const x: Byte): Integer => x));
  Show('words', specialize Seq<Word>([65535, 1]).specialize Map<Integer>(function(const x: Word): Integer => x));
  Show('smallints', specialize Seq<SmallInt>([-32768, 5]).specialize Map<Integer>(function(const x: SmallInt): Integer => x));
  Show('shortints', specialize Seq<ShortInt>([-128, 127]).specialize Map<Integer>(function(const x: ShortInt): Integer => x));
  Show('cardinals', specialize Seq<Cardinal>([4000000000, 9]).specialize Map<Integer>(function(const x: Cardinal): Integer => x div 1000));
  Show('chars', specialize Seq<Char>(['a', 'b', 'z']).specialize Map<Integer>(function(const x: Char): Integer => Ord(x)));
  Show('booleans', specialize Seq<Boolean>([True, False, True]).specialize Map<Integer>(function(const x: Boolean): Integer => Ord(x)));
  Show('doubles', specialize Seq<Double>([1.5, -2.25, 1e3]).specialize Map<Integer>(function(const x: Double): Integer => Round(x * 4)));
  Show('enumeration', specialize Seq<TColor>([cBlue, cRed, cGreen]).specialize Map<Integer>(function(const x: TColor): Integer => Ord(x)));
  Show('strings', specialize Seq<string>(['ab', '', 'cde']).specialize Map<Integer>(function(const x: string): Integer => Length(x)));
  Show('int64s', specialize Seq<Int64>([5000000000, -5000000000]).specialize Map<Integer>(function(const x: Int64): Integer => x div 1000000000));
  Show('qwords', specialize Seq<QWord>([18446744073709551615, 2]).specialize Map<Integer>(function(const x: QWord): Integer => x mod 1000));

  { records: a string inside, copied item by item }
  Recs := nil;
  SetLength(Recs, 2);
  Recs[0].Name := 'x' + 'y';
  Recs[0].N := 7;
  Recs[1].Name := 'abc';
  Recs[1].N := 9;
  Show('records', specialize Seq<TRec>(Recs).specialize Map<Integer>(function(const r: TRec): Integer => Length(r.Name) * 10 + r.N));

  { the copy does not see later changes to the source }
  Ints := nil;
  SetLength(Ints, 3);
  Ints[0] := 1; Ints[1] := 2; Ints[2] := 3;
  S1 := specialize Seq<Integer>(Ints);
  Ints[1] := 99;
  Show('after change', S1);
  Strs := nil;
  SetLength(Strs, 2);
  Strs[0] := 'a' + 'b';
  Strs[1] := 'c';
  Show('strings after change', specialize Seq<string>(Strs).specialize Map<Integer>(function(const x: string): Integer => Length(x)));

  { a large array, with a sum check }
  Big := nil;
  SetLength(Big, 100000);
  for I := 0 to High(Big) do
    Big[I] := Int64(I) * 3;
  Total := specialize Seq<Int64>(Big).specialize Fold<Int64>(0, function(const a: Int64; const x: Int64): Int64 => a + x);
  writeln('large: ', Total);
  S1.Next := nil;
end.
