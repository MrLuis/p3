{ Real arithmetic: a particle simulation and a Mandelbrot set. }
program reals;
const
  N = 64;
type
  TVec = array[1..N] of double;
var
  px, py, vx, vy, m: TVec;

function Simulate(steps: longint): double;
var s, i, j: longint; dx, dy, d2, inv, ax, ay, e: double;
begin
  for i := 1 to N do
  begin
    px[i] := i * 1.5;
    py[i] := (i mod 7) * 2.0;
    vx[i] := 0.0;
    vy[i] := 0.0;
    m[i] := 1.0 + (i mod 3);
  end;
  for s := 1 to steps do
  begin
    for i := 1 to N do
    begin
      ax := 0.0;
      ay := 0.0;
      for j := 1 to N do
        if i <> j then
        begin
          dx := px[j] - px[i];
          dy := py[j] - py[i];
          d2 := dx * dx + dy * dy + 0.01;
          inv := m[j] / (d2 * sqrt(d2));
          ax := ax + dx * inv;
          ay := ay + dy * inv;
        end;
      vx[i] := vx[i] + ax * 0.001;
      vy[i] := vy[i] + ay * 0.001;
    end;
    for i := 1 to N do
    begin
      px[i] := px[i] + vx[i] * 0.001;
      py[i] := py[i] + vy[i] * 0.001;
    end;
  end;
  e := 0.0;
  for i := 1 to N do
    e := e + m[i] * (vx[i] * vx[i] + vy[i] * vy[i]);
  Simulate := e;
end;

function Mandelbrot(size: longint): longint;
var x, y, it, inside: longint; cr, ci, zr, zi, t: double;
begin
  inside := 0;
  for y := 0 to size - 1 do
    for x := 0 to size - 1 do
    begin
      cr := 3.0 * x / size - 2.0;
      ci := 2.0 * y / size - 1.0;
      zr := 0.0;
      zi := 0.0;
      it := 0;
      while (it < 100) and (zr * zr + zi * zi <= 4.0) do
      begin
        t := zr * zr - zi * zi + cr;
        zi := 2.0 * zr * zi + ci;
        zr := t;
        it := it + 1;
      end;
      if it = 100 then
        inside := inside + 1;
    end;
  Mandelbrot := inside;
end;

begin
  writeln(Simulate(6000):0:6);
  { in thousands: points on the border fall either way with the
    compiler's rounding (fused multiply-add or not) }
  writeln(Mandelbrot(1500) div 1000);
end.
