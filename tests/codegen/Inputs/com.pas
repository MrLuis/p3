program com;
type
  IA = interface
    function F: integer;
  end;
  TA = class(TInterfacedObject, IA)
    Other: IA;
    function F: integer;
  end;

function TA.F: integer;
begin
  F := 1;
end;

function Make: IA;
begin
  Make := TA.Create;
end;

procedure Use(a: IA; const b: IA);
var
  l: IA;
begin
  l := a;
  writeln(Make.F);
end;

var
  g: IA;
begin
  g := TA.Create;
  Use(g, g);
end.
