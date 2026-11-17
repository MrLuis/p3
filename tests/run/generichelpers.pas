{ Generic type helpers (objfpc, pcc's own): generic TH<T> = type helper
  for array of T extends every dynamic array (T its element type), and a
  helper for specialize TGen<T> every specialization of TGen that matches.
  The helper is specialized where it is used; an exact (non-generic)
  helper comes first, then the generic ones, the last declared first (one
  helper applies to a type, as in Free Pascal).
  Generic methods in them (Map<R>) take their types from lambdas.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/generichelpers.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/generichelpers.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/generichelpers.expected
}
{$H+}
{$modeswitch typehelpers}
{$modeswitch advancedrecords}
program generichelpers;
uses SysUtils;

type
  generic TFunc<T, R> = reference to function(const X: T): R;
  generic TPred<T> = reference to function(const X: T): Boolean;

  generic TArrayHelper<T> = type helper for array of T
    function Count: Integer;
    function First: T;
    function Filter(F: specialize TPred<T>): specialize TArray<T>;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TArray<R>;
    function Join(const Sep: string): string;
  end;
  generic TArray<T> = array of T;

  generic TPair<A, B> = record
    Left: A;
    Right: B;
  end;
  generic TPairHelper<T> = record helper for specialize TPair<T, T>
    function Swapped: specialize TPair<T, T>;
  end;
  { an exact helper comes before the generic ones }
  TBytesHelper = type helper for array of Byte
    function Which: Integer;
  end;

function TArrayHelper.Count: Integer;
begin
  Result := Length(Self);
end;

function TArrayHelper.First: T;
begin
  Result := Self[0];
end;

function TArrayHelper.Filter(F: specialize TPred<T>): specialize TArray<T>;
var
  X: T;
begin
  Result := nil;
  for X in Self do
    if F(X) then
    begin
      SetLength(Result, Length(Result) + 1);
      Result[High(Result)] := X;
    end;
end;

generic function TArrayHelper.Map<R>(F: specialize TFunc<T, R>): specialize TArray<R>;
var
  I: Integer;
begin
  SetLength(Result, Length(Self));
  for I := 0 to High(Self) do
    Result[I] := F(Self[I]);
end;

function TArrayHelper.Join(const Sep: string): string;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to High(Self) do
  begin
    if I > 0 then
      Result := Result + Sep;
    Result := Result + Format('%s', [Self[I]]);
  end;
end;

function TBytesHelper.Which: Integer; begin Result := 3; end;

function TPairHelper.Swapped: specialize TPair<T, T>;
begin
  Result.Left := Right;
  Result.Right := Left;
end;

type
  TInts = specialize TArray<Integer>;
  TNames = array of string;
  TIntPair = specialize TPair<Integer, Integer>;
  TBytes = array of Byte;
var
  bytes: TBytes;
  ints: TInts;
  names: TNames;
  p: TIntPair;
begin
  ints := [1, 2, 3, 4, 5, 6];
  Writeln(ints.Count, ' ', ints.First);
  Writeln(ints.Filter(function(x) => not Odd(x)).Map(function(x) => 'n' + IntToStr(x)).Join(', '));
  names := ['ann', 'bob'];
  Writeln(names.Map(function(s) => Length(s) * 10).Count, ' ', names.Join('+'));
  p.Left := 1;
  p.Right := 2;
  p := p.Swapped;
  Writeln(p.Left, ' ', p.Right);
  SetLength(bytes, 2);
  Writeln(bytes.Which, ' ', Length(bytes));
end.
