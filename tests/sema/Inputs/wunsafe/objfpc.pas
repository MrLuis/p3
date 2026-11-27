{$H+}
program wunsafe_objfpc;

type
  PInt = ^integer;
  TBox = class
    V: integer;
  end;

procedure Test(b: TBox; u: pointer);
var
  p: pointer;
  x: TBox;
begin
  p := pointer(b);             { warn: class -> pointer }
  x := TBox(u);                { warn: pointer -> class }
  unsafe
    p := pointer(b);
    x := TBox(u);
  end;
  p := unsafe(pointer(b));
end;

begin
end.
