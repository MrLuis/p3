{$H+}
program inlinevar;
type
  TNode = class
  end;

procedure Run(n: integer);
begin
  var count := n;
  var name: string;
  var zero: int64;
  while count > 0 do
  begin
    var node: TNode owned := TNode.Create;
    count := count - 1;
  end;
  name := 'x';
  zero := count;
end;

begin
  Run(2);
end.
