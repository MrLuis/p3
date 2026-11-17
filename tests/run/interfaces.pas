{$interfaces corba}
{ Interfaces (--mode=objfpc, CORBA style, no reference counting):
  interface inheritance, classes implementing several interfaces (listed
  by them or an ancestor), virtual methods reached through an interface,
  is / as, comparisons and Assigned, interfaces as parameters, fields,
  array elements and function results, overloaded interface methods.
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/interfaces.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/interfaces.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/interfaces.expected %t.g.out
}
program interfaces;

type
  INamed = interface ['INamed']
    function Name: string;
  end;

  IShape = interface(INamed) ['IShape']
    function Area: integer;
    procedure Scale(k: integer);
  end;

  IPrintable = interface ['IPrintable']
    procedure Print; overload;
    procedure Print(const prefix: string); overload;
  end;

  ICounter = interface ['ICounter']
    function Next: integer;
  end;

  TShape = class(TObject, IShape)
  protected
    W: integer;
  public
    constructor Create(aw: integer);
    function Name: string; virtual;
    function Area: integer; virtual;
    procedure Scale(k: integer);
  end;

  TSquare = class(TShape, IPrintable)
    function Name: string; override;
    function Area: integer; override;
    procedure Print; overload;
    procedure Print(const prefix: string); overload;
  end;

  { Implements an interface its parent does not, with an inherited method. }
  TCube = class(TSquare, ICounter)
    Count: integer;
    function Name: string; override;
    function Next: integer;
  end;

  TCounter = class(ICounter) { the first name may be an interface }
    N: integer;
    function Next: integer;
  end;

  TCanvas = class
    Shapes: array[1..3] of IShape;
    Used: integer;
    procedure Add(s: IShape);
    function Largest: IShape;
  end;

constructor TShape.Create(aw: integer);
begin
  W := aw;
end;

function TShape.Name: string;
begin
  Name := 'shape';
end;

function TShape.Area: integer;
begin
  Area := 0;
end;

procedure TShape.Scale(k: integer);
begin
  W := W * k;
end;

function TSquare.Name: string;
begin
  Name := 'square';
end;

function TSquare.Area: integer;
begin
  Area := W * W;
end;

procedure TSquare.Print;
begin
  writeln('  a square of ', W);
end;

procedure TSquare.Print(const prefix: string);
begin
  write(prefix, ':');
  Print;
end;

function TCube.Name: string;
begin
  Name := 'cube';
end;

function TCube.Next: integer;
begin
  Count := Count + 1;
  Next := Count * W;
end;

function TCounter.Next: integer;
begin
  N := N + 10;
  Next := N;
end;

procedure TCanvas.Add(s: IShape);
begin
  Used := Used + 1;
  Shapes[Used] := s;
end;

function TCanvas.Largest: IShape;
var
  i: integer;
begin
  Result := nil;
  for i := 1 to Used do
    if (Result = nil) or (Shapes[i].Area > Result.Area) then
      Result := Shapes[i];
end;

procedure Report(s: IShape);
begin
  writeln('  ', s.Name, ': area ', s.Area);
end;

procedure Count3(c: ICounter);
var
  i: integer;
begin
  for i := 1 to 3 do
    write(' ', c.Next);
  writeln;
end;

var
  sq: TSquare;
  cube: TCube;
  shape: TShape;
  s, t: IShape;
  n: INamed;
  p: IPrintable;
  o: TObject;
  canvas: TCanvas;
  big: IShape;

begin
  sq := TSquare.Create(3);
  cube := TCube.Create(2);
  shape := TShape.Create(5);

  writeln('through interfaces');
  s := sq;
  Report(s);
  s.Scale(2);
  Report(sq);
  Report(cube);
  Report(shape);
  n := s;
  writeln('  named ', n.Name);

  writeln('is and as');
  o := sq;
  writeln('  ', o is IShape, ' ', o is IPrintable, ' ', o is ICounter, ' ',
          shape is IPrintable, ' ', cube is IShape, ' ', o is INamed);
  p := o as IPrintable;
  p.Print;
  p.Print('as');
  p := cube;
  p.Print('cube');
  o := nil;
  writeln('  nil is IShape: ', o is IShape);
  p := o as IPrintable;
  writeln('  nil as IPrintable = nil: ', p = nil);

  writeln('comparisons');
  t := nil;
  writeln('  ', t = nil, ' ', s <> nil, ' ', Assigned(s), ' ', Assigned(t),
          ' ', s = IShape(sq), ' ', s = IShape(cube));

  writeln('counters');
  write(' ');
  Count3(cube);
  write(' ');
  Count3(TCounter.Create);

  writeln('canvas');
  canvas := TCanvas.Create;
  canvas.Add(sq);
  canvas.Add(cube);
  canvas.Add(shape);
  big := canvas.Largest;
  writeln('  largest: ', big.Name, ' ', big.Area);
  canvas.Shapes[2].Scale(10);
  writeln('  now: ', canvas.Largest.Name, ' ', canvas.Largest.Area);

  canvas.Free;
  sq.Free;
  cube.Free;
  shape.Free;
end.
