unit team;
{ The records of data/team.json, read and written by the code the library's
  json generator writes for the types marked [Json]. }
{$H+}
{$modeswitch advancedrecords}
interface

uses json;

type
  [Json]
  TLevel = (Junior, Senior, Lead);

  { (the keys are the fields' names as spelled here: code can still write
    M.Name, Pascal does not mind the case) }
  [Json]
  TMember = partial record
    name: string;
    level: TLevel;
    [JsonName('started')] Year: Integer;
    skills: array of string;
    [Skip] Selected: Boolean;
  end;

  [Json]
  TTeam = partial record
    team: string;
    budget: Double;
    members: array of TMember;
  end;

implementation

end.
