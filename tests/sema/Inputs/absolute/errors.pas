program errors;
type
  TRec = record a: longint end;
  TObj = class f: longint; procedure M; end;
const
  c = 5;
var
  i: longint;
  p: ^longint;
  o: TObj;
  arr: array[1..4] of longint;
  e1: longint absolute nope;
  e2: longint absolute 4096;
  e3: longint absolute c;
  e4: longint absolute p^;
  e5: longint absolute o.f;
  e6: longint absolute arr[i];
  e7: longint absolute i + 1;
  e8: longint absolute i = 3;
  e9: longint absolute e9;
  e10: longint absolute arr[9];
  e11: longint absolute longint(i);
  e12: longint absolute TRec;
procedure TObj.M;
var g: longint absolute f;
begin g := 1 end;
procedure Q(const a: array of const);
var v: longint absolute a;
begin v := 1 end;
begin
end.
