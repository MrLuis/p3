{ Copying everything used (objfpc): function[=](x) => e copies, when it is
  made, every variable and parameter of the routines around it that its
  body names; what else it uses (globals, the object's members through
  Self) it shares.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/copyall.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/copyall.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/copyall.expected
}
{$H+}
program copyall;
type
  TFn = reference to function: Integer;
  TIntFn = reference to function(x: Integer): Integer;
  TProc = reference to procedure;
  TFns = array of TFn;
  TPoint = record
    X, Y: Integer;
  end;
  TThing = class
    Name: string;
    function Show(const suffix: string): TProc;
  end;

var
  global: Integer;

function TThing.Show(const suffix: string): TProc;
begin
  { suffix copied; Name through Self (shared: the object) }
  Show := procedure[=] => Writeln(Name, suffix);
end;

function Make(n: Integer): TFns;
var
  i, scale: Integer;
begin
  scale := 3;
  SetLength(Result, n);
  for i := 0 to n - 1 do
    Result[i] := function[=]: Integer => i * scale + global;
  scale := 100;
end;

function Point(var p: TPoint; const label_: string): TFn;
begin
  Point := function[=]: Integer => p.X * 10 + p.Y + Length(label_);
  p.X := 9;
end;

function Apply(f: TIntFn; v: Integer): Integer;
begin
  Apply := f(v);
end;

function Nested(base: Integer): TIntFn;
begin
  { a [=] lambda in a [=] lambda: base copied twice }
  Nested := function[=](x) =>
    Apply(function[=](y: Integer): Integer => y + base + x, 1);
end;

function Shadow(x: Integer): TIntFn;
begin
  Shadow := function[=](x) => x * 2;  { its own x: nothing copied }
end;

var
  fs: TFns;
  i: Integer;
  t: TThing;
  p: TProc;
  pt: TPoint;
  f: TFn;
  g: TIntFn;
begin
  global := 1000;
  fs := Make(3);
  global := 2000; { a global: shared }
  for i := 0 to High(fs) do
    Write(fs[i](), ' ');
  Writeln;
  t := TThing.Create;
  t.Name := 'thing';
  p := t.Show('!');
  t.Name := 'renamed';
  p();
  t.Free;
  pt.X := 1;
  pt.Y := 2;
  f := Point(pt, 'ab');
  Writeln(f(), ' ', pt.X);
  g := Nested(10);
  Writeln(g(5));
  Writeln(Shadow(1)(21));
end.
