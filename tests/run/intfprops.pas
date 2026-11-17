{ Properties in interfaces (objfpc): property P: T read GetP write SetP,
  through the interface's methods, read and written on an interface value;
  for ... in over an interface whose GetEnumerator gives an interface with
  a Current property.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/intfprops.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/intfprops.expected
}
{$H+}
program intfprops;
uses SysUtils;

type
  generic IIter<T> = interface
    function MoveNext: Boolean;
    function GetCurrent: T;
    property Current: T read GetCurrent;
  end;
  generic IEnumerable<T> = interface
    function GetEnumerator: specialize IIter<T>;
  end;
  INamed = interface
    function GetName: string;
    procedure SetName(const Value: string);
    property Name: string read GetName write SetName;
  end;

  generic TRange<T> = class(TInterfacedObject, specialize IIter<Integer>)
    FCur, FHi: Integer;
    constructor Create(Lo, Hi: Integer);
    function MoveNext: Boolean;
    function GetCurrent: Integer;
  end;
  TNumbers = class(TInterfacedObject, specialize IEnumerable<Integer>)
    function GetEnumerator: specialize IIter<Integer>;
  end;
  TThing = class(TInterfacedObject, INamed)
    FName: string;
    function GetName: string;
    procedure SetName(const Value: string);
  end;

constructor TRange.Create(Lo, Hi: Integer);
begin
  FCur := Lo - 1;
  FHi := Hi;
end;
function TRange.MoveNext: Boolean;
begin
  Inc(FCur);
  Result := FCur <= FHi;
end;
function TRange.GetCurrent: Integer;
begin
  Result := FCur;
end;
function TNumbers.GetEnumerator: specialize IIter<Integer>;
begin
  Result := specialize TRange<Integer>.Create(1, 4);
end;
function TThing.GetName: string;
begin
  Result := FName;
end;
procedure TThing.SetName(const Value: string);
begin
  FName := UpperCase(Value);
end;

var
  it: specialize IIter<Integer>;
  nums: specialize IEnumerable<Integer>;
  n: INamed;
  i: Integer;
begin
  it := specialize TRange<Integer>.Create(5, 7);
  while it.MoveNext do
    Write(it.Current, ' ');
  Writeln;
  nums := TNumbers.Create;
  for i in nums do
    Write(i, ' ');
  Writeln;
  n := TThing.Create;
  n.Name := 'ann';
  Writeln(n.Name);
end.
