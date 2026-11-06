unit dunit;
{ Unit used by d08_units.pas. }

interface

type
  PCell = ^TCell;
  TCell = record
    value: longint;
    next: PCell;
  end;

procedure push(var top: PCell; v: longint);
function sum(top: PCell): longint;

implementation

procedure push(var top: PCell; v: longint);
var
  c: PCell;
begin
  new(c);
  c^.value := v;
  c^.next := top;
  top := c;
end;

function sum(top: PCell): longint;
var
  s: longint;
begin
  s := 0;
  while top <> nil do
  begin
    s := s + top^.value;
    top := top^.next;
  end;
  sum := s;
end;

end.
