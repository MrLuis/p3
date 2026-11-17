{ Generic inheritance (objfpc): class(specialize TBase<T>) and
  class(TInterfacedObject, specialize IIter<T>) in a class's heading,
  interface(specialize IBase<T>) in an interface's: a generic class
  implementing a generic interface, and inheriting from a generic class.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/genericinherit.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/genericinherit.expected
}
{$H+}
program genericinherit;
uses SysUtils;

type
  generic TFunc<T, R> = reference to function(const X: T): R;

  generic IIter<T> = interface
    function MoveNext: Boolean;
    function GetCurrent: T;
  end;
  generic IStack<T> = interface(specialize IIter<T>)
    procedure Push(const X: T);
  end;

  generic TArrayIter<T> = class(TInterfacedObject, specialize IIter<T>)
    FItems: array of T;
    FPos: Integer;
    constructor Create(const Items: array of T);
    function MoveNext: Boolean;
    function GetCurrent: T;
  end;

  generic TMapIter<T, R> = class(TInterfacedObject, specialize IIter<R>)
    FSrc: specialize IIter<T>;
    FF: specialize TFunc<T, R>;
    constructor Create(Src: specialize IIter<T>; F: specialize TFunc<T, R>);
    function MoveNext: Boolean;
    function GetCurrent: R;
  end;

  { generic inheritance: a generic class from a generic class }
  generic TBox<T> = class
    Value: T;
    function Show: string; virtual;
  end;
  generic TNamedBox<T> = class(specialize TBox<T>)
    Name: string;
    function Show: string; override;
  end;
  TIntBox = class(specialize TNamedBox<Integer>)
  end;

constructor TArrayIter.Create(const Items: array of T);
var
  I: Integer;
begin
  SetLength(FItems, Length(Items));
  for I := 0 to High(Items) do
    FItems[I] := Items[I];
  FPos := -1;
end;
function TArrayIter.MoveNext: Boolean;
begin
  Inc(FPos);
  Result := FPos < Length(FItems);
end;
function TArrayIter.GetCurrent: T;
begin
  Result := FItems[FPos];
end;

constructor TMapIter.Create(Src: specialize IIter<T>; F: specialize TFunc<T, R>);
begin
  FSrc := Src;
  FF := F;
end;
function TMapIter.MoveNext: Boolean;
begin
  Result := FSrc.MoveNext;
end;
function TMapIter.GetCurrent: R;
begin
  Result := FF(FSrc.GetCurrent);
end;

function TBox.Show: string;
begin
  Result := 'box';
end;
function TNamedBox.Show: string;
begin
  Result := Name + '=' + inherited Show;
end;

var
  it: specialize IIter<string>;
  b: specialize TBox<Integer>;
  ib: TIntBox;
begin
  it := specialize TMapIter<Integer, string>.Create(
          specialize TArrayIter<Integer>.Create([1, 2, 3]),
          function(const X: Integer): string => '#' + IntToStr(X * X));
  while it.MoveNext do
    Write(it.GetCurrent, ' ');
  Writeln;
  ib := TIntBox.Create;
  ib.Name := 'n';
  ib.Value := 7;
  b := ib;
  Writeln(b.Show, ' ', b.Value);
  ib.Free;
end.
