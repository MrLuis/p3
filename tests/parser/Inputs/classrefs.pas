program classrefs;
type
  TA = class;
  TAClass = class of TA;
  TA = class
    class var Count, Total: integer;
    var Name: string;
    class function Make: TA; virtual;
    constructor Create; virtual;
  end;
class function TA.Make: TA;
begin
  Make := Create;
end;
constructor TA.Create;
begin
  Count := Count + 1;
end;
var
  c: TAClass;
  a: TA;
begin
  c := TA;
  a := c.Create;
  a := TA.Make;
  writeln(TA.Count, a.Total, c.ClassName);
end.
