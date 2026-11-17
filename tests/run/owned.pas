{ Owned variables (objfpc): `x: TFoo owned` owns its object, which is
  freed when the variable gets another one and when its routine ends,
  however it ends (normally, exit, an exception), the last declared
  first. `move x` gives the object up: x is nil afterwards. A destructor
  that raises does not stop the others being freed; its exception is
  raised afterwards unless another is already leaving the routine.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/owned.expected
  RUN: %p3 --mode=objfpc -O2 -fno-range-checks %s -o %t.fast
  RUN: %t.fast | diff - %S/owned.expected
}
program owned;

type
  EBoom = class(Exception) end;

  TNode = class
    name: string;
    failing: boolean;
    constructor Create(const n: string);
    destructor Destroy; override;
    procedure Show;
  end;

  TList = class
    items: array[0..3] of TNode;
    count: integer;
    destructor Destroy; override;
    procedure Add(n: TNode);
  end;

constructor TNode.Create(const n: string);
begin
  name := n;
  writeln('  create ', name);
end;

destructor TNode.Destroy;
begin
  writeln('  destroy ', name);
  if failing then
    raise EBoom.Create('destroying ' + name);
  inherited Destroy;
end;

procedure TNode.Show;
begin
  writeln('  show ', name);
end;

destructor TList.Destroy;
var
  i: integer;
begin
  for i := 0 to count - 1 do
    items[i].Free;
  inherited Destroy;
end;

procedure TList.Add(n: TNode);
begin
  items[count] := n;
  count := count + 1;
end;

{ freed at the end, the last declared first; never assigned: nothing }
procedure Simple;
var
  a: TNode owned;
  b: TNode owned;
  never: TNode owned;
begin
  a := TNode.Create('a');
  b := TNode.Create('b');
  a.Show;
  if never = nil then
    writeln('  never is nil');
end;

{ the caller owns the result }
function Make(const n: string): TNode;
var
  p: TNode owned;
begin
  p := TNode.Create(n);
  p.Show;
  Make := move p;
end;

{ a new object frees the old one; nil frees it now }
procedure Reassign;
var
  p: TNode owned;
  borrowed: TNode;
begin
  p := TNode.Create('r1');
  p := TNode.Create('r2');
  borrowed := p;
  p := borrowed; { the same object: kept }
  borrowed.Show;
  p := nil;
  writeln('  after nil');
  p := Make('r3');
end;

procedure Raises;
var
  p: TNode owned;
  q: TNode owned;
begin
  p := TNode.Create('x1');
  q := TNode.Create('x2');
  raise EBoom.Create('boom');
end;

{ exit, break and continue }
function Leaves(how: integer): integer;
var
  p: TNode owned;
  i: integer;
begin
  Result := 0;
  for i := 1 to 3 do
  begin
    p := TNode.Create('loop');
    if i = how then
      continue;
    if i = how + 1 then
      break;
    Result := Result + 1;
  end;
  if how = 3 then
    exit;
  Result := Result + 10;
  try
    p := TNode.Create('in try');
    if how = 1 then
      exit(100);
  finally
    writeln('  finally');
  end;
end;

{ moved into a container, in a loop; moved on one path only }
procedure Moves(keep: boolean);
var
  list: TList owned;
  n: TNode owned;
  i: integer;
begin
  list := TList.Create;
  for i := 1 to 2 do
  begin
    n := TNode.Create('item');
    list.Add(move n);
  end;
  n := TNode.Create('last');
  if keep then
    list.Add(move n);
  writeln('  ', list.count, ' kept');
end;

{ a nested routine uses and replaces the variable }
procedure Nested;
var
  p: TNode owned;

  procedure Replace;
  begin
    p.Show;
    p := TNode.Create('inner');
  end;

begin
  p := TNode.Create('outer');
  Replace;
  p.Show;
end;

{ a destructor's exception: the others are freed, then it is raised }
procedure BadDestructor;
var
  a, b, c: TNode owned;
begin
  a := TNode.Create('d1');
  b := TNode.Create('d2');
  c := TNode.Create('d3');
  b.failing := true;
  c.failing := true;
end;

{ ... unless an exception is already leaving the routine }
procedure BadDestructorRaising;
var
  a, b: TNode owned;
begin
  a := TNode.Create('e1');
  b := TNode.Create('e2');
  b.failing := true;
  raise EBoom.Create('the first');
end;

{ ... and when the object is replaced, it is raised there }
procedure BadReplace;
var
  a: TNode owned;
begin
  a := TNode.Create('f1');
  a.failing := true;
  a := TNode.Create('f2');
  writeln('  not reached');
end;

type
  TOwner = class
    procedure Run;
  end;

procedure TOwner.Run;
var
  p: TNode owned;
begin
  p := TNode.Create('in method');
end;

var
  kept: TNode;
  o: TOwner;
  total: integer;

begin
  writeln('simple');
  Simple;
  writeln('make');
  kept := Make('made');
  kept.Show;
  kept.Free;
  writeln('reassign');
  Reassign;
  writeln('raises');
  try
    Raises;
  except
    on e: EBoom do
      writeln('  caught ', e.Message);
  end;
  writeln('leaves 1');
  total := Leaves(1);
  writeln('  = ', total);
  writeln('leaves 2');
  total := Leaves(2);
  writeln('  = ', total);
  writeln('leaves 3');
  total := Leaves(3);
  writeln('  = ', total);
  writeln('moves');
  Moves(true);
  Moves(false);
  writeln('nested');
  Nested;
  writeln('bad destructor');
  try
    BadDestructor;
  except
    on e: EBoom do
      writeln('  caught ', e.Message);
  end;
  try
    BadDestructorRaising;
  except
    on e: EBoom do
      writeln('  caught ', e.Message);
  end;
  try
    BadReplace;
  except
    on e: EBoom do
      writeln('  caught ', e.Message);
  end;
  writeln('method');
  o := TOwner.Create;
  o.Run;
  o.Free;
  writeln('done');
end.
