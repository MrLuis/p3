program errors;
type
  T = class
    F: integer;
    class procedure A; static;
    procedure B; static;
    class procedure C; virtual; static;
    class procedure D;
  end;
class procedure T.A;
begin
  F := 1;
  D;
  writeln(Self.ClassName);
end;
procedure T.B; begin end;
class procedure T.C; begin end;
class procedure T.D; begin end;
begin
end.
