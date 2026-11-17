{ Null-safe references (objfpc; item 2 of the proposals): under
  the nullsafe switch a plain class or interface type is never nil, T? may be;
  a nil check narrows a T? variable. A program that passes the checks: a
  linked list walked with narrowing, exit with a value, var parameters,
  'is', case, if-expressions, interfaces, class references, a constructor
  that calls another, a T? result narrowed.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/nullsafe.expected
}
{$H+}
program nullsafe;
{$nullsafe on}
uses SysUtils;

type
  TNode = class
    Value: Integer;
    Next: TNode?;
    constructor Create(AValue: Integer; ANext: TNode?);
    function Last: TNode;
  end;

  TList = class
    Head: TNode?;
    Name: string;
    constructor Create(const AName: string);
    destructor Destroy; override;
    procedure Push(V: Integer);
    function Find(V: Integer): TNode?;
    function Sum: Integer;
  end;

constructor TNode.Create(AValue: Integer; ANext: TNode?);
begin
  Value := AValue;
  Next := ANext;
end;

function TNode.Last: TNode;
var n: TNode;
begin
  n := Self;
  while n.Next <> nil do
  begin
    var nx := n.Next;
    if nx = nil then break;
    n := nx;
  end;
  Result := n;
end;

constructor TList.Create(const AName: string);
begin
  Name := AName;
end;

destructor TList.Destroy;
var n: TNode?;
begin
  while Head <> nil do
  begin
    n := Head;
    Head := n?.Next;
    n.Free;
  end;
  inherited;
end;

procedure TList.Push(V: Integer);
begin
  Head := TNode.Create(V, Head);
end;

function TList.Find(V: Integer): TNode?;
var n: TNode?;
begin
  n := Head;
  while n <> nil do
  begin
    if n.Value = V then
      exit(n);
    n := n.Next;
  end;
  Result := nil;
end;

function TList.Sum: Integer;
var n: TNode?;
begin
  Result := 0;
  n := Head;
  while Assigned(n) do
  begin
    Result := Result + n.Value;
    n := n.Next;
  end;
end;

function Describe(n: TNode?): string;
begin
  if n = nil then
    exit('none');
  Result := IntToStr(n.Value);
end;

procedure Swap(var a, b: TNode?);
var t: TNode?;
begin
  t := a;
  a := b;
  b := t;
end;

procedure Test;
var
  L: TList;
  found, other: TNode?;
  k: Integer;
begin
  L := TList.Create('numbers');
  for k := 1 to 5 do
    L.Push(k * 10);
  Writeln(L.Sum);
  found := L.Find(30);
  Writeln(Describe(found), ' ', Describe(L.Find(99)));
  Writeln(found?.Value ?? -1, ' ', L.Find(99)?.Value ?? -1);
  if (found <> nil) and (found.Next <> nil) then
    Writeln('after 30: ', Describe(found.Next));
  other := nil;
  Swap(found, other);
  Writeln(Describe(found), ' ', Describe(other));
  if other is TNode then
    Writeln('last: ', other.Last.Value);
  try
    raise Exception.Create('oops');
  except
    on E: Exception do
      Writeln(E.Message);
  end;
  case k of
    5: found := L.Head;
  else
    found := nil;
  end;
  Writeln(if found <> nil then found.Value else 0);
  L.Free;
end;

type
  IShape = interface
    function Area: Integer;
  end;
  TSquare = class(TInterfacedObject, IShape)
    Side: Integer;
    constructor Create(S: Integer);
    function Area: Integer;
  end;
  TSquareClass = class of TSquare;
  TBox = class
    Shape: IShape;
    Kind: TSquareClass;
    constructor Create;
    constructor CreateFrom(S: IShape);
  end;

constructor TSquare.Create(S: Integer);
begin
  Side := S;
end;

function TSquare.Area: Integer;
begin
  Result := Side * Side;
end;

constructor TBox.CreateFrom(S: IShape);
begin
  Shape := S;
  Kind := TSquare;
end;

constructor TBox.Create;
begin
  CreateFrom(TSquare.Create(2));    // sets the fields
end;

function Maybe(n: Integer): IShape?;
begin
  if n > 0 then
    Result := TSquare.Create(n);
  if Result <> nil then
    Writeln('made ', Result.Area);
end;

procedure Test2;
var
  s: IShape?;
  t: IShape;
  c: TSquareClass?;
begin
  s := Maybe(3);
  t := s ?? TSquare.Create(1);
  Writeln(t.Area, ' ', s?.Area ?? 0);
  c := TSquare;
  if c <> nil then
  begin
    t := c.Create(4);
    Writeln(t.Area);
  end;
  if s <> nil then
    t := s;
  Writeln(t.Area);
  Writeln(Maybe(0)?.Area ?? -1);
end;

begin
  Test;
  Test2;
end.
