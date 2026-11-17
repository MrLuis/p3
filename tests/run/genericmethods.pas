{ Generic methods (objfpc, as in Free Pascal 3.2): generic function M<T>
  in a class, generic function TFoo.M<T> its implementation, called as
  obj.specialize M<T>(args) or (pcc) with the types inferred from the
  arguments, obj.M(args); inside a method also without the object. Class
  methods, methods of generic classes (TList<T>.Map<R>) and inherited ones.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/genericmethods.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/genericmethods.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/genericmethods.expected
}
{$H+}
program genericmethods;

type
  TBox = class
  private
    FName: string;
  public
    constructor Create(const N: string);
    generic function Pair<T>(const X: T): string;
    generic function Pick<A, B>(Flag: Boolean; const X: A; const Y: B): string;
    generic class function Twice<T>(const X: T): T;
    generic procedure Show<T>(const X: T);
  end;

  generic TFunc<T, R> = reference to function(const X: T): R;
  generic TStep<A, T> = reference to function(const Acc: A; const X: T): A;

  generic TList<T> = class
  private
    FItems: array of T;
  public
    procedure Add(const X: T);
    function Count: Integer;
    function Get(I: Integer): T;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TList<R>;
    generic function Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
  end;

  TInts = specialize TList<Integer>;
  TStrs = specialize TList<string>;

  TBase = class
    generic function Tag<T>(const X: T): string;
  end;
  TDerived = class(TBase)
  end;

constructor TBox.Create(const N: string);
begin
  FName := N;
end;

generic function TBox.Pair<T>(const X: T): string;
begin
  Result := FName + ':' + Chr(48 + SizeOf(T));
  specialize Show<T>(X);   { (on Self) }
  Show(X);                 { (on Self, inferred) }
end;

generic function TBox.Pick<A, B>(Flag: Boolean; const X: A; const Y: B): string;
begin
  if Flag then
    Result := 'A'
  else
    Result := 'B';
end;

generic class function TBox.Twice<T>(const X: T): T;
begin
  Result := X + X;
end;

generic procedure TBox.Show<T>(const X: T);
begin
  Writeln(FName, ' shows ', X);
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

generic function TList.Fold<A>(const Start: A; F: specialize TStep<A, T>): A;
var
  X: T;
begin
  Result := Start;
  for X in FItems do
    Result := F(Result, X);
end;

generic function TBase.Tag<T>(const X: T): string;
begin
  Result := ClassName + ':' + X;
end;

var
  b: TBox;
  ints: TInts;
  strs: TStrs;
  i: Integer;
  d: TDerived;
begin
  b := TBox.Create('box');
  Writeln(b.specialize Pair<Integer>(42));
  Writeln(b.Pair(3.5));
  Writeln(b.Pick(True, 1, 'x'), b.Pick(False, 'y', 2.0));
  Writeln(TBox.specialize Twice<Integer>(21), ' ', TBox.Twice('ab'));
  b.Show('hello');
  b.specialize Show<Char>('c');
  b.Free;

  ints := TInts.Create;
  for i := 1 to 4 do
    ints.Add(i * i);
  strs := ints.specialize Map<string>(
    function(const X: Integer): string => '<' + Chr(48 + X mod 10) + '>');
  for i := 0 to strs.Count - 1 do
    Write(strs.Get(i), ' ');
  Writeln;
  Writeln(ints.Fold(0, function(const Acc: Integer; const X: Integer): Integer => Acc + X));
  Writeln(strs.Fold('', function(const Acc: string; const X: string): string => Acc + X));
  strs.Free;
  ints.Free;

  d := TDerived.Create;
  Writeln(d.Tag('x'));
  d.Free;
end.
