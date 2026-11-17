{ Typed constants and initialized variables of method-pointer types: nil,
  alone and inside records and arrays; they can be assigned afterwards, and
  local ones keep their values between calls. Output identical to Free Pascal
  3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/methodconstants.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/methodconstants.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/methodconstants.expected %t.g.out
}
program methodconstants;
{$H+}
type
  TE = procedure(x: longint) of object;
  TF = function: longint of object;
  TA = class
    w: longint;
    procedure H(x: longint);
    function G: longint;
  end;
  TR = record
    e: TE;
    n: longint;
    s: string;
  end;
  TN = record
    inner: TR;
    list: array[0..1] of TE;
  end;
procedure TA.H(x: longint); begin writeln('H ', w + x); end;
function TA.G: longint; begin G := w; end;
const
  K: TE = nil;
  F: TF = nil;
  R: TR = (e: nil; n: 4; s: 'str');
  A: array[0..1] of TE = (nil, nil);
  N: TN = (inner: (e: nil; n: 1; s: 'in'); list: (nil, nil));
var
  V: TE = nil;
  a1: TA;
procedure Q;
const
  C: TE = nil;
var
  L: TE = nil;
begin
  writeln(Assigned(C), Assigned(L));
  C := @a1.H;
  L := @a1.H;
  C(1);
end;
begin
  a1 := TA.Create; a1.w := 5;
  writeln(Assigned(K), K = nil, Assigned(F), Assigned(V));
  K := @a1.H; K(2); writeln(Assigned(K), K = nil);
  K := nil; writeln(Assigned(K));
  F := @a1.G; writeln(F());
  V := @a1.H; V(3);
  writeln(Assigned(R.e), R.n, R.s);
  R.e := @a1.H; R.e(4);
  writeln(Assigned(A[0]), Assigned(A[1]));
  A[1] := @a1.H; A[1](5); writeln(Assigned(A[0]), Assigned(A[1]));
  writeln(Assigned(N.inner.e), N.inner.n, N.inner.s, Assigned(N.list[1]));
  N.list[1] := @a1.H; N.list[1](6);
  Q; Q;
  writeln(sizeof(K), sizeof(R));
  a1.Free;
end.
