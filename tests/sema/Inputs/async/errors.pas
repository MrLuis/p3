program errors;
{$modeswitch advancedrecords}
type
  TThing = class
    function Load: async Integer; virtual;
    constructor Make: async;
  end;
  TRec = record
    X: Integer;
    function Get: async Integer;
  end;

function Work(x: Integer): async Integer;
begin
  Result := x;
end;

procedure Tick: async;
begin
end;

function ByRef(var x: Integer; const a: array of Integer): async;
begin
end;

function Ext: async Integer; cdecl; external name 'ext';

function Files: async Text;
begin
end;

procedure Outer;
  function Inner: async;
  begin
  end;
begin
end;

function TThing.Load: async Integer;
begin
  Result := 1;
end;

constructor TThing.Make: async;
begin
end;

function TRec.Get: async Integer;
begin
  Result := X;
end;

procedure Plain;
var
  n: Integer;
begin
  n := await Work(1);
  spawn Work(2);
  scope Work(1)
  end;
  Pool.Run(n);
  Pool.Run(Work(4));
end;

function UseAll: async;
var
  n: Integer;
  ts: array of async;
begin
  n := await n;
  n := await Tick;
  Work(3);
  scope
    spawn Plain;
  end;
  detached n;
  detached(n) Tick;
  n := Wait(n);
  n := Wait(Tick);
  n := WhenAll(n);
  n := Wait(WhenAny(ts));
end;

begin
end.
