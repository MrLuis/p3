program ansistringrefs;
{$H+}
var
  s, t: string;

function F(const x: string): string;
begin
  F := x;
end;

begin
  s := 'a';       { a static constant: no count update }
  t := s;         { another reference: counted }
  s := s + t;     { s is unique: append in place }
  s := s + 'b' + 'c'; { a chain: append each piece in order }
  s := F(t);      { the fresh result moves into s }
  t := s;         { another reference: counted }
end.
