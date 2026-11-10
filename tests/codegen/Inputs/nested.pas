program n;
var g: integer;
procedure outer(p: integer);
var local, own: integer;
  procedure mid;
    procedure inner;
    begin
      local := p + g
    end;
  begin
    inner
  end;
begin
  own := 1;
  mid
end;
function plain(a: integer): integer;
begin
  plain := a
end;
begin
  outer(plain(1))
end.
