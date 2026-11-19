{ A routine with a label: no local variable's range is followed, since a
  goto may arrive with any value (optimization3.md item 4). }
program rangelabels;
{$Q+}

function labelled(n: longint): longint;
label 1;
var x: longint;
begin
  x := 1;
1:
  x := x + 1;
  if x < n then goto 1;
  labelled := x + 1;
end;

{ The same without the label: x is 1, then 2, 3. }
function straight: longint;
var x: longint;
begin
  x := 1;
  x := x + 1;
  straight := x + 1;
end;

begin
  writeln(labelled(5), ' ', straight);
end.
