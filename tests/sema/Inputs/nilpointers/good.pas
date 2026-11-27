{$T+}
program good;

{$nilpointers on}
type
  PInt = ^integer;

function Pick(p: PInt?; var fallback: integer): PInt;
begin
  if Assigned(p) and (p^ > 0) then
    Pick := p
  else
    Pick := @fallback;
end;

procedure Run;
var
  x: integer;
  q: PInt?;
  np: PInt;
  up: pointer;
begin
  x := 7;
  new(q);
  q^ := 42;
  np := Pick(q, x);
  Writeln(np^, ' ', (q ?? @x)^);
  dispose(q);
  q := nil;
  np := q ?? @x;
  Writeln(np^);
  getmem(up, 8);
  reallocmem(up, 16);
  freemem(up);
  Writeln('ok');
end;

begin
  Run;
end.
