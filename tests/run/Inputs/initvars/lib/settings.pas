unit settings;

interface

var
  Verbose: boolean = true;
  Level: integer = 3;
  Title: string = 'settings';

function NextId: integer;

implementation

var
  LastId: integer = 100;

function NextId: integer;
var
  step: integer = 1;
begin
  LastId := LastId + step;
  step := step + 10;
  NextId := LastId;
end;

end.
