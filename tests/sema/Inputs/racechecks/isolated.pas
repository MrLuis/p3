program isolated;
{ an isolated variable belongs to one task, with or without racechecks }
type
  TBytes = array of Byte;
function Run: async;
var
  data: isolated TBytes;
begin
  scope
    spawn begin data := nil; end;
  end;
end;
begin
end.
