{ constref parameters (tp and objfpc), as in Free Pascal: read-only like
  const, and always passed by reference: a variable of the parameter's type
  is passed itself (its address is the parameter's, and a change to it shows
  through the parameter), any other argument (a constant, an expression, a
  value of another type) through a temporary. Integers, reals, chars,
  booleans, enumerations, sets, pointers, objects, ansistrings, short
  strings, records and arrays; routines, methods, procedural variables,
  overloads, untyped constref, and a cdecl external routine that gets the
  pointer. Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/constref.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/constref.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/constref.expected
}
program constreftest;
{$H+}

type
  TColor = (Red, Green, Blue);
  TColors = set of TColor;
  TRec = record
    a, b: longint;
  end;
  TArr = array[1..3] of longint;
  TProc = procedure(constref x: longint);
  TCounter = class
    n: longint;
    procedure Add(constref by: longint);
    function Same(constref x: longint): boolean;
  end;
  TCharPair = packed record
    c, nul: char;
  end;

var
  g: longint;
  d: double;
  ch: char;
  col: TColor;
  cs: TColors;
  r: TRec;
  arr: TArr;
  s: ansistring;
  ss: shortstring;
  p: ^longint;
  obj: TCounter;
  pr: TProc;
  w: word;
  pair: TCharPair;
  pr2: ^TRec;

function strlen(constref c: char): int64; cdecl; external 'c';

procedure Show(constref x: longint);
begin
  writeln('  ', x, ' same=', @x = @g);
end;

function Twice(constref x: longint): longint;
begin
  Twice := x * 2;
end;

procedure Alias(constref x: longint);
begin
  g := 99; { x is g }
  writeln('  alias ', x);
end;

procedure Many(constref a: double; constref b: char; constref c: TColor;
               constref e: TColors; constref q: pointer);
begin
  writeln('  ', a:0:2, ' ', b, ' ', Ord(c), ' ', Green in e, ' ', q = @g);
end;

procedure Struct(constref x: TRec; constref y: TArr);
begin
  writeln('  ', x.a + x.b, ' ', y[1] + y[3], ' ', @x = @r);
end;

procedure Strings(constref a: ansistring; constref b: shortstring);
begin
  writeln('  ', a, ' ', length(a), ' ', b, ' ', length(b), ' ', @a = @s);
end;

procedure SameAs(constref x: longint; q: pointer);
begin
  write(' ', @x = q);
end;

procedure Untyped(constref x);
begin
  writeln('  ', longint(x), ' ', @x = @g);
end;

procedure Over(constref x: longint); overload;
begin
  writeln('  constref longint ', x);
end;

procedure Over(const x: ansistring); overload;
begin
  writeln('  const ansistring ', x);
end;

procedure TCounter.Add(constref by: longint);
begin
  n := n + by;
end;

function TCounter.Same(constref x: longint): boolean;
begin
  Same := @x = @n;
end;

begin
  writeln('-- integers');
  g := 5;
  Show(g);
  Show(7);
  Show(g + 1);
  w := 300;
  Show(w); { another type: a temporary }
  writeln('  ', Twice(21), ' ', Twice(g));
  Alias(g);

  writeln('-- other types');
  d := 2.5;
  ch := 'z';
  col := Blue;
  cs := [Green];
  p := @g;
  Many(d, ch, col, cs, p);
  Many(1, 'a', Red, [], nil);
  r.a := 1;
  r.b := 2;
  arr[1] := 10;
  arr[3] := 30;
  Struct(r, arr);
  s := 'abc';
  ss := 'short';
  Strings(s, ss);
  Strings('lit' + s, 'x');
  g := 42;
  Untyped(g);

  writeln('-- calls');
  obj := TCounter.Create;
  obj.Add(3);
  g := 4;
  obj.Add(g);
  writeln('  ', obj.n, ' ', obj.Same(obj.n), ' ', obj.Same(g));
  obj.Free;
  pr := @Show;
  g := 8;
  pr(g);
  pr(9);
  Over(3);
  Over('three');

  writeln('-- fields and elements');
  SameAs(r.b, @r.b);
  SameAs(arr[3], @arr[3]);
  new(pr2);
  SameAs(pr2^.a, @pr2^.a);
  with r do
    SameAs(a, @r.a);
  obj := TCounter.Create;
  SameAs(obj.n, @obj.n);
  obj.Free;
  dispose(pr2);
  writeln;

  writeln('-- cdecl');
  pair.c := 'A';
  pair.nul := #0;
  writeln('  ', strlen(pair.c));
end.
