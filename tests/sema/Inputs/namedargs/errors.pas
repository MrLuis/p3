program errors;
{$H+}
type
  TProc = procedure(A: Integer; B: Integer = 0);
  TObj = class
    procedure M(A: Integer; B: Integer = 2);
  end;

procedure Connect(Host: string; Port: Integer = 5432; Timeout: Integer = 30);
begin
end;

procedure Mid(A: Integer; B: Integer = 1; C: Integer);
begin
end;

function F(A: Integer; B: Integer = 1): Integer;
begin
  F := A;
end;

function G(A: Integer): Integer; overload;
begin
  G := A;
end;

function G(A: Integer; B: Integer = 1): Integer; overload;
begin
  G := A;
end;

procedure H(A: Integer; B: Integer = 1); overload;
begin
end;

procedure H(A: Integer; C: Real = 1); overload;
begin
end;

procedure V(var X: Integer; Y: Integer = 0);
begin
end;

procedure TObj.M(A: Integer; B: Integer);
begin
end;

var
  x: Integer;
  s: string;
  p: TProc;
  o: TObj;

begin
  Connect(Hots := 'a');
  Connect('a', Host := 'b');
  Connect(Port := 1, Port := 2, Host := 'a');
  Connect(Host := 'a', 5);
  Connect(Port := 1);
  Connect('a', 1, 2, Timeout := 3);
  Connect('a', Port := 'x');
  Connect(Host := x);
  Mid(1);
  Mid(1, 2);
  Mid(C := 3);
  Mid(1, C := 3);
  Mid(1, 2, 3);
  x := F(1);
  x := F(1, B := 2);
  x := F(1, D := 2);
  x := G(1);
  x := G(1, B := 2);
  x := G(B := 2);
  x := G(1, B := 'two');
  H(1);
  H(1, C := 2);
  H(1, B := 2);
  V(Y := 1, X := 5);
  V(Y := 1, X := x);
  writeln(x := 1);
  x := abs(x := 1);
  x := Integer(x := 1);
  p(B := 1);
  p(A := 1, Z := 2);
  o.M(B := 1);
  o.M(B := 1, A := 2);
  x := length(S := s);
  Connect(Host := 'a', Timeout := nope);
end.
