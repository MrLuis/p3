{ DistinctBy and GroupBy (docs/sequences-review.md SQ-2, SQ-3) look a key up
  in a hash table when the unit can hash its type (the integer kinds,
  string, Char, Boolean, pointers and classes) and search the keys one by
  one for any other (an enumeration). Every case is compared with a
  reference written here: the first value of each key, the groups in the
  order their keys first appear, the values in their order in each group.
  Keys that are equal strings in different allocations, many keys, one
  key, no values and clusters of one key are all in.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/seqkeys.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/seqkeys.expected
}
{$H+}
program seqkeys;
uses SysUtils, sequences;

type
  TColor = (cRed, cGreen, cBlue, cBlack);
  TObj = class
  end;
  TInts = specialize TArray<Integer>;

var
  seed: longint;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := (seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := (seed shr 8) mod n;
end;
{$Q+}{$R+}

{ Keys[I] is the key of value I. DistinctBy gives the indexes of the first
  value of each key; GroupBy, the indexes of each group, in order. Both are
  compared with a search written here. }
generic procedure CheckKeys<K>(const What: string; const AKeys: specialize TArray<K>);
var
  Keys: specialize TArray<K>;
  KeyNo, First, Sizes, GotFirst, GotItems: TInts;
  Groups: specialize TArray<specialize TGroup<K, Integer>>;
  I, J, G, N, Bad: Integer;
begin
  Keys := AKeys;
  N := Length(Keys);
  KeyNo := nil; First := nil; Sizes := nil;
  SetLength(KeyNo, N);
  for I := 0 to N - 1 do
  begin
    J := 0;
    while (J < I) and not (Keys[J] = Keys[I]) do
      Inc(J);
    if J = I then
    begin
      KeyNo[I] := Length(First);
      SetLength(First, Length(First) + 1);
      First[High(First)] := I;
      SetLength(Sizes, Length(Sizes) + 1);
    end
    else
      KeyNo[I] := KeyNo[J];
    Inc(Sizes[KeyNo[I]]);
  end;

  GotFirst := Range(0, N - 1).specialize DistinctBy<K>(function(const X: Integer): K => Keys[X]).ToArray;
  Groups := Range(0, N - 1).specialize GroupBy<K>(function(const X: Integer): K => Keys[X]).ToArray;

  Bad := 0;
  if (Length(GotFirst) <> Length(First)) or (Length(Groups) <> Length(First)) then
    Bad := 1
  else
    for G := 0 to High(First) do
    begin
      if GotFirst[G] <> First[G] then
        Inc(Bad);
      GotItems := Groups[G].Items;
      if Length(GotItems) <> Sizes[G] then
        Inc(Bad)
      else
      begin
        { in order, all with this key, the first one the first of the key }
        if GotItems[0] <> First[G] then
          Inc(Bad);
        for I := 1 to High(GotItems) do
          if (GotItems[I] <= GotItems[I - 1]) or (KeyNo[GotItems[I]] <> G) then
            Inc(Bad);
      end;
    end;
  writeln(What, ': ', Length(First), ' keys, ', Bad, ' differences');
end;

procedure Ints;
var
  A: TInts;
  I, N, Keys, Mode: Integer;
begin
  A := nil;
  for Mode := 0 to 6 do
  begin
    case Mode of
      0: begin N := 0; Keys := 1; end;
      1: begin N := 1; Keys := 1; end;
      2: begin N := 500; Keys := 1; end;
      3: begin N := 1000; Keys := 7; end;
      4: begin N := 5000; Keys := 3000; end;
      5: begin N := 4000; Keys := 100000; end;
      else begin N := 3000; Keys := 50; end;
    end;
    SetLength(A, N);
    for I := 0 to N - 1 do
      if Mode = 6 then
        A[I] := (I * Keys div (N + 1)) * 1000003
      else
        A[I] := Rnd(Keys) * 7919 - 100000;
    specialize CheckKeys<Integer>('integer, mode ' + IntToStr(Mode), A);
  end;
end;

procedure OtherInts;
var
  B: specialize TArray<Byte>;
  W: specialize TArray<Int64>;
  Q: specialize TArray<QWord>;
  C: specialize TArray<Cardinal>;
  I: Integer;
begin
  B := nil; W := nil; Q := nil; C := nil;
  SetLength(B, 2000); SetLength(W, 2000); SetLength(Q, 2000); SetLength(C, 2000);
  for I := 0 to 1999 do
  begin
    B[I] := Rnd(256);
    W[I] := Int64(Rnd(100000)) * 4294967311 - 5000000000000;
    Q[I] := QWord(Rnd(300)) * 36028797018963968;
    C[I] := Cardinal(Rnd(500)) * 8388608;
  end;
  specialize CheckKeys<Byte>('byte', B);
  specialize CheckKeys<Int64>('int64', W);
  specialize CheckKeys<QWord>('qword', Q);
  specialize CheckKeys<Cardinal>('cardinal', C);
end;

procedure Strings;
var
  S: specialize TArray<string>;
  I, J: Integer;
  T: string;
begin
  S := nil;
  SetLength(S, 3000);
  for I := 0 to 2999 do
  begin
    { equal strings built apart, so no two share a buffer }
    T := '';
    for J := 1 to Rnd(5) do
      T := T + Chr(Ord('a') + Rnd(3));
    S[I] := T + IntToStr(Rnd(400));
  end;
  specialize CheckKeys<string>('string', S);
  SetLength(S, 0);
  specialize CheckKeys<string>('string, empty', S);
  SetLength(S, 5);
  for I := 0 to 4 do
    S[I] := '';
  specialize CheckKeys<string>('string, empty keys', S);
end;

procedure Chars;
var
  C: specialize TArray<Char>;
  F: specialize TArray<Boolean>;
  I: Integer;
begin
  C := nil; F := nil;
  SetLength(C, 500);
  SetLength(F, 500);
  for I := 0 to 499 do
  begin
    C[I] := Chr(32 + Rnd(60));
    F[I] := Rnd(3) = 0;
  end;
  specialize CheckKeys<Char>('char', C);
  specialize CheckKeys<Boolean>('boolean', F);
end;

procedure Objects;
var
  Pool: array[0..39] of TObj;
  O: specialize TArray<TObj>;
  I: Integer;
begin
  O := nil;
  for I := 0 to 39 do
    Pool[I] := TObj.Create;
  SetLength(O, 800);
  for I := 0 to 799 do
    O[I] := Pool[Rnd(40)];
  specialize CheckKeys<TObj>('class', O);
  for I := 0 to 39 do
    Pool[I].Free;
end;

{ not hashed: searched one by one, as before }
procedure Unhashed;
var
  E: specialize TArray<TColor>;
  I: Integer;
begin
  E := nil;
  SetLength(E, 400);
  for I := 0 to 399 do
    E[I] := TColor(Rnd(4));
  specialize CheckKeys<TColor>('enumeration', E);
end;

{ hashed: +0.0 and -0.0 are one key }
procedure Floats;
var
  D: specialize TArray<Double>;
  I, K: Integer;
  Zero: Double;
begin
  D := nil;
  Zero := 0;
  SetLength(D, 2000);
  for I := 0 to 1999 do
  begin
    K := Rnd(30);
    D[I] := K / 4;
    if (K = 0) and (I mod 2 = 0) then
      D[I] := -Zero;
  end;
  specialize CheckKeys<Double>('double', D);
end;

begin
  seed := 11;
  Ints;
  OtherInts;
  Strings;
  Chars;
  Objects;
  Unhashed;
  Floats;
end.
