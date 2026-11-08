{ Classes (--mode=objfpc): fields, methods, constructors and destructors,
  virtual and abstract methods, inherited, properties, Self, Result, is/as,
  typecasts, with on objects, TObject (Create, Destroy, Free, ClassName).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/classes.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/classes.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/classes.expected %t.g.out
}
program classes;

type
  TShape = class
  private
    FName: string;
  public
    constructor Create(const AName: string);
    destructor Destroy; override;
    function Area: real; virtual; abstract;
    procedure Show; virtual;
    property Name: string read FName write FName;
  end;

  TCircle = class(TShape)
    R: real;
    constructor Create(ARadius: real);
    function Area: real; override;
  end;

  TRect = class(TShape)
  private
    FW, FH: integer;
    procedure SetW(V: integer);
  public
    constructor Create(W, H: integer);
    function Area: real; override;
    procedure Show; override;
    property Width: integer read FW write SetW;
  end;

  { A forward declaration, a linked list of objects. }
  TNode = class;
  TList = class
    Head: TNode;
    procedure Push(V: integer);
    function Sum: integer;
    destructor Destroy; override;
  end;
  TNode = class
    Value: integer;
    Next: TNode;
  end;

  TCounter = class
    Count: integer;
    Log: string;
    procedure Add(N: integer); virtual;
    procedure Twice(N: integer);
    function Fact(N: integer): int64;
    function Me: TCounter;
    procedure Report;
  end;
  TLogged = class(TCounter)
    procedure Add(N: integer); override;
    procedure Report;
  end;

constructor TShape.Create(const AName: string);
begin
  inherited Create;
  FName := AName;
end;

destructor TShape.Destroy;
begin
  writeln('destroying ', FName);
  inherited;
end;

procedure TShape.Show;
begin
  writeln(Name, ' area ', Area:0:2);
end;

constructor TCircle.Create(ARadius: real);
begin
  inherited Create('circle');
  R := ARadius;
end;

function TCircle.Area: real;
begin
  Result := 3.14159 * R * R;
end;

constructor TRect.Create(W, H: integer);
begin
  inherited Create('rect');
  FW := W;
  FH := H;
end;

procedure TRect.SetW(V: integer);
begin
  writeln('SetW ', V);
  FW := V;
end;

function TRect.Area: real;
begin
  Area := FW * FH;
end;

procedure TRect.Show;
begin
  write('[');
  inherited Show;
  writeln(']');
end;

procedure TList.Push(V: integer);
var N: TNode;
begin
  N := TNode.Create;
  N.Value := V;
  N.Next := Head;
  Head := N;
end;

function TList.Sum: integer;
var N: TNode;
begin
  Result := 0;
  N := Head;
  while N <> nil do
  begin
    Result := Result + N.Value;
    N := N.Next;
  end;
end;

destructor TList.Destroy;
var N: TNode;
begin
  while Head <> nil do
  begin
    N := Head;
    Head := Head.Next;
    N.Free;
  end;
  writeln('list freed');
end;

procedure TCounter.Add(N: integer);
begin
  Count := Count + N;
end;

procedure TCounter.Twice(N: integer);
  procedure Inner; { reaches Self through the static link }
  begin
    Add(N);
    Log := Log + '+';
  end;
begin
  Inner;
  Inner;
end;

function TCounter.Fact(N: integer): int64;
begin
  if N <= 1 then exit(1);
  Result := N * Fact(N - 1);
end;

function TCounter.Me: TCounter;
begin
  Me := Self;
end;

procedure TCounter.Report;
begin
  writeln('count=', Count, ' log=', Log);
end;

procedure TLogged.Add(N: integer);
begin
  write('add ', N, '; ');
  inherited; { TCounter.Add(N) }
end;

procedure TLogged.Report;
begin
  inherited;
  with Self do writeln('logged ', Count);
end;

procedure Swap(var A, B: TCounter);
var T: TCounter;
begin
  T := A; A := B; B := T;
end;

var
  Shapes: array[1..2] of TShape;
  I: integer;
  S: TShape;
  O: TObject;
  L: TList;
  C, D: TCounter;
  P: pointer;
begin
  Shapes[1] := TCircle.Create(2.0);
  Shapes[2] := TRect.Create(3, 4);
  for I := 1 to 2 do
    Shapes[I].Show;
  if Shapes[2] is TRect then
    TRect(Shapes[2]).Width := 10;
  (Shapes[2] as TRect).Show;
  writeln(Shapes[1] is TRect, ' ', Shapes[1] is TCircle, ' ', Shapes[1].ClassName);
  O := Shapes[1];
  writeln(O is TShape, ' ', O.ClassName, ' ', O is TObject);
  Shapes[1].Name := 'disc';
  S := nil;
  writeln(S is TShape, ' ', assigned(S));
  S.Free; { Free accepts nil }
  for I := 1 to 2 do
    Shapes[I].Free;
  writeln(sizeof(TShape) = sizeof(pointer));

  L := TList.Create;
  for I := 1 to 10 do L.Push(I);
  writeln(L.Sum);
  L.Destroy;

  C := TLogged.Create;
  C.Twice(5);
  writeln;
  C.Report;
  TLogged(C).Report;
  writeln(C.Fact(10), ' ', C.Me = C, ' ', C.Me.Count);
  D := TCounter.Create;
  D.Add(1);
  Swap(C, D);
  writeln(C.Count, ' ', D.Count, ' ', assigned(C), ' ', C <> D);
  P := C;
  writeln(TCounter(P).Count);
  with TCounter.Create do
  begin
    Add(42);
    Report;
    Free;
  end;
  C.Free;
  D.Free;
end.
