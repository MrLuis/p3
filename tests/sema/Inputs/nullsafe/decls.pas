{$H+}
program decls;
{$nullsafe on}
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
  TAlias = TSquare?;                 // error: a name
  TBad = record X: Integer? end;     // error: not a reference

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

procedure Test;
var
  s: IShape?;
  t: IShape;
  c: TSquareClass?;
begin
  s := Maybe(3);
  t := s ?? TSquare.Create(1);
  Writeln(t.Area, ' ', s?.Area ?? 0);
  Writeln(s.Area);                  // error
  c := nil;
  t := c.Create(1);                 // error: c is nil
  t := nil;                         // error
  t := Maybe(0);                    // error
end;

begin
  Test;
end.
