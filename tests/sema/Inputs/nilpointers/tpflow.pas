{$T+}
program tpflow;

type
  PExternal = ^integer;

var
  ext: PExternal;

{$nilpointers on}
type
  PInt = ^integer;
  PBad = ^integer?;                   { error: named nullable }
  TBadRec = record x: integer? end;   { error: not nullable kind }

function BadReturn(ok: boolean; var target: integer): PInt;
begin
  if ok then
    BadReturn := @target;
end;

procedure TestParams(q, q2, q3: PInt?);
var
  p: ^integer;
  np: PInt;
  up: pointer;
  x: integer;
begin
  x := 10;
  p := nil;                           { error: non-null }
  np := PInt(nil);                    { error: typecast nil }
  np := q ?? @x;
  Writeln(np^);
  Writeln(q^);                        { error: q may be nil }
  Writeln(q[0]);                      { error: q may be nil on index }
  if q <> nil then
    Writeln(q^, ' ', q[0]);
  if Assigned(q) and (q^ > 0) then
    Writeln(q^);
  q := q + 1;                         { error: arithmetic on maybe-nil }
  Inc(q2);                            { error: inc on maybe-nil }
  dispose(q3);                        { error: dispose on maybe-nil }
  new(q);
  q^ := 42;
  dispose(q);
  Writeln(q^);                        { error: q is nil after dispose }
  getmem(up, 8);
  freemem(up);
  freemem(up);                        { error: up is nil after freemem }
  reallocmem(up, 16);
  freemem(up);
  reallocmem(up, 0);
  freemem(up);                        { error: up is nil after reallocmem(..., 0) }
end;

procedure TestUnassigned;
var
  np: PInt;
begin
  Writeln(np^);                       { error: unassigned }
end;

{$push}
{$nilpointers off}
procedure TestScopedOff;
var
  p: ^integer;
begin
  p := nil;
  ext := nil;
end;
{$pop}

procedure TestRestored;
var
  p: ^integer;
begin
  p := nil;                           { error: restored on }
  ext := nil;                         { ok: external declaration }
end;

begin
end.
