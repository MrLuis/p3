{ Named arguments (objfpc): Name := value among the arguments of a call
  is the argument for the parameter Name. Arguments by position come first;
  a parameter that is not given takes its default value, wherever it is in
  the list. Among overloaded routines that fit equally well, the one that
  leaves fewer parameters to their default values is called.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/namedargs.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/namedargs.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/namedargs.expected
}
{$H+}
program namedargs;

type
  TColor = (Red, Green, Blue);
  TPoint = record
    X, Y: Integer;
  end;
  TCallback = function(Value: Integer; Scale: Integer = 2): Integer;

  IShape = interface
    function Area(Scale: Integer = 1; Offset: Integer = 0): Integer;
    procedure Move(DX: Integer = 0; DY: Integer = 0);
  end;

  TShape = class(TInterfacedObject, IShape)
    FX, FY, FSide: Integer;
    constructor Create(Side: Integer = 1; X: Integer = 0; Y: Integer = 0);
    function Area(Scale: Integer = 1; Offset: Integer = 0): Integer; virtual;
    procedure Move(DX: Integer = 0; DY: Integer = 0);
    function Describe(const Prefix: string = '<'; const Suffix: string = '>'): string;
    class function Make(Side: Integer; Double: Boolean = False): TShape;
  end;

  TCube = class(TShape)
    constructor Create(Side: Integer = 1; X: Integer = 0; Y: Integer = 0);
    function Area(Scale: Integer = 1; Offset: Integer = 0): Integer; override;
  end;

var
  trace: string;

procedure Connect(Host: string; Port: Integer = 5432;
                  Timeout: Integer = 30; UseTls: Boolean = True);
begin
  Writeln(Host, ':', Port, ' timeout ', Timeout, ' tls ', UseTls);
end;

{ a parameter without a default value after one with it: given by name,
  or by position with all those before it }
function Pad(const S: string; Width: Integer = 8; Fill: Char): string;
begin
  Result := S;
  while Length(Result) < Width do
    Result := Result + Fill;
end;

procedure Swap(var A, B: Integer);
var
  t: Integer;
begin
  t := A;
  A := B;
  B := t;
end;

procedure Fill(out Dest: TPoint; X: Integer = 1; Y: Integer = 2);
begin
  Dest.X := X;
  Dest.Y := Y;
end;

function Sum(const Items: array of Integer; Start: Integer = 0): Integer;
var
  i: Integer;
begin
  Result := Start;
  for i := 0 to High(Items) do
    Result := Result + Items[i];
end;

function Note(const S: string; V: Integer): Integer;
begin
  trace := trace + S;
  Note := V;
end;

function Sub(A, B: Integer): Integer;
begin
  Sub := A - B;
end;

function Twice(Value: Integer; Scale: Integer = 2): Integer;
begin
  Twice := Value * Scale;
end;

{ overloads: the arguments written decide, then the fewest defaults }
function Pick(A: Integer): string; overload;
begin
  Pick := 'one integer';
end;

function Pick(A: Integer; B: Integer = 0): string; overload;
begin
  Pick := 'two integers';
end;

function Pick(A: Real; B: Integer = 0; C: Integer = 0): string; overload;
begin
  Pick := 'real and two';
end;

function Pick(const Name: string; Count: Integer = 1): string; overload;
begin
  Pick := 'string';
end;

procedure Show(X: Integer; Color: TColor = Red); overload;
begin
  Writeln('show ', X, ' color ', Ord(Color));
end;

procedure Show(X: Integer; const Text: string = 'none'); overload;
begin
  Writeln('show ', X, ' text ', Text);
end;

constructor TShape.Create(Side: Integer; X: Integer; Y: Integer);
begin
  FSide := Side;
  FX := X;
  FY := Y;
end;

function TShape.Area(Scale: Integer; Offset: Integer): Integer;
begin
  Area := FSide * FSide * Scale + Offset;
end;

