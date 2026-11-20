{ OrderBy and OrderByDescending (docs/sequences-review.md SQ-1): a stable
  merge sort. Each result is compared with a reference insertion sort
  written here, for sorted, reversed, random, all-equal, few-keys, empty
  and one-value input, in both directions; the Tag shows that equal keys
  keep their order. The source is read only when the first value is asked
  for.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/seqorder.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/seqorder.expected
}
{$H+}
program seqorder;
uses SysUtils, sequences;

type
  TItem = record
    Key, Tag: Integer;
  end;
  TItems = specialize TArray<TItem>;

var
  seed: longint;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := (seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := (seed shr 8) mod n;
end;
{$Q+}{$R+}

function Reference(const A: TItems; Desc: Boolean): TItems;
var
  I, J: Integer;
  X: TItem;
begin
  Result := nil;
  SetLength(Result, Length(A));
  for I := 0 to High(A) do
  begin
    X := A[I];
    J := I;
    while (J > 0) and ((not Desc and (X.Key < Result[J - 1].Key)) or
                       (Desc and (Result[J - 1].Key < X.Key))) do
    begin
      Result[J] := Result[J - 1];
      Dec(J);
    end;
    Result[J] := X;
  end;
end;

procedure Check(const What: string; const A: TItems);
var
  Asc, Desc, RA, RD: TItems;
  I, Bad: Integer;
begin
  Asc := A.Seq.specialize OrderBy<Integer>(function(const x: TItem): Integer => x.Key).ToArray;
  Desc := A.Seq.specialize OrderByDescending<Integer>(function(const x: TItem): Integer => x.Key).ToArray;
  RA := Reference(A, False);
  RD := Reference(A, True);
  Bad := 0;
  if (Length(Asc) <> Length(A)) or (Length(Desc) <> Length(A)) then
    Bad := 1
  else
    for I := 0 to High(A) do
      if (Asc[I].Key <> RA[I].Key) or (Asc[I].Tag <> RA[I].Tag) or
         (Desc[I].Key <> RD[I].Key) or (Desc[I].Tag <> RD[I].Tag) then
        Inc(Bad);
  writeln(What, ' (', Length(A), '): ', Bad, ' differences');
end;

function Make(N, Keys, Mode: Integer): TItems;
var
  I: Integer;
begin
  Result := nil;
  SetLength(Result, N);
  for I := 0 to N - 1 do
  begin
    Result[I].Tag := I;
    case Mode of
      0: Result[I].Key := I div (Keys + 1);
      1: Result[I].Key := (N - I) div (Keys + 1);
      else Result[I].Key := Rnd(Keys);
    end;
  end;
end;

procedure Lazy;
var
  Reads: Integer;
  S: specialize TSeq<Integer>;
begin
  Reads := 0;
  S := Range(1, 5).Map(function(x) => begin
                          Inc(Reads);
                          Result := x;
                        end).specialize OrderBy<Integer>(function(const x: Integer): Integer => -x);
  writeln('before: ', Reads);
  writeln('first: ', S.First, ' after: ', Reads);
  S.Next := nil;
end;

var
  N: Integer;
begin
  Check('empty', Make(0, 1, 2));
  Check('one', Make(1, 1, 2));
  Check('sorted', Make(100, 0, 0));
  Check('reversed', Make(100, 0, 1));
  Check('all equal', Make(100, 1, 2));
  Check('two keys', Make(257, 2, 2));
  Check('few keys', Make(1000, 10, 2));
  Check('random', Make(1000, 100000, 2));
  for N := 2 to 40 do
    Check('size', Make(N, 4, 2));
  Lazy;
end.
