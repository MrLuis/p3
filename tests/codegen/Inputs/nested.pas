program n;
var g: integer;
procedure outer(p: integer);
var local: integer;
  procedure inner;
  begin
    local := p + g
  end;
begin
  inner
end;
begin
  outer(1)
end.