procedure TShape.Move(DX: Integer; DY: Integer);
begin
  FX := FX + DX;
  FY := FY + DY;
end;

function TShape.Describe(const Prefix: string; const Suffix: string): string;
begin
  Describe := Prefix + 'shape' + Suffix;
end;

class function TShape.Make(Side: Integer; Double: Boolean): TShape;
begin
  if Double then
    Side := Side * 2;
  Make := TShape.Create(Side := Side);
end;

constructor TCube.Create(Side: Integer; X: Integer; Y: Integer);
begin
  inherited Create(Y := Y, Side := Side, X := X);
end;

function TCube.Area(Scale: Integer; Offset: Integer): Integer;
begin
  Area := 6 * inherited Area(Offset := 0, Scale := Scale) + Offset;
end;

var
  x, y: Integer;
  p: TPoint;
  s: TShape;
  c: TCube;
  i: IShape;
  cb: TCallback;
begin
  Connect('db.local');
  Connect('db.local', Timeout := 5);
  Connect(Host := 'db.local', UseTls := False);
  Connect(UseTls := False, Port := 1, Host := 'h', Timeout := 2);
  Connect('a', 1, UseTls := False);
  Connect('b', 2, 3, UseTls := False);

  Writeln(Pad('ab', Fill := '.'), '|');
  Writeln(Pad('ab', 4, '*'), '|');
  Writeln(Pad(Fill := '-', S := 'xy', Width := 3), '|');

  x := 1;
  y := 2;
  Swap(B := x, A := y);
  Writeln(x, ' ', y);
  Fill(p, Y := 9);
  Writeln(p.X, ' ', p.Y);
  Fill(Y := 7, Dest := p, X := 5);
  Writeln(p.X, ' ', p.Y);
  Writeln(Sum([1, 2, 3], Start := 10), ' ', Sum(Start := 1, Items := [4, 5]));

  { the arguments are evaluated in the order of the parameters }
  Writeln(Sub(B := Note('b', 1), A := Note('a', 10)), ' ', trace);

  Writeln(Pick(1));
  Writeln(Pick(1, 2));
  Writeln(Pick(1, B := 2));
  Writeln(Pick(A := 1));
  Writeln(Pick(1.5));
  Writeln(Pick(1, C := 3));
  Writeln(Pick('x'));
  Writeln(Pick(Count := 2, Name := 'x'));
  Show(1, Blue);
  Show(2, 'two');
  Show(3, Text := 'three');
  Show(Color := Green, X := 4);

  s := TShape.Create(Side := 3);
  Writeln(s.Area, ' ', s.Area(2), ' ', s.Area(Offset := 1), ' ',
          s.Area(Offset := 1, Scale := 3));
  s.Move(DY := 4);
  s.Move(1);
  Writeln(s.FX, ' ', s.FY);
  Writeln(s.Describe, ' ', s.Describe(Suffix := ']'), ' ',
          s.Describe('[', Suffix := ']'));
  s.Free;
  s := TShape.Make(2, Double := True);
  Writeln(s.FSide);
  s.Free;
  s := TShape.Make(Double := False, Side := 5);
  Writeln(s.FSide);
  s.Free;

  c := TCube.Create(2, Y := 8);
  Writeln(c.FSide, ' ', c.FX, ' ', c.FY, ' ', c.Area, ' ', c.Area(Offset := 1));
  s := c;
  Writeln(s.Area(Scale := 2));
  i := c;
  Writeln(i.Area, ' ', i.Area(Offset := 5), ' ', i.Area(Offset := 1, Scale := 2));
  i.Move(DY := 2);
  Writeln(c.FX, ' ', c.FY);
  i := nil;

  cb := @Twice;
  Writeln(cb(4), ' ', cb(4, Scale := 5), ' ', cb(Scale := 3, Value := 2));
  with p do
    Fill(p, X := Y, Y := X);
  Writeln(p.X, ' ', p.Y);
end.
