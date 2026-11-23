generator SlowGenerator for Serializable;
{ never finishes: the time limit stops it }
uses p3gen;

procedure Generate(const T: TTypeModel; W: TSourceWriter);
begin
  while True do
    ;
end;

begin
  RunGenerator(@Generate);
end.
