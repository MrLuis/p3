{ Dynamic arrays: SetLength growth, copies, passing (reference counts),
  arrays of arrays. }
program dynarrays;
type
  TInts = array of longint;
  TGrid = array of TInts;

{ Grows one element at a time, as a list built by appending does. }
function Build(n, seed: longint): TInts;
var
  i: longint;
begin
  Result := nil;
  for i := 0 to n - 1 do
  begin
    SetLength(Result, length(Result) + 1);
    Result[i] := (seed * 31 + i * 17) mod 1000;
  end;
end;

{ Takes the array by value: a reference, counted. }
function Sum(a: TInts): int64;
var
  i: longint;
begin
  Result := 0;
  for i := 0 to high(a) do
    Result := Result + a[i];
end;

{ Insertion sort of a copy; the original is left alone. }
function Sorted(const a: TInts): TInts;
var
  i, j, x: longint;
begin
  Result := Copy(a);
  for i := 1 to high(Result) do
  begin
    x := Result[i];
    j := i - 1;
    while (j >= 0) and (Result[j] > x) do
    begin
      Result[j + 1] := Result[j];
      j := j - 1;
    end;
    Result[j + 1] := x;
  end;
end;

function GridRounds(n: longint): int64;
var
  g: TGrid;
  row: TInts;
  r, i, j: longint;
  total: int64;
begin
  total := 0;
  for r := 1 to n do
  begin
    SetLength(g, 16);
    for i := 0 to high(g) do
    begin
      SetLength(g[i], 8 + (r + i) mod 8);
      for j := 0 to high(g[i]) do
        g[i][j] := (r * i + j) mod 97;
    end;
    for i := 0 to high(g) do
    begin
      row := g[i]; { shares the row }
      total := total + Sum(row) + length(row);
    end;
    g := nil;
  end;
  GridRounds := total;
end;

function ListRounds(n: longint): int64;
var
  r: longint;
  a, s: TInts;
  total: int64;
begin
  total := 0;
  for r := 1 to n do
  begin
    a := Build(200, r);
    s := Sorted(a);
    total := total + Sum(s) + s[0] + s[high(s)] + a[0];
  end;
  ListRounds := total;
end;

begin
  writeln(ListRounds(6000));
  writeln(GridRounds(120000));
end.
