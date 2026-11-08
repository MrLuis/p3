unit animals;
interface
type
  TAnimal = class
  private
    FName: string;
    FLegs: integer;
  protected
    function Sound: string; virtual;
  public
    constructor Create(const AName: string; ALegs: integer);
    procedure Speak;
    function Describe(Prefix: string; Count: integer): string; virtual;
    property Name: string read FName;
    property Legs: integer read FLegs write FLegs;
  end;
var
  Created: integer;
implementation
constructor TAnimal.Create(const AName: string; ALegs: integer);
begin
  FName := AName;
  FLegs := ALegs;
  inc(Created);
end;
function TAnimal.Sound: string;
begin
  Sound := '...';
end;
procedure TAnimal.Speak;
begin
  writeln(FName, ' says ', Sound);
end;
function TAnimal.Describe(Prefix: string; Count: integer): string;
var S: string;
  procedure Add(const T: string);
  begin
    S := S + T;
  end;
begin
  S := Prefix;
  Add(FName);
  if Count > 1 then Add('s');
  Describe := S;
end;
end.
