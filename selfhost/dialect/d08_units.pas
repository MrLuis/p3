program d08_units;
{ Dialect check: units, nested routines with access to outer locals,
  short-circuit boolean evaluation. }
uses dunit;

var
  stack: PCell;

procedure fill(n: longint);
var
  count: longint;

  procedure addOne(v: longint);
  begin
    push(stack, v);
    count := count + 1;
  end;

var
  i: longint;
begin
  count := 0;
  for i := 1 to n do
    addOne(i);
  writeln('pushed ', count);
end;

begin
  stack := nil;
  fill(10);
  writeln(sum(stack));
  { the right operand would dereference nil without short-circuit evaluation }
  if (stack <> nil) and (stack^.value = 10) then writeln('top is 10');
  stack := nil;
  if (stack = nil) or (stack^.value = 0) then writeln('empty');
end.
