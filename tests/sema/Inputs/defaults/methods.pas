program methods;
type
  T = class
    procedure A(x: longint = 1);
    procedure B(x: longint);
    property P[i: longint = 1]: longint read G;
  end;
begin
end.
