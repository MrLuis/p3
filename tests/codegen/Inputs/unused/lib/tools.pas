unit tools;
{ A unit of which the program uses a part. }
interface
type
  TShape = class
    function Area: Integer; virtual;
    function Name: string; virtual;
  end;
  TUnusedShape = class(TShape)
    function Area: Integer; override;
  end;
function Twice(N: Integer): Integer;
function Thrice(N: Integer): Integer;
procedure Describe;
implementation
const
  Factors: array[1..2] of Integer = (2, 3);

function Scale(N, By: Integer): Integer;
begin
  Scale := N * Factors[By];
end;

function Hidden(N: Integer): Integer;
begin
  Hidden := N + 1;
end;

{ not in the interface, and nothing calls it }
function Orphan(N: Integer): Integer;
begin
  writeln('an orphan');
  Orphan := Hidden(N);
end;

function Twice(N: Integer): Integer;
begin
  Twice := Scale(N, 1);
end;

function Thrice(N: Integer): Integer;
begin
  Thrice := Scale(Hidden(N) - 1, 2);
end;

procedure Describe;
begin
  writeln('a unit of tools');
end;

function TShape.Area: Integer;
begin
  Area := 0;
end;

function TShape.Name: string;
begin
  Name := 'shape';
end;

function TUnusedShape.Area: Integer;
begin
  Area := Thrice(7);
end;
end.
