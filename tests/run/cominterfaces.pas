{ COM interfaces and TInterfacedObject (--mode=objfpc, where interfaces
  are COM style unless $interfaces corba): reference counting through
  local and global variables, value / const / var parameters, function
  results, fields, arrays and records (copied and finalized), properties,
  `dispose`; objects freed when their last reference goes away, also when
  an exception leaves a routine; RefCount, the count of 1 while a
  constructor runs, IInterface / IUnknown, is / as between interfaces and
  from an interface to a class. Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/cominterfaces.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/cominterfaces.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/cominterfaces.expected %t.g.out
}
program cominterfaces;

uses sysutils;

type
  INamed = interface ['{5A0B0001-0000-0000-0000-000000000001}']
    function Name: string;
  end;

  IShape = interface(INamed) ['{5A0B0001-0000-0000-0000-000000000002}']
    function Area: integer;
  end;

  ICounter = interface ['{5A0B0001-0000-0000-0000-000000000003}']
    function Next: integer;
  end;

  TNamed = class(TInterfacedObject, INamed)
    FName: string;
    constructor Create(const n: string);
    destructor Destroy; override;
    function Name: string; virtual;
  end;

  TSquare = class(TNamed, IShape, ICounter)
    Side, Count: integer;
    constructor Create(const n: string; s: integer);
    function Area: integer;
    function Next: integer;
  end;

  { Holds interfaces in fields: released after its destructor runs. }
  THolder = class
    Main: IShape;
    Pair: array[1..2] of INamed;
    destructor Destroy; override;
  end;

  TBox = record
    Tag: integer;
    Item: INamed;
    More: array[0..1] of INamed;
  end;
  PBox = ^TBox;

  { A constructor passing Self as an interface: the count is 1 meanwhile. }
  TSelfish = class(TNamed)
    constructor Create(const n: string);
  end;

  TOwner = class
  private
    FItem: INamed;
    function GetItem: INamed;
    procedure SetItem(const v: INamed);
  public
    property Item: INamed read GetItem write SetItem;
    property Direct: INamed read FItem write FItem;
  end;

  EStop = class(Exception);

var
  global: INamed;

constructor TNamed.Create(const n: string);
begin
  FName := n;
  writeln('  + ', n);
end;

destructor TNamed.Destroy;
begin
  writeln('  - ', FName);
  inherited Destroy;
end;

function TNamed.Name: string;
begin
  Name := FName;
end;

constructor TSquare.Create(const n: string; s: integer);
begin
  inherited Create(n);
  Side := s;
end;

function TSquare.Area: integer;
begin
  Area := Side * Side;
end;

function TSquare.Next: integer;
begin
  Count := Count + 1;
  Next := Count;
end;

destructor THolder.Destroy;
begin
  writeln('  holder destroyed, fields next');
  inherited Destroy;
end;

procedure Show(const n: INamed);
begin
  writeln('  show ', n.Name);
end;

constructor TSelfish.Create(const n: string);
begin
  inherited Create(n);
  Show(Self);                 { a const parameter: no reference }
  global := Self;             { one reference ... }
  global := nil;              { ... released: not freed while constructing }
  writeln('  constructed with count ', RefCount);
end;

function TOwner.GetItem: INamed;
begin
  GetItem := FItem;
end;

procedure TOwner.SetItem(const v: INamed);
begin
  FItem := v;
end;

function Make(const n: string): INamed;
begin
  Result := TNamed.Create(n);
end;

function MakeSquare(const n: string; s: integer): IShape;
var
  sq: TSquare;
begin
  sq := TSquare.Create(n, s);
  MakeSquare := sq;
end;

procedure ByValue(n: INamed);
var
  o: TNamed;
begin
  o := n as TNamed;
  writeln('  by value ', n.Name, ' count ', o.RefCount);
end;

procedure ByVar(var n: INamed);
begin
  n := TNamed.Create('from ByVar');
end;

procedure Locals;
var
  a, b: INamed;
  arr: array[1..2] of INamed;
  box: TBox;
begin
  a := Make('a');
  b := Make('b');
  arr[1] := a;
  arr[2] := TNamed.Create('arr2');
  box.Item := TNamed.Create('box.item');
  box.More[1] := b;
  writeln('  leaving Locals');
end;

procedure Fails(depth: integer);
var
  mine: INamed;
begin
  mine := Make('depth ' + chr(ord('0') + depth));
  if depth = 2 then
    raise EStop.Create('stop at 2');
  Fails(depth + 1);
  writeln('  not reached');
end;

function Early(flag: boolean): INamed;
var
  tmp: INamed;
