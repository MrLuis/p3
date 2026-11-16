{$H+}
program ast;
type
  TFn = reference to function(x: Integer): Integer;

function MakeAdder(n: Integer): TFn;
var
  s: string;
begin
  s := 'abc';
  Result := function(x) => x + n + Length(s);
end;

var
  f: TFn;
begin
  f := MakeAdder(1);
  Writeln(f(2));
end.
