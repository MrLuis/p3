program g(output, missing);
label 1, 2, 3, 3;
var f: text; t: file of integer; i: integer;
procedure p(f: text); begin end;
procedure q(procedure x(k: integer)); begin end;
procedure takesReal(k: real); begin end;
begin
  goto 4;
  if i = 0 then begin 2: writeln end;
  goto 2;
  for i := 1 to 2 do 1: writeln;
  f := f;
  write(t, 'x');
  writeln(t, 1);
  readln(t, i);
  q(takesReal);
  q(writeln);
  q(i);
  if eoln(t) then;
  reset(i)
end.
