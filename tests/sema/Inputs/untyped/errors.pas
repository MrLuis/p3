program errors;
procedure Read1(var x);
begin
  writeln(x);
end;
procedure Assign1(var x);
begin
  x := 1;
end;
procedure Arith(var x);
var y: longint;
begin
  y := x + 1;
end;
procedure ConstCast(const x);
begin
  longint(x) := 1;
end;
procedure Size(var x);
begin
  writeln(sizeof(x));
end;
procedure ConstToVar(const x);
  procedure SetIt(var y);
  begin
    longint(y) := 0;
  end;
begin
  SetIt(x);
  fillchar(x, 1, 0);
end;
procedure Typed(var x: longint);
begin
end;
procedure ToTyped(var x);
begin
  Typed(x);
end;
var
  i: longint;
begin
  Read1(i + 1);
  Read1(5);
  ConstCast('text');
end.
