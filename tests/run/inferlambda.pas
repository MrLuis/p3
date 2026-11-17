{ Type inference through lambdas (objfpc, p3's own): a lambda passed
  for a parameter of a generic procedural type (specialize TFunc<T, R>)
  takes the types of its parameters from the type arguments known (T in,
  from the other arguments or the receiver's class) and gives the result's
  type argument by its body (R out). Types written in it give type
  arguments too.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/inferlambda.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/inferlambda.expected
}
{$H+}
program inferlambda;
uses SysUtils;

type
  generic TFunc<T, R> = reference to function(const X: T): R;
  generic TPred<T> = reference to function(const X: T): Boolean;
  generic TStep<A, T> = reference to function(const Acc: A; const X: T): A;
  TInts = array of Integer;

  generic TList<T> = class
  private
    FItems: array of T;
  public
    procedure Add(const X: T);
    function Count: Integer;
    function Get(I: Integer): T;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TList<R>;
    function Filter(F: specialize TPred<T>): specialize TList<T>;
    generic function Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
  end;

procedure TList.Add(const X: T);
begin
  SetLength(FItems, Length(FItems) + 1);
  FItems[High(FItems)] := X;
end;

function TList.Count: Integer;
begin
  Result := Length(FItems);
end;

function TList.Get(I: Integer): T;
begin
  Result := FItems[I];
end;

generic function TList.Map<R>(F: specialize TFunc<T, R>): specialize TList<R>;
var
  X: T;
begin
  Result := specialize TList<R>.Create;
  for X in FItems do
    Result.Add(F(X));
end;

function TList.Filter(F: specialize TPred<T>): specialize TList<T>;
var
  X: T;
begin
  Result := specialize TList<T>.Create;
  for X in FItems do
    if F(X) then
      Result.Add(X);
end;

generic function TList.Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
var
  X: T;
begin
  Result := Start;
  for X in FItems do
    Result := F(Result, X);
end;

{ a generic routine: T from the array, R from the lambda }
generic function MapArr<T, R>(const Xs: array of T; F: specialize TFunc<T, R>): string;
var
  X: T;
begin
  Result := '';
  for X in Xs do
    Result := Result + ' ' + F(X);
end;

generic function Apply<T, R>(F: specialize TFunc<T, R>; const X: T): R;
begin
  Result := F(X);
end;

type
  TIntList = specialize TList<Integer>;
  TCharList = specialize TList<Char>;
var
  ints, odds: TIntList;
  chars: TCharList;
  i: Integer;
begin
  ints := TIntList.Create;
  for i := 1 to 5 do
    ints.Add(i);
  { T in: x is an Integer; R out: string }
  with ints.Map(function(x) => '#' + IntToStr(x * x)) do
  begin
    for i := 0 to Count - 1 do
      Write(Get(i), ' ');
    Writeln;
    Free;
  end;
  odds := ints.Filter(function(x) => Odd(x));
  with odds.Map(function(x) => x * 2.5) do
  begin
    for i := 0 to Count - 1 do
      Write(Get(i):0:1, ' ');
    Writeln;
    Free;
  end;
  odds.Free;
  Writeln(ints.Fold(0, function(acc, x) => acc + x));
  Writeln(ints.Fold('', function(acc, x) => acc + IntToStr(x)));
  Writeln(MapArr([1, 2, 3], function(x) => IntToStr(x + 10)));
  Writeln(Apply(function(s) => Length(s) * 2, 'hello'));
  chars := ints.Map(function(const x: Integer): Char => Chr(64 + x));
  Writeln(chars.Get(2));
  chars.Free;
  ints.Free;
end.
