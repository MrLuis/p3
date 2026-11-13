program objfpc;
type
  TA = class
    f: integer;
  end;
  TB = class(TA)
  end;
var
  o: TObject;
  a: array of integer;
  calls: integer;

generic function Twice<T>(v: T): T;
begin
  result := v;
  result += v
end;

function idx: integer;
begin
  calls += 1;
  result := 0
end;

function sum(n: integer): integer;
var
  i: integer;
begin
  result := 0;
  for i := 1 to n do
    result += i
end;

begin
  o := TB.Create;
  (o as TA).f := 2;
  (o as TA).f *= 21;
  writeln((o as TA).f);
  writeln(specialize Twice<integer>(4), ' ', specialize Twice<string>('ab'));
  setlength(a, 1);
  calls := 0;
  a[idx] -= 4;
  writeln(a[0], ' ', calls);
  writeln(sum(10));
  o.Free
end.
