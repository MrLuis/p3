unit oldlib;

interface

type
  TLegacy = record
    x: integer;
  end deprecated 'use TModern';

procedure Show(i: integer); overload;
procedure Show(const s: string); overload; deprecated 'pass integers';
function Twice(i: integer): integer;
procedure Later;

implementation

procedure Show(i: integer);
begin
  writeln(i);
end;

procedure Show(const s: string);
begin
  writeln(s);
end;

function Twice(i: integer): integer;
begin
  Twice := 2 * i;
end;

{ a directive here, not on the heading, has no effect (as in Free Pascal) }
procedure Later; experimental;
begin
end;

end.
