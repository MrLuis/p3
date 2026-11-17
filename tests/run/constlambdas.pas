{ Lambdas and routines in typed constants (objfpc): a procedural typed
  constant (plain, `reference to` or `of object`) or initialized variable
  can hold nil, @F of a routine declared at program or unit level or of a
  static method, or a lambda that uses nothing of the routines around it
  and has no object; also as elements of arrays and fields of records:
  dispatch tables.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/constlambdas.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/constlambdas.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/constlambdas.expected
}
{$H+}
program constlambdas;
type
  TFn = function(x: Integer): Integer;
  TRef = reference to function(x: Integer): Integer;
  TCalc = function(x: Integer): Integer of object;
  TOp = record
    Name: string;
    Fn: TFn;
  end;
  TMath = class
    class function Neg(x: Integer): Integer; static;
  end;

class function TMath.Neg(x: Integer): Integer;
begin
  Neg := -x;
end;

function Id(x: Integer): Integer;
begin
  Id := x;
end;

const
  Ident: TFn = @Id;
  Twice: TFn = function(x) => 2 * x;
  Square: TRef = function(x) => x * x;
  Halve: TCalc = function(x) => x div 2;
  Neg: TFn = @TMath.Neg;
  None: TFn = nil;
  Ops: array[0..2] of TOp = (
    (Name: 'inc'; Fn: function(x) => x + 1),
    (Name: 'dec'; Fn: function(x) => x - 1),
    (Name: 'id'; Fn: @Id));

var
  Scale: TFn = function(x) => x * 10;

function Run(n: Integer): Integer;
const
  Local: TFn = function(x) => x + 100;  { static, in a routine }
begin
  Run := Local(n);
end;

var
  i: Integer;
begin
  Writeln(Ident(1), ' ', Twice(2), ' ', Square(3), ' ', Halve(9), ' ', Neg(5));
  Writeln(Assigned(None));
  for i := 0 to High(Ops) do
    Writeln(Ops[i].Name, ' ', Ops[i].Fn(10));
  Writeln(Scale(4), ' ', Run(1));
end.
