program errors;
type
  TP1 = procedure(const x: longint);
var
  g: longint;
  p: TP1;
procedure ByVar(var y: longint);
begin
end;
procedure Bad(constref x: longint);
begin
  x := 3;
  inc(x);
  ByVar(x);
end;
procedure F(constref x: longint); forward;
procedure F(const x: longint);
begin
end;
procedure H(const x: longint); overload;
begin
end;
procedure H(constref x: longint); overload;
begin
end;
procedure Q(constref x: longint);
begin
end;
begin
  p := @Q;
  Bad(g);
end.
