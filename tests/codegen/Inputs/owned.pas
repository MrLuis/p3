program owned;
type
  TNode = class
  end;

function Make: TNode;
var
  first, second: TNode owned;
begin
  first := TNode.Create;
  second := TNode.Create;
  Make := move second;
end;

begin
  Make.Free;
end.
