{ Method pointers (of object): @obj.M, @M, @TFoo.M, virtual and class methods,
  overloads, records' methods, parameters, results, fields, properties, nil,
  comparisons. Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/methodpointers.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/methodpointers.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/methodpointers.expected %t.g.out
}
program methodpointers;
{$H+}
{$modeswitch advancedrecords}
uses sysutils;
type
  TEv = procedure(x: longint) of object;
  TFn = function: longint of object;
  TSt = function(const s: string; n: longint): string of object;
  TN = procedure of object;
  TOb = procedure(Sender: TObject) of object;
  TRecR = record a: string; b: longint; end;
  TRf = function(n: longint): TRecR of object;
  TOv = procedure(s: string) of object;
  TA = class
    w: longint;
    name: string;
    FOnEv: TEv;
    FOnFn: TFn;
    FN: TN;
    list: array[0..1] of TEv;
    procedure H(x: longint);
    procedure V(x: longint); virtual;
    function G: longint;
    function Cat(const s: string; n: longint): string;
    function MakeRec(n: longint): TRecR;
    class procedure C(x: longint);
    class procedure VC(x: longint); virtual;
    procedure Ov(x: longint); overload;
    procedure Ov(s: string); overload;
    procedure Bare;
    procedure Fire(x: longint);
    procedure Wire;
    function CallFn: longint;
    procedure Inner;
    function GetEv: TEv;
    procedure SetEv(e: TEv);
    procedure OnClick(Sender: TObject);
    property OnEv: TEv read FOnEv write FOnEv;
    property OnEv2: TEv read GetEv write SetEv;
    property OnFn: TFn read FOnFn write FOnFn;
  end;
  TB = class(TA)
    procedure V(x: longint); override;
    class procedure VC(x: longint); override;
    procedure Wire2;
  end;
  TAdv = record
    v: longint;
    procedure M;
    function Plus(x: longint): longint;
  end;
  TFa = function(x: longint): longint of object;
  THold = record
    e: TEv;
    tag: string;
  end;
  TBtn = class
    OnClick: TOb;
    procedure Click;
  end;
procedure TBtn.Click;
begin if Assigned(OnClick) then OnClick(Self); end;
procedure TAdv.M; begin writeln('M', v); end;
function TAdv.Plus(x: longint): longint; begin Plus := v + x; end;
procedure TA.H(x: longint); begin writeln(ClassName, ' H ', w + x); end;
procedure TA.V(x: longint); begin writeln('TA.V ', x); end;
function TA.G: longint; begin G := w; end;
function TA.Cat(const s: string; n: longint): string; begin Cat := name + s + IntToStr(n); end;
function TA.MakeRec(n: longint): TRecR; begin Result.a := name + IntToStr(n); Result.b := n; end;
class procedure TA.C(x: longint); begin writeln(ClassName, ' C ', x); end;
class procedure TA.VC(x: longint); begin writeln('TA.VC ', ClassName, x); end;
procedure TA.Ov(x: longint); begin writeln('Ov int ', x); end;
procedure TA.Ov(s: string); begin writeln('Ov str ', s); end;
procedure TA.Bare; begin writeln('bare ', w); end;
procedure TA.Fire(x: longint);
begin
  if Assigned(FOnEv) then FOnEv(x);
  if OnEv <> nil then OnEv(x + 1);
  if FN <> nil then FN();
  if Assigned(OnEv2) then OnEv2(x + 2);
end;
procedure TA.Wire;
begin
  FOnEv := @H;
  OnEv := @Self.V;
  FN := @Bare;
  list[0] := @H; list[1] := @V;
  list[0](5); list[1](6);
end;
function TA.CallFn: longint;
begin CallFn := FOnFn() + OnFn() + 1; end;
procedure TA.Inner;
  procedure Local;
  var e: TEv;
  begin e := @H; e(100); end;
