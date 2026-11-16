{ Overloads chosen by a lambda's shape (objfpc): a lambda argument fits a
  procedural parameter of the same kind (function or procedure), with as
  many parameters, of the types and kinds it writes and the result type it
  writes; a parameter written without a kind fits a value parameter best.
  The overload chosen then gives the lambda its types.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/lambdaoverloads.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/lambdaoverloads.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/lambdaoverloads.expected
}
{$H+}
program lambdaoverloads;
type
  TUnary = function(x: Integer): Integer;
  TBinary = function(a, b: Integer): Integer;
  TStrFn = function(const s: string): string;
  TRealFn = function(x: Real): Real;
  TProc = procedure(x: Integer);
  TSwap = procedure(var a, b: Integer);
  TPair = procedure(a, b: Integer);
  TRef = reference to function(x: Integer): Integer;
  TCalc = function(x: Integer): Integer of object;

  TList = class
    procedure Each(f: TProc); overload;
    procedure Each(f: TPair); overload;
  end;

function Apply(f: TUnary; v: Integer): Integer; overload;
begin
  Apply := f(v);
end;

function Apply(f: TBinary; v: Integer): Integer; overload;
begin
  Apply := f(v, v + 1);
end;

function Apply(f: TStrFn; const v: string): string; overload;
begin
  Apply := f(v);
end;

function Calc(f: TUnary): Integer; overload;
begin
  Calc := f(2);
end;

function Calc(f: TRealFn): Integer; overload;
begin
  Calc := Round(f(2.5));
end;

procedure Run(f: TProc); overload;
begin
  f(1);
end;

procedure Run(f: TUnary); overload;
begin
  Writeln('unary ', f(1));
end;

procedure Swap2(f: TSwap); overload;
var
  a, b: Integer;
begin
  a := 1;
  b := 2;
  f(a, b);
  Writeln(a, b);
end;

procedure Swap2(f: TPair); overload;
begin
  f(5, 6);
end;

function Make(f: TRef): Integer; overload;
begin
  Make := f(1);
end;

function Make(f: TStrFn): Integer; overload;
begin
  Make := Length(f('abc'));
end;

procedure TList.Each(f: TProc);
begin
  f(10);
end;

procedure TList.Each(f: TPair);
begin
  f(10, 20);
end;

var
  k: Integer;
  l: TList;
begin
  k := 3;
  Writeln(Apply(function(x) => x * k, 5));
  Writeln(Apply(function(a, b) => a * b, 5));
  Writeln(Apply(function(s) => s + '!', 'hi'));
  Writeln(Calc(function(x: Integer) => x * 10));
  Writeln(Calc(function(x: Real) => x * 10));
  Writeln(Calc(function(x): Real => x * 2));
  Run(procedure(x) => Writeln('proc ', x));
  Run(function(x) => x + 41);
  Swap2(procedure(var a, b) => begin k := a; a := b; b := k end);
  Swap2(procedure(a, b: Integer) => Writeln(a + b));
  l := TList.Create;
  l.Each(procedure(x) => Writeln('each ', x));
  l.Each(procedure(x, y) => Writeln('each ', x, ' ', y));
  l.Free;
  Writeln(Make(function(x: Integer) => x + k));
  Writeln(Make(function(const s: string) => s + s));
end.
