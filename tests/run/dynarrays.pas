{ Dynamic arrays (not in --mode=iso), as in Free Pascal: `array of T`, indexed
  from 0, reference counted; assignment shares, setlength and copy give a
  variable its own. Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/dynarrays.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/dynarrays.expected
  RUN: %pcc --mode=objfpc -O3 -fno-range-checks -fno-overflow-checks %s -o %t.O3
  RUN: %t.O3 | diff - %S/dynarrays.expected
  RUN: %pcc --mode=objfpc -O2 -fno-inline-runtime %s -o %t.ni
  RUN: %t.ni | diff - %S/dynarrays.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/dynarrays.expected
}
program dynarrays;
{$H+}
uses sysutils;

type
  TI = array of longint;
  TJ = array of longint;
  TS = array of string;
  TM = array of array of longint;
  TC3 = array of array of array of byte;
  TShort = array of shortstring;
  TPt = record
    x, y: longint;
  end;
  TPts = array of TPt;
  TRec = record
    a: TI;
    s: string;
  end;
  TRecs = array of TRec;
  TFix = array[0..1] of TI;
  PI = ^TI;
  IThing = interface
    function Id: longint;
  end;
  TThing = class(TInterfacedObject, IThing)
    FId: longint;
    constructor Create(n: longint);
    destructor Destroy; override;
    function Id: longint;
  end;
  TThings = array of IThing;
  TObj = class
    f: TI;
    function Get: TI;
    property P: TI read f write f;
  end;

const
  CNil: TI = nil;

var
  gnil: TI = nil;

constructor TThing.Create(n: longint);
begin
  FId := n;
end;

destructor TThing.Destroy;
begin
  writeln('destroy ', FId);
  inherited Destroy;
end;

function TThing.Id: longint;
begin
  Id := FId;
end;

function TObj.Get: TI;
begin
  Result := f;
end;

function Mk(n: longint): TI;
var i: longint;
begin
  SetLength(Result, n);
  for i := 0 to n - 1 do
    Result[i] := i * 2;
end;

function MkS(c: char; n: longint): string;
var i: longint;
begin
  SetLength(Result, n);
  for i := 1 to n do
    Result[i] := c;
end;

procedure ByValue(a: TI);
begin
  a[0] := 5;
  SetLength(a, 1);
  writeln(length(a));
end;

procedure ByVar(var a: TI);
begin
  SetLength(a, 1);
end;

procedure ByConst(const a: TI);
begin
  a[0] := 8;
  writeln(length(a));
end;

procedure ByOut(out a: TI);
begin
  writeln('out ', length(a));
  SetLength(a, 2);
end;

procedure OpenC(const a: array of longint);
begin
  writeln(high(a), ' ', a[0]);
end;

function OpenV(a: array of longint): longint;
var i: longint;
begin
  Result := 0;
  a[0] := 100;
  for i := 0 to high(a) do
    Result := Result + a[i];
end;

procedure OpenVar(var a: array of longint);
begin
  a[0] := 1;
end;

procedure Strs(a: TS);
begin
  writeln(length(a), ' ', a[0]);
end;

function EarlyExit(n: longint): TI;
begin
  SetLength(Result, 1);
  if n = 0 then
    exit(Mk(4));
  Result[0] := n;
end;

{ locals are released, also when an exception passes }
procedure Locals(fail: boolean);
var
  l: TS;
  t: TThings;
begin
  SetLength(l, 2);
  l[0] := MkS('l', 2);
  SetLength(t, 1);
  t[0] := TThing.Create(10);
  if fail then
    raise Exception.Create('failed');
  writeln('locals ', l[0], ' ', t[0].Id);
end;

procedure Nested;
var
  a: TI;
  procedure Inner;
  begin
    SetLength(a, 3);
    a[2] := 9;
  end;
begin
  Inner;
  writeln(a[2], ' ', length(a));
end;

var
  a, b: TI;
  j: TJ;
  anon: array of byte;
  s: TS;
  m: TM;
  c3: TC3;
  sh: TShort;
  pts: TPts;
  r, q: TRec;
  recs: TRecs;
  fix: TFix;
  th, th2: TThings;
  o: TObj;
  p: pointer;
  pa: PI;
  i, k: longint;
