{ Records: functions returning large records, record copies, records
  with string fields. }
program records;
type
  TBox = record
    x0, y0, x1, y1: int64;
    weight, tag: int64;
  end;
  TPerson = record
    name, city: ansistring;
    age: longint;
  end;

function MakeBox(i: longint): TBox;
begin
  Result.x0 := i mod 97;
  Result.y0 := i mod 89;
  Result.x1 := Result.x0 + i mod 13 + 1;
  Result.y1 := Result.y0 + i mod 11 + 1;
  Result.weight := i mod 7;
  Result.tag := i;
end;

{ The smallest box holding both. }
function Union(const a, b: TBox): TBox;
begin
  Result := a;
  if b.x0 < Result.x0 then Result.x0 := b.x0;
  if b.y0 < Result.y0 then Result.y0 := b.y0;
  if b.x1 > Result.x1 then Result.x1 := b.x1;
  if b.y1 > Result.y1 then Result.y1 := b.y1;
  Result.weight := (a.weight + b.weight) mod 1000;
  Result.tag := a.tag xor b.tag;
end;

function Shifted(b: TBox; d: int64): TBox;
begin
  b.x0 := b.x0 + d;
  b.x1 := b.x1 + d;
  Shifted := b;
end;

function Area(const b: TBox): int64;
begin
  Area := (b.x1 - b.x0) * (b.y1 - b.y0);
end;

function BoxRounds(n: longint): int64;
var
  i, j: longint;
  acc, b: TBox;
  boxes: array[0..255] of TBox;
  total: int64;
begin
  total := 0;
  for j := 0 to 255 do
    boxes[j] := MakeBox(j);
  for i := 1 to n do
  begin
    acc := MakeBox(i);
    for j := 0 to 15 do
    begin
      b := Shifted(boxes[(i + j) and 255], j mod 3);
      acc := Union(acc, b);
    end;
    boxes[i and 255] := acc;
    total := total + Area(acc) mod 1000 + acc.weight;
  end;
  BoxRounds := total;
end;

function MakePerson(i: longint): TPerson;
const
  Names: array[0..4] of ansistring = ('Ada', 'Brian', 'Carmen', 'Dmitri', 'Eve');
  Cities: array[0..3] of ansistring = ('Lima', 'Oslo', 'Quito', 'Turin');
begin
  Result.name := Names[i mod 5];
  Result.city := Cities[i mod 4];
  Result.age := 20 + i mod 50;
end;

function Older(const a, b: TPerson): TPerson;
begin
  if a.age >= b.age then
    Older := a
  else
    Older := b;
end;

function PersonRounds(n: longint): int64;
var
  i, j: longint;
  people: array[0..63] of TPerson;
  best, p: TPerson;
  total: int64;
begin
  total := 0;
  for j := 0 to 63 do
    people[j] := MakePerson(j);
  for i := 1 to n do
  begin
    best := people[i and 63];
    for j := 0 to 7 do
    begin
      p := people[(i * 7 + j) and 63];
      best := Older(best, p);
    end;
    people[(i * 3) and 63] := best;
    total := total + best.age + length(best.name) + length(best.city);
  end;
  PersonRounds := total;
end;

begin
  writeln(BoxRounds(2000000));
  writeln(PersonRounds(2000000));
end.
