{$H+}
{$T+}
program TypedAddressOk;

type
  PInt = ^integer;
  PByte = ^byte;

procedure pick(p: PInt); overload;
begin
  writeln('int:', p^);
end;

procedure pick(p: PByte); overload;
begin
  writeln('byte:', p^);
end;

procedure untypedRef(var u; dest: PInt);
begin
  dest := @u;
  writeln('untyped:', dest^);
end;

var
  x: integer;
  b: byte;
  p: PInt;
  u: pointer;
  c: char;
  pc: pchar;
  r: ^integer;
begin
  x := 10;
  b := 7;
  p := @x;
  writeln(p^);
  (@x)^ := 42;
  writeln(x);
  var inf := @x;
  writeln(inf^);
  r := inf;
  writeln(r^);
  u := @x;
  writeln(u = p);
  c := 'Z';
  pc := @c;
  writeln(pc^);
  pick(@x);
  pick(@b);
  untypedRef(x, nil);
  {$push}
  {$T-}
  p := @b;
  {$pop}
  {$typedaddress off}
  p := @b;
  {$typedaddress on}
  r := @x;
  writeln(r^);
end.