begin Local; end;
function TA.GetEv: TEv; begin GetEv := FOnEv; end;
procedure TA.SetEv(e: TEv); begin FOnEv := e; end;
procedure TA.OnClick(Sender: TObject); begin writeln('clicked by ', Sender.ClassName, ' in ', ClassName); end;
procedure TB.V(x: longint); begin writeln('TB.V ', x); end;
class procedure TB.VC(x: longint); begin writeln('TB.VC ', ClassName, x); end;
procedure TB.Wire2;
var e: TEv;
begin e := @V; e(1); e := @C; e(2); e := @VC; e(3); end;
procedure Run(e: TEv; x: longint); begin e(x); end;
procedure RunC(const e: TEv; x: longint); begin e(x); end;
procedure RunV(var e: TEv; n: TEv); begin e := n; end;
function GetIt(a: TA): TEv; begin GetIt := @a.H; end;
function GetExit(a: TA): TEv; begin exit(@a.V); end;
function Boom(a: TA): TEv; begin raise Exception.Create('x'); GetIt(a); end;
var
  a: TA; b: TA;
  e, f: TEv;
  fn: TFn;
  st: TSt;
  rf: TRf;
  n: TN;
  ov: TOv;
  r: TAdv;
  fa: TFa;
  hold: THold;
  arr: array[1..2] of TEv;
  pe: ^TEv;
  btn: TBtn;
  i: longint;
  an: procedure(x: longint) of object;
begin
  a := TA.Create; a.w := 3; a.name := 'nm';
  b := TB.Create; b.w := 7;
  e := nil; writeln(Assigned(e), e = nil, e <> nil);
  e := @a.H; e(1); writeln(Assigned(e), e = nil, e <> nil);
  f := e; f(2); writeln(e = f, e <> f);
  f := @a.H; writeln(e = f); f := @b.H; writeln(e = f); f := @a.V; writeln(e = f); writeln(@a.H = e); f := nil; writeln(e = f);
  e := @b.H; a.w := 10; e(1);
  e := @b.V; e(1); e := @a.V; e(2);
  e := @a.C; e(1); e := @b.C; e(2); e := @TA.C; e(3); e := @TB.C; e(4);
  e := @a.VC; e(1); e := @b.VC; e(2); e := @TA.VC; e(3); e := @TB.VC; e(4);
  fn := @a.G; writeln(fn(), ' ', fn() + 1); a.w := 11; writeln(fn());
  st := @a.Cat; writeln(st('x', 1)); writeln(length(st('abc' + a.name, 22)));
  rf := @a.MakeRec; writeln(rf(4).a, rf(5).b);
  n := @a.Bare; n; n();
  e := @a.Ov; e(1); ov := @a.Ov; ov('s');
  an := @a.H; an(0);
  r.v := 4; n := @r.M; r.v := 5; n(); fa := @r.Plus; writeln(fa(1));
  Run(@a.H, 1); Run(e, 2); RunC(@b.V, 3); e := nil; RunV(e, @a.H); e(4); RunV(f, e); f(5);
  e := GetIt(b); e(7); e := GetExit(b); e(8);
  a.OnEv := @a.H; a.OnEv(1); a.FOnEv(2); e := a.OnEv; e(3);
  a.OnEv2 := @b.V; a.OnEv2(4); e := a.OnEv2; e(5); Run(a.OnEv, 6); Run(a.FOnEv, 7);
  writeln(Assigned(a.OnEv), Assigned(a.OnEv2), Assigned(a.FN), a.OnEv = nil, a.OnEv2 <> nil, a.OnEv = a.FOnEv);
  a.Fire(1);
  a.Wire; a.Fire(2);
  a.FN := nil; a.OnEv := nil; a.Fire(3);
  a.OnFn := @a.G; writeln(a.CallFn); writeln(a.OnFn());
  a.Inner;
  (b as TB).Wire2;
  hold.e := @a.H; hold.tag := 't'; hold.e(1); with hold do e(2);
  arr[1] := @a.H; arr[2] := @b.V; for i := 1 to 2 do arr[i](i);
  new(pe); pe^ := @a.H; pe^(9); dispose(pe);
  writeln(sizeof(e));
  btn := TBtn.Create; btn.Click; btn.OnClick := @a.OnClick; btn.Click; btn.OnClick := @b.OnClick; btn.Click; btn.Free;
  try e := Boom(a); except on x: Exception do writeln(x.Message); end;
  a.Free; b.Free;
end.
