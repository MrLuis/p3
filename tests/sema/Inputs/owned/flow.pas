program flow;
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

{ no errors: the variable has an object again wherever it is used }
procedure Fine(c: boolean; n: integer);
var
  p, q: TNode owned;
  i: integer;
begin
  p := TNode.Create;
  if c then
    Take(move p)
  else
    p.Show;
  p := TNode.Create;
  p.Show;
  for i := 1 to n do
  begin
    q := TNode.Create;
    if i = 2 then
      continue;
    Take(move q);
  end;
  while n > 0 do
  begin
    Take(move p);
    p := TNode.Create;
    n := n - 1;
  end;
  p.Show;
  repeat
    Take(move p);
    if n = 0 then
      break;
    p := TNode.Create;
  until n > 3;
  p := nil;
  try
    q := TNode.Create;
    Take(move q);
    q := TNode.Create;
  finally
    writeln('done');
  end;
  q.Show;
  case n of
    1: Take(move q);
    2: exit;
  else
    Take(move q);
  end;
  if c then
    raise TNode.Create;
  q := TNode.Create;
  Take(q);
end;

{ errors }
procedure Bad(c: boolean; n: integer);
var
  p, q, r, s: TNode owned;
  i, j: integer;
begin
  p := TNode.Create;
  q := TNode.Create;
  r := TNode.Create;
  s := TNode.Create;
  for i := 1 to n do
  begin
    for j := 1 to n do
      p.Show;
    Take(move p);
  end;
  while n > 0 do
  begin
    if c then
      continue;
    Take(move q);
    q := TNode.Create;
    if n = 5 then
    begin
      Take(move q);
      continue;
    end;
  end;
  try
    try
      Take(move r);
    finally
      r.Show;
    end;
    r := TNode.Create;
  except
    on e: TNode do
      r.Show;
  end;
  Take(move s);
  Take(move s);
  Take((s));
end;

begin
end.
