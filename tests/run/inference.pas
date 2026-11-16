{ Types that are inferred (objfpc). A call to a generic routine without
  specialize takes its type arguments from the arguments, and from the
  type wanted of the result; const x := e is a variable set once, with
  the type of its value; quoted text is a string where a type is inferred,
  and [a, b, c] an array (a set if its elements are enumeration values,
  characters or booleans, or it has a range).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/inference.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/inference.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/inference.expected
}
{$H+}
program inference;

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

generic function Largest<T>(a, b: T): T;
begin
  if a > b then
    Result := a
  else
    Result := b;
end;

generic procedure Exchange<T>(var a, b: T);
var
  t: T;
begin
  t := a;
  a := b;
  b := t;
end;

generic function Total<T>(const Items: array of T; Start: T): T;
var
  i: Integer;
begin
  Result := Start;
  for i := 0 to High(Items) do
    Result := Result + Items[i];
end;

generic function First<T>(const Items: array of T): T;
begin
  Result := Items[0];
end;

generic function Pair<K, V>(const Key: K; const Value: V): string;
begin
  Result := `${Key}=${Value}`;
end;

generic function Zero<T>: T;
var
  none: T;   { (pcc starts variables at zero) }
begin
  Result := none;
end;

generic function Convert<T, R>(const v: T; scale: R): R;
begin
  Result := v * scale;
end;

generic procedure Show<T>(const Value: T; const Tag: string = 'value');
begin
  Writeln(Tag, ': ', Value);
end;

function Nothing: Int64;
begin
  exit(Zero());
end;

function TShape.Name: string;
begin
  Name := 'shape';
end;

function TCircle.Name: string;
begin
  Name := 'circle';
end;

var
  a, b: Integer;
  s, t: string;
  r: Real;
  p: TPoint;
  bytes: array[1..3] of Byte;
  dyn: array of Real;
  shape: TShape;
  circle: TCircle;
  small: Byte;
  wide: Int64;
begin
  { generic calls: the type arguments from the arguments }
  Writeln(Largest(3, 7));
  Writeln(Largest(2.5, 1.5):0:1);
  Writeln(Largest(1, 2.5):0:1);            { integer and real: real }
  Writeln(Largest('pear', 'apple'));
  Writeln(Largest('a', 'b'));              { quoted text: strings }
  small := 200;
  wide := 5000000000;
  Writeln(Largest(small, 100));            { byte and integer: integer }
  Writeln(Largest(wide, 1));               { int64 and integer: int64 }
  a := 1;
  b := 2;
  Exchange(a, b);
  s := 'left';
  t := 'right';
  Exchange(s, t);
  Writeln(a, ' ', b, ' ', s, ' ', t);
  Writeln(Total([1, 2, 3], 10));
  Writeln(Total([1.5, 2.5], 0):0:1);       { element and start: real }
  bytes[1] := 1; bytes[2] := 2; bytes[3] := 3;
  Writeln(First(bytes));
  SetLength(dyn, 2);
  dyn[0] := 9.5;
  Writeln(First(dyn):0:1, ' ', First(['x', 'y']));
  Writeln(Pair('n', 42), ' ', Pair(2, True), ' ', Pair(Value := 'v', Key := Red));
  p.X := 11;
  p.Y := 22;
  { ... and from the type wanted of the result }
  a := Zero();
  s := Zero();
  r := Convert(3, 1.5);
  Writeln(a, ' [', s, '] ', r:0:1);
  var z: Int64 := Zero();
  Writeln(z, ' ', Nothing);
  Show(7);
  Show('text', Tag := 'named');
  Show(Tag := 'color', Value := Ord(Blue));
  circle := TCircle.Create;
  shape := circle;
  Writeln(Largest(1, Largest(2, 3)));
  { a class and a descendant: the ancestor }
  Writeln(First([shape, circle]).Name);

  { const x := e }
  const limit := Largest(a, 10) * 2;
  const greeting: string = 'hi';
  const tau = 6.28;
  const origin := p;
  p.X := 0;
  Writeln(limit, ' ', greeting, ' ', tau:0:2, ' ', origin.X);
  for a := 1 to 2 do
  begin
    const square := a * a;
    Write(square, ' ');
  end;
  Writeln;

  { literals }
  var c := 'a';
  c := c + 'bc';
  var n := 3;
  var big := 5000000000;
  var x := 2.5;
  Writeln(c, ' ', SizeOf(n), ' ', SizeOf(big), ' ', SizeOf(x));
  var ch := t[1];                          { a char value stays a char }
  Writeln(SizeOf(ch));
  var xs := [1, 2, 3];
  var rs := [1, 2.5];
  var names := ['a', 'bc'];
  var objs := [circle, shape];
  var colors := [Red, Blue];               { sets, as ever }
  var digits := ['0'..'9'];
  var flags := [True];
  SetLength(xs, 4);
  xs[3] := 40;
  Writeln(Length(xs), ' ', xs[3], ' ', rs[1]:0:1, ' ', names[1], ' ',
          objs[0].Name, ' ', Green in colors, ' ', '5' in digits, ' ',
          True in flags);
  var pair := (1, 'x');
  pair.Item2 := pair.Item2 + 'yz';
  Writeln(pair.Item2);
  circle.Free;
end.
