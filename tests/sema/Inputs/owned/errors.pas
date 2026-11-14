program errors;
type
  TNode = class
    next: TNode;
    procedure Show;
  end;
  THolder = class
    item: TNode;
  end;
var
  g: TNode owned;
  keep: TNode;

procedure TNode.Show;
begin
end;

procedure Take(n: TNode);
begin
end;

procedure Change(var n: TNode);
begin
end;

function AfterMove: TNode;
var
  p: TNode owned;
begin
  p := TNode.Create;
  AfterMove := move p;
  p.Show;
  p := TNode.Create;
  p.Show;
end;

procedure Branches(c: boolean; k: integer);
var
  p, q: TNode owned;
  r: TNode owned;
begin
  p := TNode.Create;
  q := TNode.Create;
  r := TNode.Create;
  if c then
    Take(move p);
  p.Show;
  case k of
    1: Take(move q);
    2: q := nil;
  end;
  Take(q);
  if c then
    Take(move r)
  else
    Take(move r);
  r.Show;
end;

procedure Loops(n: integer);
var
  p, q, r: TNode owned;
  i: integer;
begin
  p := TNode.Create;
  for i := 1 to n do
    Take(move p);
  while n > 0 do
  begin
    q := TNode.Create;
    Take(move q);
    n := n - 1;
  end;
  q.Show;
  r := TNode.Create;
  repeat
    r.Show;
    if n = 3 then
      Take(move r);
    n := n + 1;
  until n > 5;
end;

procedure Tries;
var
  p, q: TNode owned;
begin
  p := TNode.Create;
  q := TNode.Create;
  try
    Take(move p);
    p := TNode.Create;
  except
    p.Show;
  end;
  p.Show;
  try
    Take(move q);
  finally
    q.Show;
  end;
end;

function Escapes(h: THolder): TNode;
var
  p: TNode owned;
  b: TNode;
  i: integer owned;
  a: TNode owned = nil;
begin
  p := TNode.Create;
  b := p;
  h.item := p;
  keep := p;
  Escapes := p;
  exit(p);
  Change(p);
  p.Free;
  p.Destroy;
  b := @p;
  Take(move b);
  Take(move keep);
end;

procedure Outer;
var
  p: TNode owned;
  procedure Inner;
  begin
    p.Show;
    Take(move p);
  end;
begin
  p := TNode.Create;
  Inner;
end;

begin
end.
