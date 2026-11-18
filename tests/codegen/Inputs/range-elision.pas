program range_elision;
var
  a: array[1..10] of longint;

function Known: longint;
var
  i: longint;
begin
  for i := 1 to 10 do
    a[i] := i + 1;
  Known := a[1];
end;

function Unknown(j: longint): longint;
begin
  a[j] := j + 1;
  Unknown := a[j];
end;

begin
  writeln(Known, Unknown(3));
end.
