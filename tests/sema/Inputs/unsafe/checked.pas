{$pointermath checked}
{$T+}
program unsafe_checked;

type
  PInt = ^integer;

function ReadUnsafe(p: PInt): integer;
begin
  ReadUnsafe := unsafe((p + 1)^);
end;

function ReadChecked(p: PInt): integer;
begin
  ReadChecked := (p + 1)^;
end;

var
  p: PInt;
begin
  new(p);
  p^ := 11;
  Writeln(ReadUnsafe(p) * 0 + ReadChecked(p) * 0 + p^);
  dispose(p);
end.
