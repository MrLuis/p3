program namedargs;
function Area(Width: longint; Height: longint = 2; Scale: longint = 3): longint;
begin
  Area := Width * Height * Scale;
end;

function First: longint;
begin
  First := 1;
end;

function Second: longint;
begin
  Second := 2;
end;

var
  n: longint;
begin
  n := Area(Scale := Second, Width := First);
end.
