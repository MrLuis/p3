{ Out parameters (--mode=objfpc), as in Free Pascal: by reference, like var,
  but an ansistring or interface in the argument is released before the
  call. Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/outparams.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/outparams.expected
  RUN: %pcc --mode=objfpc -O3 -fno-range-checks -fno-overflow-checks %s -o %t.O3
  RUN: %t.O3 | diff - %S/outparams.expected
  RUN: %pcc --mode=objfpc -O2 -fno-inline-runtime %s -o %t.ni
  RUN: %t.ni | diff - %S/outparams.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/outparams.expected
}
program outparams;
{$H+}
{$modeswitch advancedrecords}

type
  IThing = interface
    function Id: longint;
  end;
  TThing = class(TInterfacedObject, IThing)
    FId: longint;
    constructor Create(n: longint);
    destructor Destroy; override;
    function Id: longint;
  end;
  TRec = record
    s: ansistring;
    n: longint;
  end;
  TArr = array[0..1] of ansistring;
  TGet = procedure(out n: longint);
  TBase = class
    S: ansistring;
    procedure V(out r: ansistring); virtual;
  end;
  TDerived = class(TBase)
    procedure V(out r: ansistring); override;
  end;
  IGet = interface
    procedure Fetch(out n: longint);
  end;
  TGetter = class(TInterfacedObject, IGet)
    procedure Fetch(out n: longint);
  end;
  TPoint = record
    X: longint;
    procedure Get(out a: longint);
  end;

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

procedure TBase.V(out r: ansistring);
begin
  writeln('base [', r, '] [', S, ']');
  r := 'base';
end;

procedure TDerived.V(out r: ansistring);
begin
  inherited V(r);
  r := r + '+derived';
end;

procedure TGetter.Fetch(out n: longint);
begin
  n := 11;
end;

procedure TPoint.Get(out a: longint);
begin
  a := X;
end;

function Mk(c: char): ansistring;
begin
  SetLength(Result, 3);
  Result[1] := c;
  Result[2] := c;
  Result[3] := c;
end;

procedure All(out a: longint; out s: ansistring; out r: TRec; out i: IThing);
begin
  writeln(a, ' [', s, '] [', r.s, '] ', r.n, ' ', i = nil);
  a := 2;
  s := 'new';
  r.s := 'rs';
  r.n := 5;
  i := TThing.Create(2);
end;

procedure Plain(out x: longint);
begin
  writeln('plain ', x);
  x := x + 1;
end;

{ aliases of the out argument, before and after it }
procedure AfterConst(out s: ansistring; const t: ansistring);
begin
  writeln('[', t, ']');
  s := 'z';
end;

procedure BeforeConst(const t: ansistring; out s: ansistring);
begin
  writeln('[', t, ']');
  s := 'z';
end;

procedure BeforeValue(v: ansistring; out s: ansistring);
begin
  writeln('[', v, ']');
  s := 'z';
end;

procedure Count(out s: ansistring; n: longint);
begin
  writeln(n);
  s := 'z';
end;

procedure Arr(out a: TArr);
begin
  writeln('[', a[0], '][', a[1], ']');
  a[1] := 'set';
end;

procedure Open(out a: array of ansistring);
begin
  writeln('[', a[0], '] ', length(a));
  a[0] := 'n';
end;

procedure OpenInts(out a: array of longint);
begin
  writeln(high(a), ' ', a[1]);
  a[0] := 4;
end;

procedure Untyped(out a);
begin
  longint(a) := 3;
end;

procedure PassVar(var s: ansistring);
begin
  writeln('var ', length(s));
end;

procedure Forward_(out s: ansistring); forward;

procedure Forward_(out s: ansistring);
begin
  PassVar(s);
  s := 'fwd';
end;

{ passed on, and used from a nested routine }
procedure Outer(out s: ansistring);
  procedure Inner;
  begin
    s := s + 'inner';
  end;
begin
  Forward_(s);
  Inner;
end;

procedure Short(out s: shortstring; out n: longint);
begin
  writeln('[', s, ']');
  s := 'set';
  n := 1;
end;

var
  evals: longint;

function Idx: longint;
begin
  evals := evals + 1;
  Idx := 0;
end;

function GetFive(out n: longint): boolean;
begin
  n := 5;
  GetFive := true;
end;

var
  a, b: longint;
  s, t: ansistring;
  r: TRec;
  i: IThing;
  x: TArr;
  o: array[0..1] of ansistring;
  ints: array[0..2] of longint;
  c: TBase;
  g: TGet;
  ig: IGet;
  p: TPoint;
  ss: shortstring;
  out: longint;       { not a reserved word }
begin
  a := 1;
  s := Mk('o');
  r.s := Mk('p');
  r.n := 9;
  i := TThing.Create(1);
  writeln('call');
  All(a, s, r, i);
  writeln(a, ' ', s, ' ', r.s, ' ', r.n, ' ', i.Id);
  i := nil;
  a := 7;
  Plain(a);
  writeln(a);
  s := Mk('a');
  AfterConst(s, s);
  writeln(s);
  s := Mk('b');
  BeforeConst(s, s);
  writeln(s);
  s := Mk('c');
  BeforeValue(s, s);
  writeln(s);
  s := Mk('d');
  Count(s, length(s));
  writeln(s);
  x[0] := Mk('e');
  x[1] := Mk('f');
  Arr(x);
  writeln('[', x[0], '][', x[1], ']');
  o[0] := Mk('g');
  o[1] := Mk('h');
  Open(o);
  writeln('[', o[0], '][', o[1], ']');
  ints[0] := 0;
  ints[1] := 6;
  OpenInts(ints);
  writeln(ints[0]);
  b := 0;
  Untyped(b);
  writeln(b);
  s := Mk('i');
  Forward_(s);
  writeln(s);
  s := Mk('j');
  Outer(s);
  writeln(s);
  ss := 'kept';
  Short(ss, b);
  writeln(ss);
  x[0] := Mk('k');
  evals := 0;
  Forward_(x[Idx]);
  writeln(evals, ' ', x[0]);
  if GetFive(a) then
    writeln(a);
  c := TDerived.Create;
  c.S := Mk('q');
  c.V(t);
  writeln(t);
  c.V(c.S);
  writeln(c.S);
  c.Free;
  g := @Plain;
  a := 20;
  g(a);
  writeln(a);
  ig := TGetter.Create;
  ig.Fetch(a);
  writeln(a);
  p.X := 12;
  p.Get(a);
  writeln(a);
  out := 1;
  writeln(out);
end.
