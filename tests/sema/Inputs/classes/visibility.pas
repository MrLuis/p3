program visibility;
uses animals;
type
  TCat = class(TAnimal)
    function Sound: string; override;
  end;
function TCat.Sound: string;
begin
  Result := FName + inherited Sound;
end;
var C: TCat; A: TAnimal;
begin
  C := TCat.Create('c', 4);
  writeln(C.Sound, A.Sound, C.FLegs);
end.
