{$pointermath off}
program PointerMathOffErrors;

type
  PInt = ^integer;
var
  p, q: PInt;
  u: pointer;
  i: integer;
begin
  p := p + 1;
  p := 1 + p;
  p := p - 1;
  i := p - q;
  i := p[0];
  Inc(p);
  Dec(p, 2);
  u := u + 1;
  Inc(u);
  if p < q then writeln; { relational ordering remains allowed }
  {$push}
  {$pointermath unchecked}
  p := p + 1;
  i := p[0];
  Inc(p);
  {$pop}
  p := p + 1; { rejected again after $pop }
  {$pointermath checked}
  p := p + 1; { allowed under checked }
  {$pointermath off}
  p := p - 1; { rejected again }
end.
