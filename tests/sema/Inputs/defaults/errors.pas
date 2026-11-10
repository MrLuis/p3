program errors;
type
  TRec = record x: longint end;
var
  v: longint;
  rv: TRec;
const
  tc: longint = 3;
procedure A1(a: longint = v); begin end;
procedure A2(a: longint = tc); begin end;
procedure A3(a: longint = 'x'); begin end;
procedure A4(a: byte = 300); begin end;
procedure A5(a: longint = nope); begin end;
procedure A6(r: TRec = 1); begin end;
procedure A7(a: array of longint = 1); begin end;
procedure C1(a: longint = 1); forward;
procedure C1(a: longint = 2); begin end;
procedure C2(a: longint); forward;
procedure C2(a: longint = 2); begin end;
procedure D1(a: longint; b: longint = 1); begin end;
function D2(a: longint; b: longint = 1): longint; begin D2 := a + b end;
procedure E1(a: longint); begin end;
procedure E1(a: longint; b: longint = 1); begin end;
begin
  D1;
  D1(1, 2, 3);
  v := D2;
  E1(1);
end.
