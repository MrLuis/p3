{ Generic functions and procedures (objfpc), as in Free Pascal 3.2.2:
  `generic function F<T>(...)` declares a template, and `specialize
  F<integer>(...)` calls the routine made for those type arguments (once
  per program). A generic coexists with non-generic overloads of its name;
  one generic can specialize another, or itself. Output is identical to
  Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/generics.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/generics.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/generics.expected
}
program generics;
{$H+}
{$modeswitch advancedrecords}

type
  TPoint = record
    x, y: longint;
  end;
  TIntArray = array of longint;

generic function IfThen<T>(val: boolean; const iftrue: T; const iffalse: T): T; inline; overload;
begin
  if val then
    result := iftrue
  else
    result := iffalse;
end;

function IfThen(val: boolean; const a, b: longint): longint; overload;
begin
  if val then
    result := a * 10
  else
    result := b * 10;
end;

generic procedure Swap<T>(var a, b: T);
var
  tmp: T;
begin
  tmp := a;
  a := b;
  b := tmp;
end;

generic function Max<A, B>(x: A; y: B): A;
begin
  if x > y then
    result := x
  else
    result := y;
end;

generic function Count<T>(const items: array of T; const what: T): longint; forward;

generic function Count<T>(const items: array of T; const what: T): longint;
var
  item: T;
begin
  result := 0;
  for item in items do
    if item = what then
      inc(result);
end;

{ a generic using another generic, and a recursive one }
generic procedure ShowSorted<T>(a, b: T);
begin
  if a > b then
    specialize Swap<T>(a, b);
  write(a, ' ', b, ' ');
end;

generic function Power<T>(x: T; n: longint): T;
begin
  if n = 0 then
    result := 1
  else
    result := x * specialize Power<T>(x, n - 1);
end;

generic function First<T>(const a: array of T): T;
begin
  result := a[0];
end;

var
  i, j: longint;
  s, t: string;
  p, q: TPoint;
  d: double;
  a: TIntArray;
begin
  writeln(specialize IfThen<longint>(true, 1, 2), ' ',
          specialize IfThen<string>(false, 'yes', 'no'), ' ',
          IfThen(true, 1, 2));
  i := 1;
  j := 2;
  specialize Swap<longint>(i, j);
  writeln(i, j);
  s := 'a';
  t := 'b';
  specialize Swap<string>(s, t);
  writeln(s, t);
  p.x := 1;
  q.x := 2;
  specialize Swap<TPoint>(p, q);
  writeln(p.x, q.x);
  writeln(specialize Max<longint, byte>(3, 7), ' ', specialize Max<char, char>('a', 'z'));
  writeln(specialize Count<longint>([1, 2, 1, 3, 1], 1), ' ',
          specialize Count<string>(['x', 'y'], 'y'));
  specialize ShowSorted<longint>(5, 2);
  specialize ShowSorted<string>('b', 'a');
  writeln;
  d := specialize Power<double>(1.5, 2);
  writeln(specialize Power<longint>(2, 10), ' ', d:0:2);
  a := [4, 5, 6];
  writeln(specialize First<longint>(a), ' ', specialize IfThen<boolean>(i > j, true, false));
end.
