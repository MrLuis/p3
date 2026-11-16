program errors;
{$H+}
type
  TFn = function(x: Integer): Integer;
  TProc = procedure(x: Integer);
  THolder = class
    F: TFn;
    Bias: Integer;
    procedure Keep;
  end;

var
  g: TFn;
  p: TProc;

function Apply(f: TFn; v: Integer): Integer;
begin
  Apply := f(v);
end;

procedure Maker(k: Integer; var out: TFn);
var
  local: TFn;
begin
  local := function(x) => x + k;
  g := function(x) => x + k;
  out := function(x) => x + local(k);
  g := function(x) => x + 1;
  g := function(x) => Apply(function(y) => y + x, 1);
end;

procedure THolder.Keep;
begin
  F := function(x) => x + Bias;
  F := function(x) => x * 2;
  F := function(x) => x + Self.Bias;
end;

procedure Nested(k: Integer);
  function Twice(n: Integer): Integer;
  begin
    Twice := 2 * n;
  end;
begin
  g := function(x) => Twice(x);
  Writeln(Apply(function(x) => Twice(x) + k, 1));
end;

begin
  var a := function(x, y) => x + y;
  var b := function(x: Integer) => nil;
  g := function(x, y) => x;
  g := function(x) => 'text';
  g := function(x: Real) => 1;
  g := function(x): Real => x;
  p := function(x) => x;
  g := procedure(x) => p(x);
  g := function(x) => nope + x;
  Writeln(Apply(function(x) => x.Missing, 1));
  Writeln(Apply(function(s: string) => 1, 1));
  var ok := function(x: Integer) => x * 2;
  g := ok;
  g := function(x) => x - 1;
  p := procedure(x) => Writeln(x);
end.
