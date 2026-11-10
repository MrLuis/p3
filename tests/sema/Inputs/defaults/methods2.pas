program methods2;
type
  T = class
    procedure A(x: longint = 1);
    procedure B(x: longint);
  end;
procedure T.A(x: longint = 2); begin end;
procedure T.B(x: longint = 2); begin end;
begin
end.
