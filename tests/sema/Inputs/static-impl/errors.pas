program errors;
type
  TObj = class
    class function A: longint;
    function B: longint;
    constructor Create;
    class function C: longint; static;
  end;
class function TObj.A: longint; static;
begin
  A := 1;
end;
function TObj.B: longint; static;
begin
  B := 2;
end;
constructor TObj.Create; static;
begin
end;
class function TObj.C: longint;
begin
  C := 3;
end;
begin
  writeln(TObj.A, ' ', TObj.C);
end.
