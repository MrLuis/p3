{$H+}
{$modeswitch advancedrecords}
program errors;
type
  TCalc = function(x: Integer): Integer of object;
  TPlain = function(x: Integer): Integer;
  TRec = record
    a: Integer;
    procedure Run;
  end;
  TThing = class
    F: TCalc;
    procedure Run(k: Integer);
    class procedure Make;
  end;

procedure TRec.Run;
var
  f: TCalc;
begin
  f := function(x) => x + a;
end;

procedure TThing.Run(k: Integer);
var
  local: Integer;
  p: TPlain;
begin
  local := 1;
  F := function(x) => x + k;
  F := function(x) => x + local;
  F := function(x, y) => x;
  p := function(x) => x;
  F := p;
end;

class procedure TThing.Make;
var
  f: TCalc;
begin
  f := function(x) => x;
end;

var
  g: TCalc;
  n: Integer;
begin
  g := function(x) => x + n;
  g := function(x) => Self;
end.
