{$H+}
{$pointermath checked}
program PointerMathChecked;

type
  PInt = ^integer;

var
  mode: string;
  p, q, r: PInt;
  arr: array[0..3] of integer;
  d: int64;
  i: integer;
begin
  if paramcount >= 1 then
    mode := paramstr(1)
  else
    mode := 'ok';

  if mode = 'ok' then
  begin
    getmem(p, 4 * sizeof(integer));
    for i := 0 to 3 do
      p[i] := (i + 1) * 10;
    q := p + 2;
    writeln(p[0], ' ', q[0], ' ', q[1], ' ', q - p);
    q := p + 4; { one past the end }
    writeln(q - p);
    Dec(q, 2);
    Inc(q, 1);
    writeln(q^);
    reallocmem(p, 6 * sizeof(integer));
    p[5] := 60;
    d := (p + 6) - p;
    writeln(p[5], ' ', d);
    freemem(p);
    new(r);
    r^ := 99;
    writeln(r[0]);
    dispose(r);
  end
  else if mode = 'oob-idx' then
  begin
    getmem(p, 4 * sizeof(integer));
    writeln(p[4]);
  end
  else if mode = 'neg-idx' then
  begin
    getmem(p, 4 * sizeof(integer));
    writeln(p[-1]);
  end
  else if mode = 'oob-add' then
  begin
    getmem(p, 4 * sizeof(integer));
    q := p + 5;
  end
  else if mode = 'oob-inc' then
  begin
    getmem(p, 4 * sizeof(integer));
    Inc(p, 5);
  end
  else if mode = 'ptrdiff' then
  begin
    getmem(p, 4 * sizeof(integer));
    getmem(q, 4 * sizeof(integer));
    writeln(p - q);
  end
  else if mode = 'nil-idx' then
  begin
    p := nil;
    writeln(p[0]);
  end
  else if mode = 'stack' then
  begin
    arr[0] := 1;
    p := @arr[0];
    writeln(p[1]);
  end
  else if mode = 'freed' then
  begin
    getmem(p, 4 * sizeof(integer));
    p[0] := 7;
    q := p;
    freemem(p);
    writeln(q[0]);
  end;
end.
