{ p3's Sequences: lazy, chainable operations on sequences (item 15 of the
  "Modernizing Pascal" proposals). A TSeq<T> produces its values one at a
  time, when they are asked for; operations such as Map, Filter and Take
  make new sequences without doing any work, and the work is done by the
  operations that consume them (ToArray, Fold, Count, a for-in loop).

    var names := Seq(customers)
      .Filter(function(c) => c.Active)
      .OrderBy(function(c) => c.Name)
      .Map(function(c) => c.Name)
      .ToArray;

  Arrays get the same operations directly (a generic type helper for
  array of T), so customers.Filter(...) works too. Needs --mode=objfpc. }
unit sequences;

{$H+}
{$modeswitch advancedrecords}
{$modeswitch typehelpers}

interface

{$IFNDEF P3_OBJFPC}
uses sequences_needs_mode_objfpc;
{$ELSE}

uses SysUtils;

type
  { First (and Last) of an empty sequence. }
  ESequenceEmpty = class(Exception);

  generic TFunc<T, R> = reference to function(const X: T): R;
  generic TPred<T> = reference to function(const X: T): Boolean;
  generic TStep<A, T> = reference to function(const Acc: A; const X: T): A;
  generic TProc<T> = reference to procedure(const X: T);
  { A sequence's source: the next value into X, or False at the end. }
  generic TNext<T> = reference to function(var X: T): Boolean;
  generic TArray<T> = array of T;

  { GroupBy's groups: a key and the values that have it, in order. }
  generic TGroup<K, T> = record
    Key: K;
    Items: specialize TArray<T>;
  end;

  { for x in seq do: the values of a sequence }
  generic TSeqEnum<T> = record
    Next: specialize TNext<T>;
    Cur: T;
    function MoveNext: Boolean;
    property Current: T read Cur;
  end;

  generic TSeq<T> = record
    Next: specialize TNext<T>;
    function GetEnumerator: specialize TSeqEnum<T>;

    { lazy: new sequences, no work done yet }
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
    function Filter(F: specialize TPred<T>): specialize TSeq<T>;
    function Take(N: Integer): specialize TSeq<T>;
    function Skip(N: Integer): specialize TSeq<T>;
    function TakeWhile(F: specialize TPred<T>): specialize TSeq<T>;
    function SkipWhile(F: specialize TPred<T>): specialize TSeq<T>;
    function Concat(const Other: specialize TSeq<T>): specialize TSeq<T>;
    { pairs, as long as the shorter }
    generic function Zip<B>(const Other: specialize TSeq<B>): specialize TSeq<(T, B)>;
    { the first value of each key }
    generic function DistinctBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;

    { these read the whole sequence when their result is used }
    { stable: equal keys keep their order }
    generic function OrderBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
    generic function OrderByDescending<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
    { the groups in the order their keys first appear }
    generic function GroupBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<specialize TGroup<K, T>>;

    { these consume it now }
    generic function Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
    function ToArray: specialize TArray<T>;
    function Count: Integer;
    function Any(F: specialize TPred<T>): Boolean;
    function All(F: specialize TPred<T>): Boolean;
    { raise ESequenceEmpty for an empty sequence }
    function First: T;
    function Last: T;
    procedure ForEach(F: specialize TProc<T>);
  end;

  { Every dynamic array is a sequence: arr.Seq, and the common operations
    directly on it. }
  generic TArraySeq<T> = type helper for array of T
    function Seq: specialize TSeq<T>;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
    function Filter(F: specialize TPred<T>): specialize TSeq<T>;
    function Take(N: Integer): specialize TSeq<T>;
    function Skip(N: Integer): specialize TSeq<T>;
    generic function OrderBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
    generic function GroupBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<specialize TGroup<K, T>>;
    generic function Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
  end;

  { The operations of an iterator (a function whose result is iterator T,
    with yield). An iterator runs once: the sequences made from it share
    it, and each value read from one is gone from the others. }
  generic TIterSeq<T> = type helper for iterator T
    { runs it to its next value: False at its end }
    function MoveNext: Boolean;
    { the value MoveNext ran it to }
    function Current: T;
    function Seq: specialize TSeq<T>;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
    function Filter(F: specialize TPred<T>): specialize TSeq<T>;
    function Take(N: Integer): specialize TSeq<T>;
    function Skip(N: Integer): specialize TSeq<T>;
    function TakeWhile(F: specialize TPred<T>): specialize TSeq<T>;
    generic function Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
    function ToArray: specialize TArray<T>;
  end;

{ The values of an array (copied: later changes to it are not seen). }
generic function Seq<T>(const Items: array of T): specialize TSeq<T>;
{ Lo, Lo + 1, ..., Hi }
function Range(Lo, Hi: Integer): specialize TSeq<Integer>;
{ Seed, F(Seed), F(F(Seed)), ...: endless (use Take or TakeWhile) }
generic function Iterate<T>(const Seed: T; F: specialize TFunc<T, T>): specialize TSeq<T>;
{ X, N times }
generic function Replicate<T>(const X: T; N: Integer): specialize TSeq<T>;

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}

{ Hashing of keys, for DistinctBy and GroupBy (docs/sequences-review.md
  SQ-2, SQ-3). KeyHash gives a hash that is the same for keys that are
  equal, and True, for the key types it knows: the integer kinds, string,
  Char, Boolean, pointers and classes (by reference). For any other type
  the untyped overload is chosen: False, and the caller searches the keys
  it has one by one, as it always did. }
type
  PHashByte = ^Byte;

{$Q-}{$R-}
function Mix64(V: QWord): Cardinal;
begin
  V := V xor (V shr 33);
  V := V * QWord($FF51AFD7ED558CCD);
  V := V xor (V shr 33);
  V := V * QWord($C4CEB9FE1A85EC53);
  V := V xor (V shr 33);
  Result := Cardinal(V and $FFFFFFFF);
end;

function KeyHash(const X: Int64; out H: Cardinal): Boolean; overload;
begin
  H := Mix64(QWord(X));
  Result := True;
end;

function KeyHash(const X: QWord; out H: Cardinal): Boolean; overload;
begin
  H := Mix64(X);
  Result := True;
end;

function KeyHash(const X: Char; out H: Cardinal): Boolean; overload;
begin
  H := Mix64(Ord(X));
  Result := True;
end;

function KeyHash(const X: Boolean; out H: Cardinal): Boolean; overload;
begin
  H := Mix64(Ord(X));
  Result := True;
end;

function KeyHash(const X: Pointer; out H: Cardinal): Boolean; overload;
begin
  H := Mix64(QWord(PHashByte(X) - PHashByte(nil)));
  Result := True;
end;

function KeyHash(const X: string; out H: Cardinal): Boolean; overload;
var
  I: Integer;
  V: Cardinal;
begin
  V := 2166136261;
  for I := 1 to Length(X) do
    V := (V xor Cardinal(Ord(X[I]))) * 16777619;
  H := Mix64(V);
  Result := True;
end;

function KeyHash(const X; out H: Cardinal): Boolean; overload;
begin
  H := 0;
  Result := False;
end;
{$Q+}{$R+}

{ The distinct keys seen so far, in the order they first came: Find gives a
  key's index, adding the key if it is new. A key with a hash is looked up
  in an open-addressing table (Slots holds an index + 1, 0 is empty; the
  table is a power of 2, at least twice the keys); any other is searched for in Keys one
  by one, the key of the last Find first, which makes runs of one key cheap. }
type
  generic TKeyIndex<K> = record
    Keys: specialize TArray<K>;
    Hashes: specialize TArray<Cardinal>;
    Slots: specialize TArray<Integer>;
    Count, Last: Integer;
    function Find(const KY: K; out Added: Boolean): Integer;
    procedure Rehash(NewSize: Integer);
  end;

procedure TKeyIndex.Rehash(NewSize: Integer);
var
  I, Slot: Integer;
begin
  Slots := nil;
  SetLength(Slots, NewSize);
  for I := 0 to Count - 1 do
  begin
    Slot := Integer(Hashes[I] and Cardinal(NewSize - 1));
    while Slots[Slot] <> 0 do
      Slot := (Slot + 1) and (NewSize - 1);
    Slots[Slot] := I + 1;
  end;
end;

function TKeyIndex.Find(const KY: K; out Added: Boolean): Integer;
var
  H: Cardinal;
  Slot, J: Integer;
  List: specialize TArray<K>;
begin
  Added := False;
  if KeyHash(KY, H) then
  begin
    if Length(Slots) < 2 * (Count + 1) then
      if Length(Slots) = 0 then
        Rehash(16)
      else
        Rehash(Length(Slots) * 2);
    Slot := Integer(H and Cardinal(Length(Slots) - 1));
    while Slots[Slot] <> 0 do
    begin
      J := Slots[Slot] - 1;
      if (Hashes[J] = H) and (Keys[J] = KY) then
        exit(J);
      Slot := (Slot + 1) and (Length(Slots) - 1);
    end;
    Slots[Slot] := Count + 1;
  end
  else
  begin
    if (Last < Count) and (Keys[Last] = KY) then
      exit(Last);
    List := Keys;
    J := 0;
    while (J < Count) and not (List[J] = KY) do
      Inc(J);
    if J < Count then
    begin
      Last := J;
      exit(J);
    end;
  end;
  if Count = Length(Keys) then
  begin
    SetLength(Keys, 2 * Count + 4);
    SetLength(Hashes, 2 * Count + 4);
  end;
  Keys[Count] := KY;
  Hashes[Count] := H;
  Last := Count;
  Result := Count;
  Inc(Count);
  Added := True;
end;

function TSeqEnum.MoveNext: Boolean;
begin
  Result := Next(Cur);
end;

function TSeq.GetEnumerator: specialize TSeqEnum<T>;
begin
  Result.Next := Next;
end;

generic function TSeq.Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
var
  Src: specialize TNext<T>;
begin
  Src := Next;
  Result.Next := function(var Y: R): Boolean => begin
      var X: T;
      Result := Src(X);
      if Result then
        Y := F(X);
    end;
end;

function TSeq.Filter(F: specialize TPred<T>): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
begin
  Src := Next;
  Result.Next := function(var Y: T): Boolean => begin
      Result := Src(Y);
      while Result and not F(Y) do
        Result := Src(Y);
    end;
end;

function TSeq.Take(N: Integer): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
  Left: Integer;
begin
  Src := Next;
  Left := N;
  Result.Next := function(var Y: T): Boolean => begin
      Result := (Left > 0) and Src(Y);
      if Result then
        Dec(Left)
      else
        Left := 0; { (the source is not asked again) }
    end;
end;

function TSeq.Skip(N: Integer): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
  Left: Integer;
begin
  Src := Next;
  Left := N;
  Result.Next := function(var Y: T): Boolean => begin
      while Left > 0 do
      begin
        Dec(Left);
        if not Src(Y) then
        begin
          Left := 0;
          exit(False);
        end;
      end;
      Result := Src(Y);
    end;
end;

function TSeq.TakeWhile(F: specialize TPred<T>): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
  Done: Boolean;
begin
  Src := Next;
  Done := False;
  Result.Next := function(var Y: T): Boolean => begin
      Result := not Done and Src(Y) and F(Y);
      if not Result then
        Done := True;
    end;
end;

function TSeq.SkipWhile(F: specialize TPred<T>): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
  Skipping: Boolean;
begin
  Src := Next;
  Skipping := True;
  Result.Next := function(var Y: T): Boolean => begin
      Result := Src(Y);
      if Skipping then
      begin
        while Result and F(Y) do
          Result := Src(Y);
        Skipping := False;
      end;
    end;
end;

function TSeq.Concat(const Other: specialize TSeq<T>): specialize TSeq<T>;
var
  First_, Second: specialize TNext<T>;
  InFirst: Boolean;
begin
  First_ := Next;
  Second := Other.Next;
  InFirst := True;
  Result.Next := function(var Y: T): Boolean => begin
      if InFirst then
      begin
        if First_(Y) then
          exit(True);
        InFirst := False;
      end;
      Result := Second(Y);
    end;
end;

generic function TSeq.Zip<B>(const Other: specialize TSeq<B>): specialize TSeq<(T, B)>;
var
  N1: specialize TNext<T>;
  N2: specialize TNext<B>;
begin
  N1 := Next;
  N2 := Other.Next;
  Result.Next := function(var Y: (T, B)): Boolean => begin
      var A: T;
      var C: B;
      Result := N1(A) and N2(C);
      if Result then
        Y := (A, C);
    end;
end;

generic function TSeq.DistinctBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
  Seen: specialize TKeyIndex<K>;
begin
  Src := Next;
  Result.Next := function(var Y: T): Boolean => begin
      Result := Src(Y);
      while Result do
      begin
        var Added: Boolean;
        Seen.Find(Key(Y), Added);
        if Added then
          exit(True);
        Result := Src(Y);
      end;
    end;
end;

{ The values of a, in order, as a sequence of its own (a is kept). }
generic function SeqOfArray<T>(const A: specialize TArray<T>): specialize TSeq<T>;
var
  Items: specialize TArray<T>;
  Pos: Integer;
begin
  Items := A;
  Pos := 0;
  Result.Next := function(var Y: T): Boolean => begin
      Result := Pos < Length(Items);
      if Result then
      begin
        Y := Items[Pos];
        Inc(Pos);
      end;
    end;
end;

{ Stable merge sort of Items[0..N-1] by Keys (moved along with them): a
  value goes after the ones with an equal key, so only < is needed on K.
  Descending puts the larger keys first, equal keys still in their order. }
generic procedure SortByKey<T, K>(var Items: specialize TArray<T>;
  var Keys: specialize TArray<K>; N: Integer; Descending: Boolean);
var
  A, B, Tmp: specialize TArray<T>;
  KA, KB, KTmp: specialize TArray<K>;
  Width, Lo, Mid, Hi, L, R, O: Integer;
  TakeRight: Boolean;
begin
  if N < 2 then
    exit;
  A := Items;
  KA := Keys;
  B := nil;
  KB := nil;
  SetLength(B, N);
  SetLength(KB, N);
  Width := 1;
  while Width < N do
  begin
    Lo := 0;
    while Lo < N do
    begin
      Mid := Lo + Width;
      if Mid > N then
        Mid := N;
      Hi := Mid + Width;
      if Hi > N then
        Hi := N;
      L := Lo;
      R := Mid;
      O := Lo;
      while (L < Mid) and (R < Hi) do
      begin
        if Descending then
          TakeRight := KA[L] < KA[R]
        else
          TakeRight := KA[R] < KA[L];
        if TakeRight then
        begin
          B[O] := A[R];
          KB[O] := KA[R];
          Inc(R);
        end
        else
        begin
          B[O] := A[L];
          KB[O] := KA[L];
          Inc(L);
        end;
        Inc(O);
      end;
      while L < Mid do
      begin
        B[O] := A[L];
        KB[O] := KA[L];
        Inc(L);
        Inc(O);
      end;
      while R < Hi do
      begin
        B[O] := A[R];
        KB[O] := KA[R];
        Inc(R);
        Inc(O);
      end;
      Lo := Hi;
    end;
    Tmp := A;
    A := B;
    B := Tmp;
    KTmp := KA;
    KA := KB;
    KB := KTmp;
    Width := Width * 2;
  end;
  Items := A;
  Keys := KA;
end;

{ OrderBy and OrderByDescending: nothing is read until the first value is
  asked for; then the whole source is read (the arrays grow by doubling)
  and sorted, and given out in order. The sorted values are released after
  the last one. }
generic function OrderedSeq<T, K>(Src: specialize TNext<T>;
  Key: specialize TFunc<T, K>; Descending: Boolean): specialize TSeq<T>;
var
  Sorted: specialize TArray<T>;
  Pos, N: Integer;
  Done: Boolean;
begin
  Done := False;
  Pos := 0;
  N := 0;
  Result.Next := function(var Y: T): Boolean => begin
      if not Done then
      begin
        var Keys: specialize TArray<K>;
        var X: T;
        Sorted := nil;
        Keys := nil;
        while Src(X) do
        begin
          if N = Length(Sorted) then
          begin
            SetLength(Sorted, 2 * N + 4);
            SetLength(Keys, 2 * N + 4);
          end;
          Sorted[N] := X;
          Keys[N] := Key(X);
          Inc(N);
        end;
        SetLength(Sorted, N);
        SetLength(Keys, N);
        specialize SortByKey<T, K>(Sorted, Keys, N, Descending);
        Done := True;
      end;
      Result := Pos < N;
      if Result then
      begin
        Y := Sorted[Pos];
        Inc(Pos);
        if Pos = N then
          Sorted := nil;
      end;
    end;
end;

generic function TSeq.OrderBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
begin
  Result := specialize OrderedSeq<T, K>(Next, Key, False);
end;

generic function TSeq.OrderByDescending<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
begin
  Result := specialize OrderedSeq<T, K>(Next, Key, True);
end;

generic function TSeq.GroupBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<specialize TGroup<K, T>>;
var
  Src: specialize TNext<T>;
  Groups: specialize TArray<specialize TGroup<K, T>>;
  Index: specialize TKeyIndex<K>;
  Pos: Integer;
  Done: Boolean;
begin
  Src := Next;
  Done := False;
  Pos := 0;
  Result.Next := function(var Y: specialize TGroup<K, T>): Boolean => begin
      if not Done then
      begin
        var X: T;
        Groups := nil;
        while Src(X) do
        begin
          var Added: Boolean;
          var G := Index.Find(Key(X), Added);
          if Added then
          begin
            SetLength(Groups, G + 1);
            Groups[G].Key := Index.Keys[G];
          end;
          var N := Length(Groups[G].Items);
          SetLength(Groups[G].Items, N + 1);
          Groups[G].Items[N] := X;
        end;
        Done := True;
      end;
      Result := Pos < Length(Groups);
      if Result then
      begin
        Y := Groups[Pos];
        Inc(Pos);
      end;
    end;
end;

generic function TSeq.Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
var
  X: T;
begin
  Result := Start;
  while Next(X) do
    Result := F(Result, X);
end;

function TSeq.ToArray: specialize TArray<T>;
var
  X: T;
  N: Integer;
begin
  Result := nil;
  N := 0;
  while Next(X) do
  begin
    if N = Length(Result) then
      SetLength(Result, 2 * N + 4);
    Result[N] := X;
    Inc(N);
  end;
  SetLength(Result, N);
end;

function TSeq.Count: Integer;
var
  X: T;
begin
  Result := 0;
  while Next(X) do
    Inc(Result);
end;

function TSeq.Any(F: specialize TPred<T>): Boolean;
var
  X: T;
begin
  while Next(X) do
    if F(X) then
      exit(True);
  Result := False;
end;

function TSeq.All(F: specialize TPred<T>): Boolean;
var
  X: T;
begin
  while Next(X) do
    if not F(X) then
      exit(False);
  Result := True;
end;

function TSeq.First: T;
var
  X: T;
begin
  if not Next(X) then
    raise ESequenceEmpty.Create('First of an empty sequence');
  Result := X;
end;

function TSeq.Last: T;
var
  X: T;
  Any_: Boolean;
begin
  Any_ := False;
  while Next(X) do
  begin
    Result := X;
    Any_ := True;
  end;
  if not Any_ then
    raise ESequenceEmpty.Create('Last of an empty sequence');
end;

procedure TSeq.ForEach(F: specialize TProc<T>);
var
  X: T;
begin
  while Next(X) do
    F(X);
end;

generic function Seq<T>(const Items: array of T): specialize TSeq<T>;
var
  A: specialize TArray<T>;
  I: Integer;
begin
  SetLength(A, Length(Items));
  for I := 0 to High(Items) do
    A[I] := Items[I];
  Result := specialize SeqOfArray<T>(A);
end;

function Range(Lo, Hi: Integer): specialize TSeq<Integer>;
var
  Cur: Integer;
begin
  Cur := Lo;
  Result.Next := function(var Y: Integer): Boolean => begin
      Result := Cur <= Hi;
      if Result then
      begin
        Y := Cur;
        Inc(Cur);
      end;
    end;
end;

generic function Iterate<T>(const Seed: T; F: specialize TFunc<T, T>): specialize TSeq<T>;
var
  Cur: T;
  Started: Boolean;
begin
  Cur := Seed;
  Started := False;
  Result.Next := function(var Y: T): Boolean => begin
      if Started then
        Cur := F(Cur);
      Started := True;
      Y := Cur;
      Result := True;
    end;
end;

generic function Replicate<T>(const X: T; N: Integer): specialize TSeq<T>;
var
  Left: Integer;
  V: T;
begin
  Left := N;
  V := X;
  Result.Next := function(var Y: T): Boolean => begin
      Result := Left > 0;
      if Result then
      begin
        Y := V;
        Dec(Left);
      end;
    end;
end;

function TIterSeq.MoveNext: Boolean;
begin
  Result := IteratorNext(Self);
end;

function TIterSeq.Current: T;
begin
  Result := IteratorValue(Self);
end;

function TIterSeq.Seq: specialize TSeq<T>;
var
  It: iterator T;
begin
  It := Self;
  Result.Next := function(var X: T): Boolean => begin
      Result := IteratorNext(It);
      if Result then X := IteratorValue(It);
    end;
end;

generic function TIterSeq.Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
begin
  Result := Seq.specialize Map<R>(F);
end;

function TIterSeq.Filter(F: specialize TPred<T>): specialize TSeq<T>;
begin
  Result := Seq.Filter(F);
end;

function TIterSeq.Take(N: Integer): specialize TSeq<T>;
begin
  Result := Seq.Take(N);
end;

function TIterSeq.Skip(N: Integer): specialize TSeq<T>;
begin
  Result := Seq.Skip(N);
end;

function TIterSeq.TakeWhile(F: specialize TPred<T>): specialize TSeq<T>;
begin
  Result := Seq.TakeWhile(F);
end;

generic function TIterSeq.Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
begin
  Result := Seq.specialize Fold<A>(Start, F);
end;

function TIterSeq.ToArray: specialize TArray<T>;
begin
  Result := Seq.ToArray;
end;

function TArraySeq.Seq: specialize TSeq<T>;
begin
  Result := specialize SeqOfArray<T>(Self);
end;

generic function TArraySeq.Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
begin
  Result := Seq.specialize Map<R>(F);
end;

function TArraySeq.Filter(F: specialize TPred<T>): specialize TSeq<T>;
begin
  Result := Seq.Filter(F);
end;

function TArraySeq.Take(N: Integer): specialize TSeq<T>;
begin
  Result := Seq.Take(N);
end;

function TArraySeq.Skip(N: Integer): specialize TSeq<T>;
begin
  Result := Seq.Skip(N);
end;

generic function TArraySeq.OrderBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
begin
  Result := Seq.specialize OrderBy<K>(Key);
end;

generic function TArraySeq.GroupBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<specialize TGroup<K, T>>;
begin
  Result := Seq.specialize GroupBy<K>(Key);
end;

generic function TArraySeq.Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
begin
  Result := Seq.specialize Fold<A>(Start, F);
end;

{$ENDIF}

end.
