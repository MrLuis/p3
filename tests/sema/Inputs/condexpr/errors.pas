program errors;
{$H+}
type
  TRec = record
    a: Integer;
  end;
const
  K = if True then 1 else 2;
var
  x: Integer;
  s: string;
  r: TRec;
  b: Boolean;
begin
  x := if x then 1 else 2;
  x := if b then 1 else 'a';
  x := if b then 1 else 2.5;
  s := if b then 1 else 2;
  var v := if b then x else s;
  var w := if b then r else 5;
  var n := if b then nil else nil;
  x := if b then nope else 2;
  (if b then x else x) := 1;
  x := if b then 1 else 2;
  s := if b then 'a' else s;
end.
