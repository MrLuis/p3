{ Classes: virtual calls, is and as, object creation and release. }
program classes;
type
  TShape = class
    function Area: longint; virtual; abstract;
    function Sides: longint; virtual;
  end;
  TSquare = class(TShape)
    side: longint;
    function Area: longint; override;
    function Sides: longint; override;
  end;
  TRect = class(TSquare)
    other: longint;
    function Area: longint; override;
  end;
  TCircle = class(TShape)
    radius: longint;
    function Area: longint; override;
  end;

function TShape.Sides: longint;
begin
  Sides := 0;
end;

function TSquare.Area: longint;
begin
  Area := side * side;
end;

function TSquare.Sides: longint;
begin
  Sides := 4;
end;

function TRect.Area: longint;
begin
  Area := side * other;
end;

function TCircle.Area: longint;
begin
  Area := 3 * radius * radius;
end;

const
  N = 1000;
var
  shapes: array[0..N - 1] of TShape;
  i, round, sum, squares: longint;
  s: TSquare;
  r: TRect;
  c: TCircle;
begin
  sum := 0;
  squares := 0;
  for round := 1 to 5000 do
  begin
    for i := 0 to N - 1 do
      case i mod 3 of
        0: begin
             s := TSquare.Create; s.side := i mod 10; shapes[i] := s;
           end;
        1: begin
             r := TRect.Create; r.side := i mod 10; r.other := 3; shapes[i] := r;
           end;
        2: begin
             c := TCircle.Create; c.radius := i mod 10; shapes[i] := c;
           end;
      end;
    for i := 0 to N - 1 do
    begin
      sum := (sum + shapes[i].Area + shapes[i].Sides) and $FFFFFF;
      if shapes[i] is TSquare then
      begin
        squares := squares + 1;
        sum := (sum + (shapes[i] as TSquare).side) and $FFFFFF;
      end;
    end;
    for i := 0 to N - 1 do
      shapes[i].Free;
  end;
  writeln(sum, ' ', squares);
end.
