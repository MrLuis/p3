{ A call of a call's result as a statement, f(x)(y); and chains of them:
  through methods, with, empty argument lists, evaluation order. Output
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/callchains.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/callchains.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/callchains.expected %t.g.out
}
program callchains;
{$H+}
type
  TE = procedure(x: longint) of object;
  TN = procedure of object;
  TF = function(x: longint): longint of object;
  TS = function(const s: string): string of object;
  TG = function: TE of object;
  TA = class
    w: longint;
    procedure H(x: longint);
    procedure B;
    function Inc1(x: longint): longint;
    function Up(const s: string): string;
    function GetE: TE;
    function GetG: TG;
    procedure Inside;
  end;
var
  log: longint = 0;
  ga: TA;
function Tick(n: longint): longint;
begin
  log := log * 10 + n;
  Tick := n;
end;
procedure TA.H(x: longint); begin writeln('H ', w + x); end;
procedure TA.B; begin writeln('B ', w); end;
function TA.Inc1(x: longint): longint; begin Inc1 := x + w; end;
function TA.Up(const s: string): string; begin Result := s + '!'; writeln(Result); end;
function TA.GetE: TE; begin GetE := @H; end;
function TA.GetG: TG; begin GetG := @GetE; end;
procedure TA.Inside;
begin
  GetE()(50);
  GetG()()(51);
  Self.GetE()(52);
end;
function Get(a: TA): TE; begin Get := @a.H; end;
function GetN(a: TA): TN; begin GetN := @a.B; end;
function GetF(a: TA): TF; begin GetF := @a.Inc1; end;
function GetS(a: TA): TS; begin GetS := @a.Up; end;
function GetTick(n: longint): TE; begin log := log * 10 + n; GetTick := @ga.H; end;
function NoArg: TE; begin NoArg := @ga.H; end;
function Boom(a: TA): TE; begin raise TObject.Create; Boom := @a.H; end;
var
  i: longint;
begin
  ga := TA.Create; ga.w := 100;
  Get(ga)(1);
  GetN(ga)();
  GetN(ga);
  GetF(ga)(2);
  GetS(ga)('s');
  writeln(GetF(ga)(3), ' ', GetF(ga)(GetF(ga)(4)));
  NoArg()(5);
  GetTick(1)(Tick(2)); writeln(log);
  for i := 1 to 2 do Get(ga)(i * 10);
  if ga.w > 0 then Get(ga)(6) else GetN(ga)();
  begin Get(ga)(7) end;
  with ga do GetE()(8);
  with ga do GetG()()(9);
  ga.GetG()()(10);
  ga.Inside;
  try
    Boom(ga)(11);
  except
    writeln('caught');
  end;
  ga.Free;
end.
