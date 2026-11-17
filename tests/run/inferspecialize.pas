{ Inference through specializations (objfpc): a parameter written
  specialize TGen<..., T, ...> gives T the type argument of the
  specialization its argument is, nested ones and arrays of T too.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/inferspecialize.expected
}
{$H+}
{$modeswitch advancedrecords}
program inferspecialize;

type
  generic TBox<T> = record
    Value: T;
  end;
  generic TPair<A, B> = record
    First: A;
    Second: B;
  end;
  generic TArr<T> = array of T;

generic function Box<T>(const X: T): specialize TBox<T>;
begin
  Result.Value := X;
end;

generic function Unbox<T>(const B: specialize TBox<T>): T;
begin
  Result := B.Value;
end;

generic function Pair<A, B>(const X: specialize TBox<A>; const Y: specialize TBox<B>): specialize TPair<A, B>;
begin
  Result.First := X.Value;
  Result.Second := Y.Value;
end;

generic function Deep<T>(const X: specialize TBox<specialize TBox<T>>): T;
begin
  Result := X.Value.Value;
end;

generic function Count<T>(const X: specialize TBox<specialize TArr<T>>): Integer;
begin
  Result := Length(X.Value);
end;

generic function Same<T>(const X, Y: specialize TBox<T>): Boolean;
begin
  Result := X.Value = Y.Value;
end;

var
  bi: specialize TBox<Integer>;
  bs: specialize TBox<string>;
  bb: specialize TBox<specialize TBox<Char>>;
  ba: specialize TBox<specialize TArr<Real>>;
  p: specialize TPair<Integer, string>;
begin
  bi := Box(42);
  bs := Box('hi');
  Writeln(Unbox(bi), ' ', Unbox(bs));
  p := Pair(bi, bs);
  Writeln(p.First, ' ', p.Second);
  bb.Value.Value := 'z';
  Writeln(Deep(bb));
  ba.Value := [1.5, 2.5];
  Writeln(Count(ba));
  Writeln(Same(bi, Box(42)));
end.
