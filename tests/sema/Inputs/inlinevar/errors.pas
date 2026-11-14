program errors;
type
  TNode = class
    procedure Show;
  end;

procedure TNode.Show;
begin
end;

procedure Take(n: TNode);
begin
end;

procedure Scopes(c: boolean);
begin
  if c then
    var alone := 1;
  var x := 1;
  var x := 2;
  begin
    var inner := x;
    var x := 'shadow';
  end;
  inner := 3;
  var self1 := self1 + 1;
  var n := nil;
  var e := [];
  var t: integer := 'text';
  var r: real := 1;
  r := 'no';
  for var i := 1.5 to 3 do ;
  for var j := 1 to 3 do ;
  j := 0;
  for var k in 5 do ;
  for var m in [] do ;
  for var z: boolean in [1, 2] do ;
end;

procedure Owned(c: boolean);
begin
  var p: TNode owned := TNode.Create;
  var q: integer owned := 1;
  Take(move p);
  p.Show;
  while c do
  begin
    var w: TNode owned := TNode.Create;
    Take(move w);
  end;
  var keep: TNode owned;
  if c then
  begin
    var tmp: TNode owned := TNode.Create;
    keep := tmp;
    keep := move tmp;
    tmp.Show;
  end;
end;

begin
  var g: TNode owned := TNode.Create;
  Take(move g);
  g.Show;
end.
