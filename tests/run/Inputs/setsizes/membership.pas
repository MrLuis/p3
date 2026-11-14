program inx;
type
  TKind = (kA, kB, kC, kD, kE, kF, kG, kH);
var
  n, calls: integer;
  c: char;
  k: TKind;

function F(v: integer): integer;
begin
  calls := calls + 1;
  F := v;
end;

function Count(lo, hi: integer): integer;
var i, t: integer;
begin
  t := 0;
  for i := lo to hi do
  begin
    if i in [5] then t := t + 1;
    if i in [3..9] then t := t + 10;
    if i in [1, 3, 5, 60, 62] then t := t + 100;
    if i in [0..9, 100..120, 200, 250..255] then t := t + 1000;
    if i in [0, 10..20, 70, 100..120, 200, 250..255] then t := t + 10000;
    if i in [] then t := t + 1;
    if i in [9..3] then t := t + 1;
    if i in [F(4), 7, F(10)..F(12)] then t := t + 100000;
    if i in [F(12)..F(10)] then t := t + 1;
  end;
  Count := t;
end;

begin
  calls := 0;
  writeln(Count(-5, 300), ' ', calls);
  n := 0;
  for c := #0 to #255 do
  begin
    if c in ['a'..'z', 'A'..'Z', '_'] then n := n + 1;
    if c in ['0'..'9', 'a'..'f', 'A'..'F'] then n := n + 1000;
    if c in [' ', #9, #10, #13] then n := n + 100000;
  end;
  writeln(n);
  n := 0;
  for k := kA to kH do
  begin
    if k in [kB, kD..kF] then n := n + 1;
    if not (k in [kA, kH]) then n := n + 10;
    if k in [kC] then n := n + 100;
  end;
  writeln(n);
  writeln(true in [false], true in [true], false in [false..true]);
end.
