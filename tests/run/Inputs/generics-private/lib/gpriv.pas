unit gpriv;
interface
generic function Scaled<T>(x: T): T;
implementation
function Factor: longint;
begin
  Factor := 3;
end;
generic function Scaled<T>(x: T): T;
begin
  result := x * Factor;
end;
end.
