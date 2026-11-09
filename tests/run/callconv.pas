{ Calling convention directives (cdecl, stdcall; FPC, --mode=objfpc) on
  routines, forward and external declarations, methods, interface methods
  and procedural types. pcc calls everything with the C convention; as in
  Free Pascal on 64-bit targets, stdcall is the default convention. The
  output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/callconv.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/callconv.expected %t.O2.out
}
program callconv;

type
  TStd = function(x: integer): integer; stdcall;
  TPlain = function(x: integer): integer;
  TC = function(x: integer): integer cdecl;
  TProc = procedure(const s: string); stdcall;

  IGreeter = interface
    function Greet(n: integer): integer; stdcall;
    procedure Say(const s: string); cdecl;
  end;

  TBase = class
    function Twice(x: integer): integer; virtual; cdecl;
    function Name: string; virtual; stdcall;
  end;

  TDerived = class(TBase)
    function Twice(x: integer): integer; override; { inherits cdecl }
    function Name: string; override;
  end;

  TGreeter = class(TInterfacedObject, IGreeter)
    function Greet(n: integer): integer; stdcall;
    procedure Say(const s: string); cdecl;
  end;

function labs(x: int64): int64; cdecl; external 'c' name 'labs';
function cabs(x: longint): longint; stdcall; external 'c' name 'abs';

function Square(x: integer): integer; stdcall;
begin
  Square := x * x;
end;

function Cube(x: integer): integer; stdcall; forward;

function Inc1(x: integer): integer;
begin
  Inc1 := x + 1;
end;

function Neg(x: integer): integer; cdecl;
begin
  Neg := -x;
end;

function Cube(x: integer): integer; { the forward declaration's stdcall }
begin
  Cube := x * x * x;
end;

procedure Show(const s: string); stdcall;
begin
  writeln('show: ', s);
end;

function TBase.Twice(x: integer): integer; cdecl;
begin
  Twice := 2 * x;
end;

function TBase.Name: string; stdcall;
begin
  Name := 'base';
end;

function TDerived.Twice(x: integer): integer; cdecl;
begin
  Twice := 20 * x;
end;

function TDerived.Name: string;
begin
  Name := 'derived';
end;

function TGreeter.Greet(n: integer): integer; stdcall;
begin
  Greet := n + 100;
end;

procedure TGreeter.Say(const s: string); cdecl;
begin
  writeln('say: ', s);
end;

procedure Apply(f: TStd; x: integer);
begin
  writeln('apply: ', f(x));
end;

var
  s: TStd;
  p: TPlain;
  c: TC;
  pr: TProc;
  b: TBase;
  g: IGreeter;
begin
  s := @Square;
  writeln(s(7));
  s := @Cube;
  writeln(s(3));
  s := @Inc1; { stdcall is the default convention }
  writeln(s(41));
  p := @Square;
  writeln(p(5));
  p := s;
  writeln(p(1));
  c := @Neg;
  writeln(c(9));
  Apply(@Square, 12);
  pr := @Show;
  pr('hello');
  writeln(labs(-1234567890123), ' ', cabs(-42));

  b := TDerived.Create;
  writeln(b.Twice(4), ' ', b.Name);
  b.Free;
  b := TBase.Create;
  writeln(b.Twice(4), ' ', b.Name);
  b.Free;

  g := TGreeter.Create;
  writeln(g.Greet(5));
  g.Say('world');
end.
