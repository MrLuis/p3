{ Capture by copy (objfpc): function[k, s](x) => e is a closure with its
  own copies of k and s, taken when it is made (read-only in it); what it
  uses without copying it shares, as closures do. Each time it is made it
  copies anew: closures made in a loop each keep the values of their
  iteration.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/copycapture.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/copycapture.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/copycapture.expected
}
{$H+}
program copycapture;
type
  TFn = reference to function: Integer;
  TIntFn = reference to function(x: Integer): Integer;
  TProc = reference to procedure;
  TFns = array of TFn;
  TStrFn = reference to function: string;

  TThing = class
    Name: string;
    function Namer: TStrFn;
  end;

{ an object's field, copied now }
function TThing.Namer: TStrFn;
begin
  Namer := function[Name]: string => Name + '?';
end;

function Call(f: TFn): Integer; overload;
begin
  Call := f();
end;

function Call(f: TStrFn): Integer; overload;
begin
  Call := Length(f());
end;

{ strings, one per iteration }
function Greeters(const names: array of string): TFns;
var
  i: Integer;
  s: string;
begin
  SetLength(Result, Length(names));
  for i := 0 to High(names) do
  begin
    s := names[i];
    Result[i] := function[s, i]: Integer => Length(s) * 100 + i;
  end;
end;

{ in a loop: each closure its own i }
function Make(n: Integer): TFns;
var
  i: Integer;
begin
  SetLength(Result, n);
  for i := 0 to n - 1 do
    Result[i] := function[i]: Integer => i * 10;
end;

{ a var parameter's value, copied }
function Snapshot(var total: Integer): TFn;
begin
  Snapshot := function[total]: Integer => total;
  total := total + 1;
end;

{ copies and sharing together }
function Mixed(const prefix: string): TProc;
var
  count: Integer;
  name: string;
begin
  count := 0;
  name := prefix + '!';
  Result := procedure[name] =>
    begin
      count := count + 1;
      Writeln(name, ' ', count);
    end;
  name := 'changed';
end;

function Adder(var base: Integer): TIntFn;
begin
  Adder := function[base](x) => x + base;
end;

var
  fs: TFns;
  i, t: Integer;
  f: TFn;
  p: TProc;
  a: TIntFn;
  th: TThing;
  sf: TStrFn;
  w: string;
begin
  fs := Make(3);
  for i := 0 to High(fs) do
    Write(fs[i](), ' ');
  Writeln;
  t := 5;
  f := Snapshot(t);
  Writeln(f(), ' ', t);
  p := Mixed('hi');
  p();
  p();
  t := 7;
  a := Adder(t);
  t := 100;
  Writeln(a(1));
  th := TThing.Create;
  th.Name := 'first';
  sf := th.Namer;
  th.Name := 'second';
  th.Free;
  Writeln(sf());
  fs := Greeters(['a', 'bbb']);
  Writeln(fs[0](), ' ', fs[1]());
  w := 'abcd';
  Writeln(Call(function[w]: string => w), ' ', Call(function[t]: Integer => t));
end.
