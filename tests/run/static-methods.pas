{ Static class methods (objfpc), as in Free Pascal: `class procedure P;
  static;` has no Self; it is called through the class, a descendant, an
  object or a class reference, or unqualified from the class's methods,
  uses class variables and other static methods, and @TFoo.P is an
  ordinary procedural value. Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/static-methods.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/static-methods.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/static-methods.expected
}
program staticmethods;
{$H+}
uses sysutils;

type
  TFn = function(n: integer): integer;

  TBase = class
    class var Count: integer;
    class procedure ThrowOuterException(aException: Exception); static;
    class function Twice(n: integer): integer; static;
    class function Name: string; virtual;
    class procedure Show; static;
    procedure Run;
  end;
  TDerived = class(TBase)
    class function Name: string; override;
  end;
  TBaseClass = class of TBase;

class procedure TBase.ThrowOuterException(aException: Exception);
begin
  inc(Count);
  raise aException;
end;

class function TBase.Twice(n: integer): integer;
begin
  result := n * 2 + Count;
end;

class function TBase.Name: string;
begin
  result := 'base';
end;

class function TDerived.Name: string;
begin
  result := 'derived';
end;

class procedure TBase.Show;
begin
  writeln('show ', Count, ' ', Twice(1));
end;

procedure TBase.Run;
begin
  writeln('run ', Name, ' ', Twice(10));
  Show;
end;

function Apply(f: TFn; n: integer): integer;
begin
  result := f(n);
end;

var
  p: TFn;
  b: TBase;
  c: TBaseClass;
begin
  try
    TBase.ThrowOuterException(Exception.Create('outer'));
  except
    on e: Exception do
      writeln('caught ', e.Message);
  end;
  writeln(TBase.Twice(5), ' ', TDerived.Twice(1));
  b := TDerived.Create;
  writeln(b.Twice(2));
  b.Run;
  TDerived.Show;
  p := @TBase.Twice;
  writeln(p(10), ' ', Apply(@TBase.Twice, 4));
  c := TDerived;
  writeln(c.Twice(3));
  b.Free;
end.
