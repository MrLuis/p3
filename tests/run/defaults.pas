{ Default parameter values (--mode=objfpc), as in Free Pascal: the calls that
  leave out trailing arguments pass the values of the routine's heading.
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/defaults.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/defaults.expected
  RUN: %p3 --mode=objfpc -O3 -fno-range-checks -fno-overflow-checks %s -o %t.O3
  RUN: %t.O3 | diff - %S/defaults.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/defaults.expected
}
program defaults;
{$H+}
{$modeswitch advancedrecords}

const
  K = 3;
  Name = 'const';

type
  TE = (a0, a1, a2);
  TS = set of TE;
  TBytes = set of byte;
  PL = ^longint;
  TFn = function(a: longint = 3): longint;
  TProc = procedure(a: longint; b: longint = 9);
  TCb = procedure;
  TEv = procedure(n: longint = 8) of object;
  IThing = interface
    procedure M(a: longint = 4);
  end;
  TThing = class(TInterfacedObject, IThing)
    procedure M(a: longint);
  end;
  TBase = class
    constructor Create(n: longint = 9);
    procedure V(a: longint = 1); virtual;
    function F(a: longint = 5): longint;
    class function C(a: longint = 6): longint;
    procedure Ev(n: longint);
  end;
  TDerived = class(TBase)
    procedure V(a: longint = 2); override;
    procedure Up;
  end;
  TRec = record
    X: longint;
    constructor Make(a: longint; b: longint = 2);
    function Sum(c: longint = 10): longint;
  end;

constructor TBase.Create(n: longint);
begin
  writeln('create ', n);
end;

procedure TBase.V(a: longint);
begin
  writeln('base ', a);
end;

function TBase.F(a: longint): longint;
begin
  Result := a * 2;
end;

class function TBase.C(a: longint): longint;
begin
  Result := a + 1;
end;

procedure TBase.Ev(n: longint);
begin
  writeln('event ', n);
end;

procedure TDerived.V(a: longint);
begin
  writeln('derived ', a);
end;

procedure TDerived.Up;
begin
  inherited V;
  inherited V(7);
end;

procedure TThing.M(a: longint);
begin
  writeln('thing ', a);
end;

constructor TRec.Make(a: longint; b: longint);
begin
  X := a + b;
end;

function TRec.Sum(c: longint): longint;
begin
  Sum := X + c;
end;

procedure All(a: longint; b: longint = K + 1; const c: string = 'x'; d: double = 1.5;
              e: boolean = true; f: char = 'q'; g: TE = a1; h: PL = nil);
begin
  writeln(a, ' ', b, ' ', c, ' ', d:0:1, ' ', e, ' ', f, ' ', ord(g), ' ', h = nil);
end;

function Twice(x: longint = 2): longint;
begin
  Twice := x * 2;
end;

procedure Strs(s: ansistring = 'a'; const t: shortstring = 'b'; u: shortstring = Name;
               v: ansistring = Name + '!'; c: char = #65);
begin
  writeln(s, ' ', t, ' ', u, ' ', v, ' ', c);
end;

procedure Sets(a: TS = [a0, a2]; b: TBytes = [1, 3..5]);
begin
  writeln(a0 in a, ' ', a1 in a, ' ', 4 in b, ' ', 2 in b);
end;

procedure Conv(r: double = 1; q: int64 = 2 * 3; b: byte = 200; w: word = $FFFF);
begin
  writeln(r:0:1, ' ', q, ' ', b, ' ', w);
end;

procedure Nils(o: TBase = nil; x: pointer = nil; f: TCb = nil; s: pchar = nil;
               e: TEv = nil);
begin
  writeln(o = nil, ' ', x = nil, ' ', assigned(f), ' ', s = nil, ' ', assigned(e));
end;

procedure Fwd(a: longint = 1; b: longint = 2); forward;

procedure Fwd(a: longint; b: longint);
begin
  writeln('fwd ', a, ' ', b);
end;

procedure Same(a: longint = K; r: double = 1.5; s: TBytes = [1, 2]); forward;

procedure Same(a: longint = 1 + 2; r: double = 1.5; s: TBytes = [2, 1]);
begin
  writeln('same ', a, ' ', r:0:1, ' ', 2 in s);
end;

function Rec(a: longint = 1): longint;
begin
  if a < 3 then
    Rec := Rec(a + 2)
  else
    Rec := a;
end;

{ overloads: the best fit, with defaults for what is left out }
procedure O(a: longint);
begin
  writeln('O integer ', a);
end;

procedure O(a: string; b: longint = 1);
begin
  writeln('O string ', a, ' ', b);
end;

procedure Nested(a: longint);
  procedure Inner(b: longint = 5);
  begin
    writeln('inner ', a, ' ', b);
  end;
begin
  Inner;
  Inner(6);
end;

function CAdd(a: longint; b: longint = 7): longint; cdecl;
begin
  CAdd := a + b;
end;

function Plain(a: longint): longint;
begin
  Plain := a;
end;

procedure Two(a: longint; b: longint);
begin
  writeln('two ', a, ' ', b);
end;

var
  fn: TFn;
  pr: TProc;
  ev: TEv;
  c: TBase;
  d: TDerived;
  i: IThing;
  r: TRec;
begin
  All(1);
  All(1, 2);
  All(1, 2, 'y', 2.5, false, 'r', a0);
  writeln(Twice, ' ', Twice(), ' ', Twice(4), ' ', Twice + 1);
  Strs;
  Strs('1', '2', '3', '4', 'z');
  Sets;
  Sets([a1]);
  Conv;
  Nils;
  Fwd;
  Fwd(5);
  Same;
  writeln(Rec);
  O('x');
  O(1);
  Nested(4);
  writeln(CAdd(1));
  fn := @Plain;
  writeln(fn(), ' ', fn, ' ', fn(1));
  pr := @Two;
  pr(1);
  pr(1, 2);
  c := TDerived.Create;
  c.V;
  writeln(c.F, ' ', c.F(), ' ', c.F(1), ' ', c.C, ' ', TBase.C);
  d := TDerived(c);
  d.V;
  d.Up;
  ev := @c.Ev;
  ev();
  ev(1);
  c.Free;
  i := TThing.Create;
  i.M;
  i.M(5);
  r := TRec.Make(1);
  writeln(r.X, ' ', r.Sum, ' ', r.Sum(1));
end.
