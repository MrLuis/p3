{ Explicit SIMD vectors (llvm-23.md LB-6): the four shapes p3's vector
  type is for. Whole-vector chains (f64 and i32) run on the machine's
  vector units; the element loops over a dynamic array of vectors and
  over plain doubles show the memory-bound and the auto-vectorized
  forms for reference. bench/rust/vectors.rs is the faithful port,
  printing the same values. }
program vectors;
{$H+}
type
  TVD = vector[4] of double;
  TVI = vector[4] of longint;

const
  M = 5000000;   { the whole-vector chains' iterations per pass }
  K = 500000;    { the dynamic arrays' length (vectors, or doubles) }
  ROUNDS = 8;
  MROUNDS = 10;

var
  a, b, c: TVD;
  p, q: TVI;
  pool1, pool2: array of TVD;
  lanes, plains: array of double;
  pass, i, j: longint;
  s: double;

procedure InitVectors;
begin
  { 1.0, -2.25, 0.75, 3.125 and 0.5, -0.125, 2.0, -1.75 in the lanes }
  a[1] := 1.0;    a[2] := -2.25;   a[3] := 0.75;    a[4] := 3.125;
  b[1] := 0.5;    b[2] := -0.125;  b[3] := 2.0;     b[4] := -1.75;
  c[1] := 0.0;    c[2] := 0.0;     c[3] := 0.0;     c[4] := 0.0;
  { the lanes' low bits make every element different }
  p[1] := 12345;  p[2] := -67890;  p[3] := 246809;  p[4] := -135791;
  q[1] := -1;     q[2] := 3;       q[3] := -7;      q[4] := 11;
end;

function FVectorChain: double;
begin
  for pass := 1 to ROUNDS do
    for i := 1 to M do
      c := c * 0.999 + a * b;
  FVectorChain := c[1] * 10000 + c[2] * 100 + c[3] * 10 + c[4];
end;

function IVectorChain: longint;
begin
  for pass := 1 to ROUNDS do
    for i := 1 to M do
      p := p + q * q;  { the lanes wrap: no per-lane checks }
  IVectorChain := p[1] + (p[4] mod 1000000);
end;

function PoolChain: double;
begin
  setLength(pool1, K);
  setLength(pool2, K);
  for i := 0 to K - 1 do
    for j := 1 to 4 do
    begin
      pool1[i][j] := (i * 3 + j) mod 7 + (i mod 13) * 0.25;
      pool2[i][j] := (i * 5 + j * 2) mod 11 - 5.0;
    end;
  for pass := 1 to MROUNDS do
    for i := 0 to K - 1 do
      pool1[i] := pool1[i] * 0.99 + pool2[i];
  PoolChain := pool1[0][1] * 1000 + pool1[K - 1][4] * 100;
end;

function PlainChain: double;
begin
  setLength(lanes, K * 4);
  setLength(plains, K * 4);
  for i := 0 to K * 4 - 1 do
  begin
    lanes[i] := (i * 7) mod 9 + (i mod 17) * 0.125;
    plains[i] := (i * 3) mod 13 - 6.0;
  end;
  for pass := 1 to MROUNDS do
    for i := 0 to K * 4 - 1 do
      lanes[i] := lanes[i] * 0.99 + plains[i] * 0.5;
  PlainChain := lanes[0] * 100 + lanes[K * 4 - 1] * 10;
end;

begin
  InitVectors;
  s := FVectorChain;
  writeln('fvec ', s:12:8);
  writeln('ivec ', IVectorChain);
  writeln('pool ', PoolChain:14:8);
  writeln('plain ', PlainChain:14:8);
  writeln('rounds ', ROUNDS, ' ', MROUNDS);
end.
