{ Function lambdas with a block body (objfpc, p3's own): function(x) =>
  begin ... end sets Result, the lambda's own (or exits with exit(value)).
  The result type is written, or the procedural type's, or else the type
  of the first value given to Result (or to exit).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/lambdablocks.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/lambdablocks.expected
}
{$H+}
program lambdablocks;
uses SysUtils;

type
  TIntFn = function(x: Integer): Integer;
  TStrFn = reference to function(const s: string): string;
  generic TFunc<T, R> = reference to function(const X: T): R;

function Apply(f: TIntFn; v: Integer): Integer;
begin
  Result := f(v);
end;

generic function Map<T, R>(const Xs: array of T; F: specialize TFunc<T, R>): string;
var
  X: T;
begin
  Result := '';
  for X in Xs do
    Result := Result + F(X) + ' ';
end;

function Outer: Integer;
var
  k: Integer;
begin
  k := 10;
  { the lambda's Result is its own; Outer's is untouched by it }
  Result := Apply(function(x) => begin
                    Result := 0;
                    for var i := 1 to x do
                      Result := Result + i * k;
                  end, 4);
end;

var
  f: TStrFn;
  g: TIntFn;
begin
  Writeln(Outer);
  { the type from the procedural type wanted }
  f := function(s) => begin
         if s = '' then
           Result := '(empty)'
         else
           Result := UpperCase(s);
       end;
  Writeln(f('abc'), ' ', f(''));
  { the type written }
  var h := function(n: Integer): Real => begin
             Result := n / 4;
           end;
  Writeln(h(3):0:2);
  { the type from the first Result := (inference through lambdas: R) }
  Writeln(specialize Map<Integer, string>([1, 2], function(x) => begin Result := '#' + IntToStr(x); end));
  Writeln(Map([3, 4, 5], function(x) => begin
                             if Odd(x) then
                               Result := 'odd'
                             else
                               Result := 'even';
                           end));
  { exit(value) }
  g := function(x) => begin
         if x < 0 then
           exit(0);
         Result := x * 2;
       end;
  Writeln(g(-5), ' ', g(5));
  { the type from exit(value), and a closure }
  var limit := 3;
  var clamp := function(n: Integer) => begin
                 if n > limit then
                   exit(limit);
                 Result := n;
               end;
  Writeln(clamp(1), ' ', clamp(7));
end.