begin
  tmp := Make('early tmp');
  if flag then
    exit(tmp);
  Result := nil;
end;

procedure WithFinally;
var
  x: INamed;
begin
  try
    x := Make('in try');
    exit;
  finally
    writeln('  finally, x is ', x.Name);
  end;
end;

procedure Records;
var
  b1, b2: TBox;
  p: PBox;
begin
  b1.Tag := 1;
  b1.Item := TNamed.Create('rec item');
  b1.More[0] := TNamed.Create('rec more');
  b2 := b1;                       { both hold the references now }
  b1.Item := nil;
  writeln('  copy keeps ', b2.Item.Name, ' and ', b2.More[0].Name);
  b2 := b1;                       { releases b2's old item }
  writeln('  copied again');
  new(p);
  p^.Item := TNamed.Create('on the heap');
  p^.More[1] := b2.More[0];
  dispose(p);
  writeln('  disposed');
end;

{ Free Pascal passes a function's result to a local variable directly
  but holds it in a hidden temporary otherwise, until the routine ends;
  pcc releases such temporaries when the statement ends. The scenarios
  below give results to local variables only, so that both agree. }

procedure Assignment;
var
  n, n2: INamed;
begin
  n := Make('first');
  n2 := n;
  n := nil;
  writeln('  still ', n2.Name);
  n2 := Make('second');
  n2 := n2;
  writeln('  self-assignment keeps ', n2.Name);
  n2 := nil;
  writeln('  end of Assignment');
end;

procedure Parameters;
var
  n: INamed;
begin
  n := TNamed.Create('param');
  ByValue(n);
  Show(n);
  ByVar(n);
  writeln('  now ', n.Name);
  ByValue(TNamed.Create('temporary object'));
  n := nil;
end;

procedure Conversions;
var
  s: IShape;
  n: INamed;
  c: ICounter;
  u: IUnknown;
  i: IInterface;
  sq: TSquare;
begin
  s := MakeSquare('square', 3);
  writeln('  ', s.Name, ' area ', s.Area);
  n := s;                        { a parent interface }
  c := s as ICounter;
  writeln('  next ', c.Next, ' ', c.Next);
  writeln('  is: ', n is IShape, ' ', n is ICounter, ' ', n is TSquare);
  sq := s as TSquare;
  writeln('  as class: ', sq.Side);
  u := s;
  i := c;
  writeln('  assigned: ', Assigned(u), ' ', Assigned(i), ' ', u <> nil);
  s := nil;
  c := nil;
  u := nil;
  i := nil;
  writeln('  end of Conversions');
end;

procedure ObjectThenInterface;
var
  sq: TSquare;
  n: INamed;
begin
  sq := TSquare.Create('object', 2);
  writeln('  count ', sq.RefCount);
  n := sq;
  writeln('  count ', sq.RefCount);
  n := nil;
  writeln('  released');
end;

procedure Fields;
var
  h: THolder;
begin
  h := THolder.Create;
  h.Main := TSquare.Create('field', 4);
  h.Pair[1] := TNamed.Create('pair 1');
  h.Pair[2] := h.Main;
  h.Free;
end;

procedure Properties;
var
  o: TOwner;
begin
  o := TOwner.Create;
  o.Item := TNamed.Create('via setter');
  writeln('  ', o.Direct.Name);
  o.Direct := TNamed.Create('direct');
  o.Free;
  writeln('  owner freed');
end;

procedure ExitAndFinally;
var
  n: INamed;
begin
  n := Early(true);
  writeln('  got ', n.Name);
  n := Early(false);
  writeln('  nil: ', n = nil);
  WithFinally;
end;

procedure Loop;
var
  n: INamed;
  k: integer;
begin
  for k := 1 to 3 do
    n := Make('loop ' + chr(ord('0') + k));
  writeln('  end of Loop');
end;

begin
  writeln('locals');
  Locals;
  writeln('assignment');
  Assignment;
  writeln('parameters');
  Parameters;
  writeln('results and conversions');
  Conversions;
  writeln('an object, then an interface');
  ObjectThenInterface;
  writeln('fields');
  Fields;
  writeln('properties');
  Properties;
  writeln('exceptions');
  try
    Fails(0);
  except
    on E: EStop do writeln('  caught ', E.Message);
  end;
  writeln('exit and finally');
  ExitAndFinally;
  writeln('records');
  Records;
  writeln('constructor');
  global := TSelfish.Create('selfish');
  global := nil;
  writeln('discarded result');
  Make('discarded');
  writeln('loop');
  Loop;
  writeln('done');
end.