begin
  writeln(length(a), ' ', high(a), ' ', low(a), ' ', a = nil, ' ', assigned(a));
  SetLength(a, 3);
  a[0] := 1;
  a[2] := 3;
  writeln(length(a), ' ', high(a), ' ', low(a), ' ', a[1], ' ', a = nil, ' ', assigned(a));
  { assignment shares }
  b := a;
  b[0] := 9;
  writeln(a[0], ' ', a = b, ' ', a <> b);
  { setlength unshares, even to the same length }
  SetLength(b, 3);
  b[1] := 7;
  writeln(a[1], ' ', a = b);
  SetLength(b, 5);
  writeln(b[4], ' ', b[1], ' ', length(b));
  SetLength(b, 2);
  writeln(length(b));
  { copy }
  b := Copy(a);
  b[0] := 2;
  writeln(a[0], ' ', length(b));
  b := Copy(a, 1);
  writeln(length(b), ' ', b[0]);
  b := Copy(a, 1, 1);
  writeln(length(b), ' ', b[0]);
  b := Copy(a, 1, 99);
  writeln(length(b));
  b := Copy(a, 5);
  writeln(length(b), ' ', b = nil);
  b := Copy(a, -1, 1);
  writeln(length(b));
  b := Copy(a, -1, 2);
  writeln(length(b), ' ', b[0]);
  { names of one element type are the same type }
  j := a;
  writeln(length(j));
  { nil }
  b := nil;
  writeln(length(b));
  b := a;
  SetLength(b, 0);
  writeln(b = nil, ' ', length(a));
  { strings }
  SetLength(s, 2);
  s[0] := MkS('x', 2);
  s[1] := s[0] + 'y';
  writeln(s[1], ' [', length(s[0]), ']');
  Strs(s);
  SetLength(s, 1);
  SetLength(s, 3);
  writeln('[', s[1], ']');
  s := nil;
  writeln(length(s));
  SetLength(sh, 2);
  sh[1] := 'short';
  writeln(sh[1], ' ', length(sh[0]));
  { arrays of arrays }
  SetLength(m, 2, 3);
  m[1, 2] := 5;
  m[0][0] := 4;
  writeln(length(m), ' ', length(m[0]), ' ', m[1][2], ' ', m[0, 0]);
  SetLength(m[0], 1);
  writeln(length(m[0]), ' ', length(m[1]));
  SetLength(c3, 2, 2, 2);
  c3[1, 1, 1] := 7;
  writeln(length(c3[1][1]), ' ', c3[1][1][1]);
  SetLength(m, 3);
  writeln(length(m[2]), ' ', m[2] = nil);
  SetLength(anon, 1);
  anon[0] := 200;
  for i := low(anon) to high(anon) do
    writeln(anon[i]);
  { records }
  SetLength(pts, 2);
  pts[1].x := 4;
  with pts[1] do
    y := x + 1;
  writeln(pts[1].y, ' ', pts[0].x);
  SetLength(r.a, 2);
  r.a[1] := 4;
  r.s := MkS('r', 2);
  q := r;
  q.a[1] := 6;
  writeln(r.a[1], ' ', q.s);
  r.a := nil;
  writeln(length(r.a), ' ', length(q.a));
  SetLength(recs, 2);
  recs[0] := q;
  recs[1].s := MkS('z', 1);
  SetLength(recs, 3);
  recs[0].a[0] := 3;
  writeln(q.a[0], ' ', recs[1].s, ' [', recs[2].s, ']');
  recs := nil;
  writeln(length(q.a));
  SetLength(fix[1], 2);
  fix[0] := fix[1];
  writeln(length(fix[0]));
  { classes }
  o := TObj.Create;
  SetLength(o.f, 1);
  o.f[0] := 2;
  writeln(o.f[0], ' ', o.Get[0], ' ', length(o.P));
  o.P := nil;
  writeln(length(o.f));
  o.Free;
  { interfaces in arrays are released }
  SetLength(th, 2);
  th[0] := TThing.Create(1);
  th[1] := TThing.Create(2);
  th2 := Copy(th);
  writeln('th2');
  th := nil;
  writeln('th nil');
  SetLength(th2, 1);
  writeln('shrunk');
  th2 := nil;
  writeln('th2 nil');
  { parameters and results }
  a := Mk(3);
  writeln(a[2]);
  ByValue(a);
  writeln(a[0], ' ', length(a));
  ByConst(a);
  writeln(a[0]);
  OpenC(a);
  writeln(OpenV(a), ' ', a[0]);
  OpenVar(a);
  writeln(a[0]);
  b := a;
  ByVar(b);
  writeln(length(b), ' ', length(a));
  ByOut(b);
  writeln(length(b));
  writeln(Mk(2)[1], ' ', length(Mk(4)));
  writeln(length(EarlyExit(0)), ' ', EarlyExit(7)[0]);
  { constructors }
  a := [1, 2, 3];
  writeln(length(a), ' ', a[2]);
  a := [];
  writeln(a = nil);
  s := ['x', MkS('q', 2)];
  writeln(s[1]);
  m := [[1, 2], [3]];
  writeln(length(m), ' ', m[0, 1], ' ', m[1, 0]);
  ByValue([4, 5]);
  Strs(['p']);
  { typecasts and pointers }
  a := Mk(2);
  p := pointer(a);
  writeln(p <> nil, ' ', TI(p)[1]);
  pa := @a;
  SetLength(pa^, 4);
  writeln(length(a));
  Inc(a[0]);
  writeln(a[0]);
  FillChar(a[0], 4, 1);
  writeln(a[0]);
  writeln(sizeof(a));
  { typed constants and initialized variables }
  writeln(length(CNil), ' ', length(gnil));
  Nested;
  Locals(false);
  try
    Locals(true);
  except
    on E: Exception do
      writeln(E.Message);
  end;
  k := 0;
  for i := 0 to high(a) do
    k := k + 1;
  writeln(k);
end.
