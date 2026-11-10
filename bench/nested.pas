{ Nested routines: the outer routine's loop counters and accumulators are
  its own, a few variables are shared with nested routines. }
program nested;
const
  N = 3000;
type
  TData = array[0..N - 1] of longint;

function Process(seed: longint): longint;
var
  data: TData;
  i, j, pass, swaps, checksum, tmp: longint;
  state: int64;

  function Next: longint;
  begin
    state := (state * 1103515245 + 12345) and $7FFFFFFF;
    Next := longint(state shr 8);
  end;

  procedure Count;
  begin
    swaps := swaps + 1;
  end;

begin
  state := seed;
  swaps := 0;
  for i := 0 to N - 1 do
    data[i] := Next mod 100000;
  { insertion sort: the inner loop uses only this routine's own variables }
  for i := 1 to N - 1 do
  begin
    tmp := data[i];
    j := i - 1;
    while (j >= 0) and (data[j] > tmp) do
    begin
      data[j + 1] := data[j];
      j := j - 1;
    end;
    data[j + 1] := tmp;
    if j + 1 <> i then
      Count;
  end;
  checksum := 0;
  for pass := 1 to 200 do
    for i := 0 to N - 1 do
      checksum := (checksum + data[i] * pass + i) and $FFFFFF;
  Process := checksum + swaps;
end;

{ a walk over a tree with a nested visitor, as in a compiler }
function Walk(depth: longint): longint;
var nodes, leaves, total: longint;
  procedure Visit(d, value: longint);
  var k, local: longint;
  begin
    nodes := nodes + 1;
    local := 0;
    for k := 1 to 8 do
      local := local + (value xor k);
    if d = 0 then
    begin
      leaves := leaves + 1;
      total := (total + local) and $FFFFFF;
    end
    else
    begin
      Visit(d - 1, value * 2);
      Visit(d - 1, value * 2 + 1);
    end;
  end;
begin
  nodes := 0;
  leaves := 0;
  total := 0;
  Visit(depth, 1);
  Walk := nodes + leaves + total;
end;

var r, sum: longint;
begin
  sum := 0;
  for r := 1 to 60 do
    sum := (sum + Process(r)) and $FFFFFF;
  writeln(sum);
  writeln(Walk(23));
end.
