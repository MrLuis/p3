{ Routines with large local arrays, records and strings, called often:
  what starting locals from zero costs. }
program locals;
type
  TBuf = array[0..4095] of longint;
  TRec = record
    name: string;
    values: array[1..64] of longint;
  end;

function Small(x: longint): longint;
var buf: TBuf; n, i: longint;
begin
  { uses a few elements of a large buffer }
  n := x and 15;
  for i := 0 to n do
    buf[i] := x + i;
  Small := buf[n] - buf[0];
end;

function Name(x: longint): longint;
var s, t: string; r: TRec;
begin
  s := 'item';
  t := s;
  r.name := t;
  r.values[1] := x;
  Name := length(r.name) + r.values[1];
end;

var i, sum: longint;
begin
  sum := 0;
  for i := 1 to 4000000 do
    sum := (sum + Small(i)) and $FFFFFF;
  writeln(sum);
  for i := 1 to 20000000 do
    sum := (sum + Name(i)) and $FFFFFF;
  writeln(sum);
end.
