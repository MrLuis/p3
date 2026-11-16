program ast;
type
  TCalc = function(x: Integer): Integer of object;
  TThing = class
    K: Integer;
    F: TCalc;
    procedure Init;
  end;

procedure TThing.Init;
begin
  F := function(x) => x + K;
end;

var
  t: TThing;
begin
  t := TThing.Create;
  t.Init;
  t.Free;
end.
