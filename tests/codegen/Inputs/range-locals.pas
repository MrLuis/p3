{ Ranges of local variables and of more operators (optimization3.md
  item 4). Each routine's arithmetic is either proven (no overflow check,
  nsw) or must stay checked. }
program rangelocals;

const
  Limit = 1000;

var
  g: longint;

{ count grows by at most 1 in each of the 1000 iterations: 0 .. 1000. }
function counter(var flags: array of boolean): longint;
var i, count: longint;
begin
  count := 0;
  for i := 1 to Limit do
    if flags[i mod 10] then
      count := count + 1;
  counter := count + 1;
end;

{ Two steps per iteration, one of them down. }
function twosteps(n: longint): longint;
var i, x: longint;
begin
  x := 10;
  for i := 1 to 100 do
  begin
    if n > i then
      x := x + 3
    else
      x := x - 2;
  end;
  twosteps := x * 1000;
end;

{ A stored value keeps its range until the next store. }
function stored(n: longint): longint;
var x, y: longint;
begin
  x := 5;
  y := x * 1000;
  x := n mod 100;
  stored := x * y - (n and 255) + (n div 1000) * 2;
end;

{ Unknown after an if that may change it. }
function merged(n: longint): longint;
var x: longint;
begin
  x := 1;
  if n > 0 then
    x := n;
  merged := x + 1;
end;

{ Written in a while loop: unknown in and after it. }
function inwhile(n: longint): longint;
var x: longint;
begin
  x := 1;
  while n > 0 do
  begin
    x := x * 2;
    n := n - 1;
  end;
  inwhile := x + 1;
end;

{ A for loop's accumulator inside an inner loop: no bound. }
function nested(n: longint): longint;
var i, x: longint;
begin
  x := 0;
  for i := 1 to 10 do
    while n > 0 do
    begin
      x := x + 1;
      n := n - 1;
    end;
  nested := x + 1;
end;

{ A step that is not constant: no bound. }
function notconst(n: longint): longint;
var i, x: longint;
begin
  x := 0;
  for i := 1 to 10 do
    x := x + n;
  notconst := x + 1;
end;

procedure bump(var v: longint);
begin
  v := 7;
end;

{ Written through a var parameter: not followed at all. }
function byvar: longint;
var x: longint;
begin
  x := 1;
  bump(x);
  byvar := x + 1;
end;

{ Its address taken: not followed at all. }
function addressed: longint;
var x: longint; p: ^longint;
begin
  x := 1;
  p := @x;
  p^ := 7;
  addressed := x + 1;
end;

{ Changed in a try statement: unknown after it. }
function intry(n: longint): longint;
var x: longint;
begin
  x := 1;
  try
    x := n;
  finally
    g := 0;
  end;
  intry := x + 1;
end;

var flags: array[0..9] of boolean;
begin
  flags[3] := true;
  writeln(counter(flags), ' ', twosteps(50), ' ', stored(12345), ' ',
          merged(4), ' ', inwhile(3), ' ', nested(5), ' ', notconst(2), ' ',
          byvar, ' ', addressed, ' ', intry(7));
end.
