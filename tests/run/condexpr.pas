{ Conditional expressions (objfpc): if c then a else b is a value, a where
  c holds and b otherwise; only the branch chosen is evaluated. Its type is
  the type wanted of it, or the common type of the branches.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/condexpr.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/condexpr.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/condexpr.expected
}
{$H+}
program condexpr;

type
  TColor = (Red, Green, Blue);
  TPoint = record
    X, Y: Integer;
  end;
  TShape = class
    function Name: string; virtual;
  end;
  TCircle = class(TShape)
    function Name: string; override;
  end;
  TShort = string[10];

var
  calls: Integer;

function TShape.Name: string;
begin
  Name := 'shape';
end;

function TCircle.Name: string;
begin
  Name := 'circle';
end;

function Count(n: Integer): Integer;
begin
  calls := calls + 1;
  Count := n;
end;

function Sign(n: Integer): Integer;
begin
  Sign := if n < 0 then -1 else if n > 0 then 1 else 0;
end;

function Describe(n: Integer): string;
begin
  exit(if n = 1 then 'one' else `${n} things`);
end;

function Pick(first: Boolean; const a, b: TPoint): TPoint;
begin
  Pick := if first then a else b;
end;

function Fact(n: Int64): Int64;
begin
  Fact := if n <= 1 then 1 else n * Fact(n - 1);
end;

procedure Show(const s: string; n: Integer = 0);
begin
  Writeln(s, ' ', n);
end;

var
  i, n: Integer;
  r: Real;
  s: string;
  short: TShort;
  c: Char;
  b: Boolean;
  p, q, o: TPoint;
  shape: TShape;
  circle: TCircle;
  color: TColor;
  ptr: ^Integer;
  bits: set of Byte;
  list: array of Integer;
begin
  n := 5;
  i := if n > 3 then 10 else 20;
  Writeln(i, ' ', if n < 3 then 10 else 20);
  { the common type: an integer and a real }
  r := if n > 3 then 1 else 2.5;
  var x := if n > 3 then 1 else 2.5;
  Writeln(r:0:1, ' ', x:0:1, ' ', SizeOf(x));
  { strings, of either kind, and characters }
  s := if n = 5 then 'five' else 'other';
  short := if n = 5 then 'x' else 'yz';
  c := if n = 5 then 'a' else 'b';
  Writeln(s, ' ', short, ' ', c, ' ', if n <> 5 then s else s + '!');
  var t := if n = 4 then 'four' else 'not four';
  t := t + '.';
  Writeln(t);
  { only the branch chosen runs }
  calls := 0;
  i := if n > 0 then Count(1) else Count(2);
  i := i + (if n < 0 then Count(3) else 4);
  Writeln(i, ' ', calls);
  n := 0;
  Writeln(if n = 0 then 0 else 100 div n);
  { nested, in conditions, arguments and other expressions }
  Writeln(Sign(-7), Sign(0), Sign(7), ' ', Describe(1), ' ', Describe(3));
  if (if n = 0 then True else False) then
    Writeln('condition');
  Show(if n = 0 then 'zero' else 'nonzero', n := if n = 0 then 1 else 2);
  Writeln(2 * (if n = 0 then 3 else 4) + 1);
  b := if n = 0 then n < 1 else n > 1;
  Writeln(b);
  for i := 1 to 3 do
    Write(if odd(i) then 'odd' else 'even', ' ');
  Writeln;
  { records, objects, enumerations, pointers, sets, arrays }
  p.X := 1; p.Y := 2; q.X := 3; q.Y := 4;
  o := Pick(False, p, q);
  Writeln(o.X, ' ', (if n = 0 then p else q).Y);
  circle := TCircle.Create;
  shape := TShape.Create;
  Writeln((if n = 0 then circle else shape).Name, ' ',
          (if n = 1 then circle else shape).Name);
  color := if n = 0 then Blue else Red;
  ptr := if n = 0 then nil else @n;
  bits := if n = 0 then [1, 2] else [];
  list := if n = 0 then [7, 8, 9] else nil;
  Writeln(Ord(color), ' ', ptr = nil, ' ', 2 in bits, ' ', Length(list));
  var (lo, hi) := if p.X < q.X then (p.X, q.X) else (q.X, p.X);
  Writeln(lo, ' ', hi, ' ', Fact(10));
  { an lvalue it is not, but its parts can be read }
  Writeln(Length(if n = 0 then 'abc' else 'de'), ' ',
          (if n = 0 then s else t)[1]);
  shape.Free;
  circle.Free;
end.
