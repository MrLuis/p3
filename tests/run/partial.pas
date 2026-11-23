{ Partial types: T = partial record|class ... end, declared again later in
  the same declaration part, is one type: the fields, methods and
  properties of the halves are all members, in source order. The second
  half is what a source generator writes.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/partial.expected
}
program partial;
{$modeswitch advancedrecords}

type
  TPoint3 = partial record
    X, Y: Integer;
    function Sum: Integer;
  end;

  TShape = partial class(TObject)
  private
    FName: string;
  public
    constructor Create(const AName: string);
    procedure Show;
  end;

  PPoint3 = ^TPoint3;

  [Generated]
  TPoint3 = partial record
    Z: Integer;
    function Total: Integer;
    property Third: Integer read Z;
  end;

  TShape = partial class
  private
    FCount: Integer;
  public
    procedure Extra;
    property Count: Integer read FCount;
  end;

function TPoint3.Sum: Integer;
begin
  Result := X + Y;
end;

function TPoint3.Total: Integer;
begin
  Result := Sum + Z;
end;

constructor TShape.Create(const AName: string);
begin
  FName := AName;
end;

procedure TShape.Show;
begin
  writeln('shape ', FName, ' ', FCount);
end;

procedure TShape.Extra;
begin
  FCount := FCount + 1;
end;

var
  p: TPoint3;
  pp: PPoint3;
  s: TShape;
begin
  p.X := 1;
  p.Y := 2;
  p.Z := 3;
  pp := @p;
  writeln(p.Total, ' ', pp^.Sum, ' ', p.Third, ' ', SizeOf(TPoint3));
  s := TShape.Create('sq');
  s.Extra;
  s.Extra;
  s.Show;
  writeln(s.Count);
end.
