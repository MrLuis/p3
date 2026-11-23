program directives;
type
  TC = class
    function M: integer; compiletime;
  end;
procedure P; compiletime;
begin
end;
function E: integer; compiletime; external 'c' name 'abs';
function TC.M: integer;
begin
  M := 1;
end;
function Ok: integer; compiletime;
begin
  Ok := 1;
end;
begin
  writeln(Ok);
end.
