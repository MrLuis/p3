{ Locals across an exception: a read-only local, one written and read after
  the handler, one overwritten before any read, an absolute overlay, and a
  function result (optimization2 item 3).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/pins.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/pins.expected
}
program Pins(output);
function Guarded(n: integer): integer;
var
  ro, live, dead, before: integer;
  alias: integer absolute live;
begin
  ro := n;
  before := 2;
  dead := 0;
  try
    live := ro + 1;
    alias := 10;
    dead := 99;
    Result := live;
    raise Exception.Create('x');
  except
    Result := Result + before;
    dead := 5;
  end;
  Result := Result + ro + dead;
end;
begin
  writeln(Guarded(3))
end.
