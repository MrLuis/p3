{ Only locals written inside a protected region and read after the landing
  stay in the frame; read-only and overwritten locals are allocas (item 3). }
program pins;
function F(n: integer): integer;
var
  ro, live, dead, before: integer;
  alias: integer absolute live;
begin
  ro := n;
  before := 7;
  dead := 0;
  try
    live := ro + before;
    alias := 3;
    dead := 4;
    Result := live;
    raise Exception.Create('x');
  except
    Result := Result + before + alias;
    dead := 5;
  end;
end;
begin
  F(1)
end.
