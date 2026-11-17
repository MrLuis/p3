{ TMethod: reading and writing a method pointer's Code and Data, building one
  from @TFoo.M, conversions both ways, fields, properties, function results.
  Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/tmethod.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/tmethod.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/tmethod.expected %t.g.out
}
program tmethod;
{$H+}
type
  TE = procedure(x: longint) of object;
  TF = function: longint of object;
  TA = class
    w: longint;
    FE: TE;
    procedure H(x: longint);
    procedure V(x: longint); virtual;
    function G: longint;
    class procedure C(x: longint);
    function GetE: TE;
    procedure O(x: longint); overload;
    procedure O(x: int64); overload;
    procedure Rebind(const m: TMethod);
    property PE: TE read FE write FE;
    property PG: TE read GetE;
  end;
  TB = class(TA)
    procedure V(x: longint); override;
    procedure H2(x: longint);
  end;
  THold = record
    m: TMethod;
    e: TE;
  end;
procedure TA.H(x: longint); begin writeln(ClassName, ' H ', w + x); end;
procedure TA.V(x: longint); begin writeln('TA.V ', w + x); end;
function TA.G: longint; begin G := w; end;
class procedure TA.C(x: longint); begin writeln(ClassName, ' C ', x); end;
function TA.GetE: TE; begin GetE := FE; end;
procedure TA.O(x: longint); begin writeln('O int ', x); end;
procedure TA.O(x: int64); begin writeln('O i64 ', x); end;
procedure TA.Rebind(const m: TMethod); begin TMethod(FE) := m; end;
procedure TB.V(x: longint); begin writeln('TB.V ', w + x); end;
procedure TB.H2(x: longint); begin writeln('H2 ', w + x); end;
function Get(a: TA): TE; begin Get := @a.H; end;
function DataOf(e: TE): pointer; begin DataOf := TMethod(e).Data; end;
function CodeOf(const e: TE): pointer; begin CodeOf := TMethod(e).Code; end;
procedure Clear(var e: TE); begin TMethod(e).Data := nil; TMethod(e).Code := nil; end;
procedure LocalCopy(e: TE); begin TMethod(e).Data := nil; writeln(TMethod(e).Data = nil); end;
procedure IsNotNil(p: pointer); begin writeln(p <> nil); end;
function Same(a, b: TE): boolean;
begin Same := (TMethod(a).Code = TMethod(b).Code) and (TMethod(a).Data = TMethod(b).Data); end;
var
  a, b: TA;
  e, f: TE;
  fn: TF;
  m, n: TMethod;
  h: THold;
  p: pointer;
  o: TObject;
  arr: array[1..2] of TE;
begin
  a := TA.Create; a.w := 10;
  b := TB.Create; b.w := 20;
  writeln(sizeof(TMethod), ' ', sizeof(m));
  m.Code := nil; m.Data := nil; writeln(m.Code = nil, m.Data = nil);
  e := nil; writeln(TMethod(e).Code = nil, TMethod(e).Data = nil);
  e := @a.H; writeln(TMethod(e).Data = pointer(a), TMethod(e).Code <> nil);
  o := TObject(TMethod(e).Data); writeln(o.ClassName);
  TMethod(e).Data := b; e(1);
  TMethod(e).Code := @TB.H2; e(2);
  TMethod(e).Code := @TA.H; TMethod(e).Data := a; e(3);
  TMethod(f).Code := @TA.H; TMethod(f).Data := b; f(4);
  writeln(Same(e, f)); f := e; writeln(Same(e, f));
  p := @TA.V; TMethod(e).Code := p; TMethod(e).Data := b; e(5);
  p := @TB.V; TMethod(e).Code := p; e(6);
  p := @TB.H; TMethod(e).Code := p; e(7);
  p := @TA.C; TMethod(e).Code := p; TMethod(e).Data := TA; e(8);
  TMethod(e).Data := TB; e(9);
  p := @TA.O; writeln(p <> nil);
  IsNotNil(@TA.H);
  e := @a.H;
  m := TMethod(e); writeln(m.Data = pointer(a));
  f := TE(m); f(10);
  n := m; n.Data := b; f := TE(n); f(11);
  TMethod(e) := n; e(12);
  fn := @a.G; writeln(TMethod(fn).Data = pointer(a), fn());
  writeln(TMethod(@a.H).Data = pointer(a), TMethod(@b.V).Data = pointer(b));
  writeln(TMethod(Get(a)).Data = pointer(a));
  a.FE := @a.H;
  writeln(TMethod(a.FE).Data = pointer(a), TMethod(a.PE).Data = pointer(a), TMethod(a.PG).Data = pointer(a));
  TMethod(a.FE).Data := b; a.FE(13);
  writeln(DataOf(@a.H) = pointer(a), CodeOf(@a.H) = CodeOf(@b.H));
  Clear(e); writeln(TMethod(e).Code = nil);
  e := @a.H; LocalCopy(e); writeln(TMethod(e).Data = pointer(a));
  TMethod(e).Data := b; h.m := TMethod(e); h.e := TE(h.m); h.e(15);
  arr[1] := @a.H; TMethod(arr[2]) := TMethod(arr[1]); arr[2](16);
  m.Code := @TA.H; m.Data := b; a.Rebind(m); a.FE(17);
  a.Free; b.Free;
end.
