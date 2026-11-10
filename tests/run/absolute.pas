{ Variables declared `absolute` (Turbo/Free Pascal): another view of a
  variable, a field of a record or an element at a constant index, with a
  type of its own and no storage of its own. Output is identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/absolute.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/absolute.expected
  RUN: %pcc --mode=objfpc -O3 -fno-range-checks -fno-overflow-checks %s -o %t.O3
  RUN: %t.O3 | diff - %S/absolute.expected
  RUN: %pcc --mode=objfpc -O2 -fno-inline-runtime %s -o %t.ni
  RUN: %t.ni | diff - %S/absolute.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/absolute.expected
}
program absolutes;
{$H+}
uses sysutils;

type
  TRec = record a, b: longint end;
  TNest = record inner: record j: longint end end;
  TBytes4 = array[0..3] of byte;
  TObj = class
    f: longint;
    procedure Show;
  end;

const
  tc: longint = 7;

var
  x: longint;
  y: longint absolute x;
  bytes: TBytes4 absolute x;
  first: byte absolute bytes;     { an overlay of an overlay }
  r: TRec;
  rb: longint absolute r.b;
  arr: array[1..4] of longint;
  a2: longint absolute arr[2];
  grid: array[0..1, 0..1] of longint;
  g11: longint absolute grid[1, 1];
  n: TNest;
  nj: longint absolute n.inner.j;
  tcv: longint absolute tc;
  absolute: longint;              { not a reserved word }
  old: longint absolute x deprecated;
  ss: string[10];
  slen: byte absolute ss;
  as1: ansistring;
  asp: pointer absolute as1;
  as2: ansistring absolute as1;
  d: double;
  dbits: int64 absolute d;

procedure TObj.Show;
var
  me: pointer absolute Self;
begin
  writeln('self: ', me = pointer(Self));
end;

{ parameters of every kind, a local, a nested routine using the overlays }
procedure Params(v: longint; var w: longint; const c: longint);
var
  a: longint absolute v;
  b: longint absolute w;
  e: longint absolute c;
  l: longint;
  m: longint absolute l;
  m2: longint absolute m;

  procedure Bump;
  begin
    m2 := m2 + 1;
    a := a + 1;
    b := b + 1;
  end;

begin
  a := 2;
  writeln('v=', v);
  b := 3;
  writeln('w=', w);
  writeln('c=', e);
  l := 1;
  Bump;
  writeln(l, ' ', v, ' ', w);
end;

{ a nested routine's own overlay of an enclosing routine's variable }
function Outer: longint;
var
  count: longint;

  procedure Inner;
  var
    c: longint absolute count;
  begin
    c := c + 5;
  end;

begin
  count := 1;
  Inner;
  Inner;
  Outer := count;
end;

{ the usual use: giving an untyped parameter a type }
function SumBytes(const data; n: longint): longint;
var
  b: array[0..15] of byte absolute data;
  i: longint;
begin
  Result := 0;
  for i := 0 to n - 1 do
    Result := Result + b[i];
end;

procedure Clear(var data);
var
  l: longint absolute data;
begin
  l := 0;
end;

procedure Fields(var v: TRec);
var
  z: longint absolute v.b;
begin
  z := 5;
end;

procedure Strs(s: shortstring; const o: array of longint);
var
  c: byte absolute s;
  f0: longint absolute o;
begin
  writeln(c, ' ', f0);
end;

function ByName(n: longint): longint;
var
  res: longint absolute ByName;
begin
  res := n + 1;
end;

function ByResult(n: longint): longint;
var
  res: longint absolute Result;
begin
  res := n + 2;
end;

function ByNested(n: longint): longint;
  procedure Assign;
  var
    res: longint absolute ByNested;
  begin
    res := n * 2;
  end;
begin
  Assign;
end;

function Forwarded(n: longint): longint; forward;

function Forwarded(n: longint): longint;
var
  res: longint absolute Forwarded;
begin
  res := n + 3;
end;

function Str(n: longint): shortstring;
var
  len: byte absolute Str;
begin
  Str := 'abcdef';
  len := n;
end;

{ a global seen from a routine }
function Global: longint;
var
  g: longint absolute x;
begin
  g := 4;
  Global := x;
end;

{ overlays of managed values are not released: only what they overlay is }
function Managed: longint;
var
  s: ansistring;
  t: ansistring absolute s;
  p: pointer absolute s;
begin
  s := IntToStr(1234);
  t := t + '5';
  if p <> nil then
    Managed := length(s)
  else
    Managed := 0;
end;

{ a value changed through an overlay is there when an exception arrives }
function Guarded: longint;
var
  v: longint;
  o: longint absolute v;
begin
  v := 1;
  try
    o := 2;
    raise Exception.Create('x');
  except
    Guarded := v;
  end;
end;

var
  q: longint;
  obj: TObj;
  b: array[0..3] of byte;
begin
  x := $01020304;
  writeln(y, ' ', bytes[0], ' ', bytes[3], ' ', first);
  y := 7;
  writeln(x);
  bytes[1] := 1;
  writeln(x);
  r.b := 9;
  writeln(rb);
  rb := 3;
  writeln(r.b, ' ', r.a);
  arr[2] := 5;
  writeln(a2);
  a2 := 6;
  writeln(arr[2], ' ', arr[1], ' ', arr[3]);
  grid[1, 1] := 8;
  writeln(g11);
  n.inner.j := 3;
  writeln(nj);
  writeln(tcv);
  tcv := 8;
  writeln(tc);
  absolute := 1;
  writeln(absolute);
  ss := 'abc';
  writeln(slen);
  slen := 2;
  writeln(ss);
  as1 := 'hello';
  as2 := 'bye';
  writeln(as1, ' ', asp <> nil);
  d := 1.0;
  writeln(dbits = $3FF0000000000000);
  q := 0;
  Params(1, q, 4);
  writeln(q);
  writeln(Outer);
  b[0] := 1; b[1] := 2; b[2] := 3; b[3] := 4;
  writeln(SumBytes(b, 4));
  q := 77;
  Clear(q);
  writeln(q);
  Fields(r);
  writeln(r.b);
  Strs('xy', [3, 4]);
  writeln(ByName(1), ' ', ByResult(1), ' ', ByNested(4), ' ', Forwarded(1));
  writeln(Str(3), ' ', Global);
  writeln(Managed, ' ', Guarded);
  obj := TObj.Create;
  obj.Show;
  obj.Free;
end.
