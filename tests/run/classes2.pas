{ Classes (--mode=objfpc), continued: string, record and array members,
  string properties with setters, functions returning objects and strings,
  constructors and destructors called on objects, virtual functions with
  string results, unqualified calls from nested routines, with on objects.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/classes2.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/classes2.expected %t.O2.out
}
program classes2;

type
  TPoint = record
    X, Y: integer;
  end;

  TAnimal = class
  private
    FName: string[20];
    FSound: string;
    procedure SetName(const AName: string);
  public
    Pos: TPoint;
    Visits: array[1..3] of integer;
    constructor Create(const AName: string);
    function Describe: string; virtual;
    function Speak(Times: integer): string;
    procedure Move(DX, DY: integer);
    property Name: string read FSound write SetName;
  end;

  TDog = class(TAnimal)
    Tricks: integer;
    function Describe: string; override;
    function Clone: TDog;
    procedure Learn;
    destructor Done;
  end;

  TCounter = class
    N: int64;
    procedure Reset;
    function Next: int64;
  end;

procedure TAnimal.SetName(const AName: string);
begin
  FName := AName;
  FSound := AName + '!';
end;

constructor TAnimal.Create(const AName: string);
begin
  Name := AName; { through the property's write procedure }
  Pos.X := 1;
  Pos.Y := 2;
end;

function TAnimal.Describe: string;
begin
  Result := 'animal ' + FName;
end;

function TAnimal.Speak(Times: integer): string;
var
  I: integer;
begin
  Result := '';
  for I := 1 to Times do
    Result := Result + FSound;
end;

procedure TAnimal.Move(DX, DY: integer);
  procedure Step;
  begin
    Pos.X := Pos.X + DX;
    Pos.Y := Pos.Y + DY;
    Visits[1] := Visits[1] + 1;
  end;
begin
  Step;
  Step;
end;

function TDog.Describe: string;
begin
  Result := 'dog ' + inherited Describe + ' knows ' + chr(ord('0') + Tricks);
end;

function TDog.Clone: TDog;
begin
  Result := TDog.Create(FName);
  Result.Tricks := Tricks;
  Result.Pos := Pos;
end;

procedure TDog.Learn;
begin
  Tricks := Tricks + 1;
  if Tricks < 3 then
    Learn; { recursion through Self }
end;

destructor TDog.Done;
begin
  writeln('done with ', Describe);
end;

procedure TCounter.Reset;
begin
  N := 0;
end;

function TCounter.Next: int64;
begin
  N := N + 1;
  Next := N * 1000000000;
end;

function MakeAnimal(Dog: boolean): TAnimal;
begin
  if Dog then
    MakeAnimal := TDog.Create('rex')
  else
    MakeAnimal := TAnimal.Create('cat');
end;

var
  A, B: TAnimal;
  D, E: TDog;
  C: TCounter;
  S: string;
  P: TPoint;
begin
  A := MakeAnimal(false);
  B := MakeAnimal(true);
  writeln(A.Describe, ' / ', B.Describe);
  writeln(A.Speak(3), ' ', length(B.Speak(2)));
  A.Name := 'kitty';
  writeln(A.Name, ' ', A.Describe);
  D := B as TDog;
  D.Learn;
  writeln(D.Describe, ' ', D.Tricks);
  D.Move(10, -1);
  P := D.Pos;
  writeln(P.X, ',', P.Y, ' visits ', D.Visits[1]);
  E := D.Clone;
  writeln(E.Describe, ' at ', E.Pos.X, ',', E.Pos.Y, ' ', E <> D);
  S := E.ClassName + '/' + A.ClassName;
  writeln(S);
  with E do
  begin
    Name := 'fido';
    Move(1, 1);
    writeln(Describe, ' ', Pos.X, ' ', Visits[1]);
  end;
  E.Done;
  D.Create('again'); { a constructor called on an object: no new object }
  writeln(D.Describe);
  C := TCounter.Create;
  C.Reset;
  writeln(C.Next, ' ', C.Next, ' ', C.N);
  A.Free;
  D.Free;
  C.Destroy;
end.
