program errors;
type
  TE = procedure(x: longint) of object;
  TL = procedure(x: int64) of object;
  TP = procedure(x: longint);
  TF = function(x: longint): longint of object;
  TA = class
    procedure H(x: longint);
    procedure O(x: longint); overload;
    procedure O(x: int64); overload;
    constructor Make(x: longint);
    class procedure C(x: longint);
  end;
procedure TA.H(x: longint); begin end;
procedure TA.O(x: longint); begin end;
procedure TA.O(x: int64); begin end;
constructor TA.Make(x: longint); begin end;
class procedure TA.C(x: longint); begin end;
procedure Plain(x: longint); begin end;
procedure Run(e: TE; var v: TE); begin end;
procedure Open(const a: array of longint); begin end;
var
  a: TA;
  e: TE;
  l: TL;
  p: TP;
  f: TF;
  n: longint;
  arr: array[1..3] of longint;
  bad: array[1..3] of byte;
begin
  e := @Plain;
  p := @a.H;
  l := @a.H;
  f := @a.H;
  e := @TA.H;
  e := @a.Make;
  e := @a.Free;
  e := a;
  if e = p then n := 1;
  if e = 1 then n := 1;
  Run(@Plain, e);
  Run(@a.H, @a.H);
  n := @a.O = e;
  Open(slice(bad, 2));
  Open(slice(n, 2));
  Open(slice(arr));
  Open(slice(arr, 'x'));
  n := length(slice(arr, 2));
end.
