{$H+}
program errors;
type
  TUnary = function(x: Integer): Integer;
  TStrFn = function(const s: string): string;
  TRealFn = function(x: Real): Real;
  TProc = procedure(x: Integer);
  TSwap = procedure(var a, b: Integer);
  TPair = procedure(a, b: Integer);

function Calc(f: TUnary): Integer; overload;
begin
  Calc := f(2);
end;

function Calc(f: TRealFn): Integer; overload;
begin
  Calc := Round(f(2.5));
end;

function Calc(f: TStrFn): Integer; overload;
begin
  Calc := Length(f('a'));
end;

procedure Run(f: TProc); overload;
begin
  f(1);
end;

procedure Run(f: TUnary); overload;
begin
  Writeln(f(1));
end;

procedure Swap2(f: TSwap); overload;
var
  a, b: Integer;
begin
  f(a, b);
end;

procedure Swap2(f: TPair); overload;
begin
  f(1, 2);
end;

begin
  Calc(function(x) => x);
  Calc(function(x: Boolean) => 1);
  Calc(function(a, b) => a);
  Run(procedure(var x) => Writeln(x));
  Calc(function(x: Nope) => 1);
  Calc(function(x: Integer): string => 'a');
  Swap2(procedure(a, b) => Writeln(a));
end.
