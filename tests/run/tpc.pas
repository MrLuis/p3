{ M12 (Turbo/FPC): typed constants (static, even in routines),
  untyped pointer, @, function calls as statements.
  RUN: %pcc --mode=tp %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/tpc.expected %t.out
  RUN: %pcc --mode=tp -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/tpc.expected %t.O2.out
}
program TPC;
type PInt = ^integer;
const
  Limit: integer = 3;
  Ratio: real = 2.5;
  Title: string = 'typed';
  Initial: char = 'Q';
var
  x: integer;
  p: pointer;
  q: PInt;

function counter: integer;
const calls: integer = 0;   { static: survives between calls }
begin
  calls := calls + 1;
  counter := calls
end;

begin
  writeln(Limit, ' ', Ratio:3:1, ' ', Title, ' ', Initial);
  Limit := Limit + 1;            { typed constants are variables }
  writeln(Limit);
  counter; writeln(counter, ' ', counter);
  x := 42;
  p := @x;
  q := p;                        { untyped pointer converts to any pointer }
  q^ := q^ + 1;
  writeln(x, ' ', p = q, ' ', p <> nil)
end.
