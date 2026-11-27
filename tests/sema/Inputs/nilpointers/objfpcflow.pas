{$H+}
program objfpcflow;

type
  PInt = ^integer;
  TBox = class
    V: integer;
    function Get: integer;
  end;

function TBox.Get: integer;
begin
  Result := V;
end;

{$nilpointers on}
procedure OnlyPointers(p: PInt?);
var
  b: TBox;
  np: PInt;
begin
  b := nil;                           { ok: $nullsafe is off }
  Writeln(b.Get);
  np := p;                            { error: p may be nil }
  Writeln(np^);
end;
{$nilpointers off}

{$nullsafe on}
procedure OnlyClasses;
var
  b: TBox;
  np: PInt;
begin
  b := nil;                           { error: $nullsafe is on }
  np := nil;                          { ok: $nilpointers is off }
  Writeln(b.Get, np^);
end;

{$nilpointers on}
procedure BothOn(p: PInt?; b: TBox?);
var
  np: PInt;
  nb: TBox;
begin
  np := p;                            { error: p may be nil }
  nb := b;                            { error: b may be nil }
  if (p <> nil) and (b <> nil) then
  begin
    np := p;
    nb := b;
    Writeln(np^, ' ', nb.Get);
  end;
end;

begin
end.
