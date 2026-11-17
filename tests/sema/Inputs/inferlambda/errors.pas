program l2;
type
  generic TFunc<T, R> = reference to function(const X: T): R;
  generic TProc<T> = reference to procedure(const X: T);

generic function Apply<T, R>(F: specialize TFunc<T, R>; const X: T): R;
begin
  Result := F(X);
end;

generic function Make<T, R>(F: specialize TFunc<T, R>): R;
begin
end;

generic procedure Each<T>(const Xs: array of T; F: specialize TProc<T>);
var
  X: T;
begin
  for X in Xs do
    F(X);
end;

var
  i: Integer;
begin
  i := Apply(function(x) => x.Missing, 3);
  i := Apply(function(x) => nil, 3);
  i := Make(function(x) => 1);
  i := Apply(function(x, y) => x, 3);
  i := Apply(function(const x: string): Integer => Length(x), 3);
  i := Apply(procedure(x) => Writeln(x), 3);
  Each([1, 2], procedure(x) => Writeln(x * 2));
  Each([1, 2], procedure(const x: string) => Writeln(x));
end.
