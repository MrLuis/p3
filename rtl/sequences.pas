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
  Seen: specialize TArray<K>;
begin
  Src := Next;
  Seen := nil;
  Result.Next := function(var Y: T): Boolean => begin
      Result := Src(Y);
      while Result do
      begin
        var KY := Key(Y);
        var Found := False;
        for var I := 0 to High(Seen) do
          if Seen[I] = KY then
          begin
            Found := True;
            break;
          end;
        if not Found then
        begin
          SetLength(Seen, Length(Seen) + 1);
          Seen[High(Seen)] := KY;
          exit(True);
        end;
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

generic function TSeq.OrderBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
  Sorted: specialize TArray<T>;
  Pos: Integer;
  Done: Boolean;
begin
  Src := Next;
  Done := False;
  Pos := 0;
  Result.Next := function(var Y: T): Boolean => begin
      if not Done then
      begin
        { read it all, then an insertion sort on the keys (stable) }
        var Keys: specialize TArray<K>;
        var X: T;
        Sorted := nil;
        Keys := nil;
        while Src(X) do
        begin
          var KX := Key(X);
          var J := Length(Sorted);
          SetLength(Sorted, J + 1);
          SetLength(Keys, J + 1);
          while (J > 0) and (KX < Keys[J - 1]) do
          begin
            Sorted[J] := Sorted[J - 1];
            Keys[J] := Keys[J - 1];
            Dec(J);
          end;
          Sorted[J] := X;
          Keys[J] := KX;
        end;
        Done := True;
      end;
      Result := Pos < Length(Sorted);
      if Result then
      begin
        Y := Sorted[Pos];
        Inc(Pos);
      end;
    end;
end;

generic function TSeq.OrderByDescending<K>(Key: specialize TFunc<T, K>): specialize TSeq<T>;
var
  Src: specialize TNext<T>;
  Sorted: specialize TArray<T>;
  Pos: Integer;
  Done: Boolean;
begin
  Src := Next;
  Done := False;
  Pos := 0;
  Result.Next := function(var Y: T): Boolean => begin
      if not Done then
      begin
        var Keys: specialize TArray<K>;
        var X: T;
        Sorted := nil;
        Keys := nil;
        while Src(X) do
        begin
          var KX := Key(X);
          var J := Length(Sorted);
          SetLength(Sorted, J + 1);
          SetLength(Keys, J + 1);
          while (J > 0) and (Keys[J - 1] < KX) do
          begin
            Sorted[J] := Sorted[J - 1];
            Keys[J] := Keys[J - 1];
            Dec(J);
          end;
          Sorted[J] := X;
          Keys[J] := KX;
        end;
        Done := True;
      end;
      Result := Pos < Length(Sorted);
      if Result then
      begin
        Y := Sorted[Pos];
        Inc(Pos);
      end;
    end;
end;

generic function TSeq.GroupBy<K>(Key: specialize TFunc<T, K>): specialize TSeq<specialize TGroup<K, T>>;
var
  Src: specialize TNext<T>;
  Groups: specialize TArray<specialize TGroup<K, T>>;
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
          var KX := Key(X);
          var G := 0;
          while (G < Length(Groups)) and not (Groups[G].Key = KX) do
            Inc(G);
          if G = Length(Groups) then
          begin
            SetLength(Groups, G + 1);
            Groups[G].Key := KX;
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
