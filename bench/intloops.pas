{ Integer loops and array indexing: a sieve and a matrix product. }
program intloops;
const
  N = 2000000;
  M = 120;
type
  TMatrix = array[1..M, 1..M] of longint;
var
  sieve: array[2..N] of boolean;
  a, b, c: TMatrix;

function CountPrimes: longint;
var i, j, count: longint;
begin
  for i := 2 to N do
    sieve[i] := true;
  i := 2;
  while i * i <= N do
  begin
    if sieve[i] then
    begin
      j := i * i;
      while j <= N do
      begin
        sieve[j] := false;
        j := j + i;
      end;
    end;
    i := i + 1;
  end;
  count := 0;
  for i := 2 to N do
    if sieve[i] then
      count := count + 1;
  CountPrimes := count;
end;

function Product(rounds: longint): longint;
var r, i, j, k, sum: longint;
begin
  for i := 1 to M do
    for j := 1 to M do
    begin
      a[i, j] := (i + j) mod 7;
      b[i, j] := (i * j) mod 5;
    end;
  sum := 0;
  for r := 1 to rounds do
  begin
    for i := 1 to M do
      for j := 1 to M do
      begin
        sum := 0;
        for k := 1 to M do
          sum := sum + a[i, k] * b[k, j];
        c[i, j] := sum mod 1000;
      end;
    a[1, 1] := c[M, M] mod 7;
  end;
  sum := 0;
  for i := 1 to M do
    sum := (sum + c[i, i]) mod 1000000;
  Product := sum;
end;

var round, primes: longint;
begin
  primes := 0;
  for round := 1 to 30 do
    primes := primes + CountPrimes;
  writeln(primes);
  writeln(Product(100));
end.
