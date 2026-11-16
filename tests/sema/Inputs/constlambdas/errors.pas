program errors;
type
  TFn = function(x: Integer): Integer;
  TCalc = function(x: Integer): Integer of object;
  TThing = class
    K: Integer;
    procedure Run;
  end;

procedure TThing.Run;
const
  C: TCalc = function(x) => x + K;
begin
end;

procedure Outer(k: Integer);
const
  C: TFn = function(x) => x + k;
var
  V: TFn = function(x) => x * k;
  function Inner(x: Integer): Integer;
  begin
    Inner := x;
  end;
const
  D: TFn = @Inner;
begin
end;

const
  E: TFn = 1;
  F: array[0..0] of TFn = (2);
  G: TFn = function(a, b) => a;
  H: TCalc = function(x: string) => 1;

begin
end.
