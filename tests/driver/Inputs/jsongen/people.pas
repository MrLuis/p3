unit people;
{$H+}
{$modeswitch advancedrecords}
interface

uses json;

type
  [Json]
  TRole = (Admin, Editor, Viewer);

  [Json]
  TAddress = partial record
    City: string;
    [JsonName('zip_code')] Zip: string;
  end;

  [Json]
  TPerson = partial record
    Id: Int64;
    Name: string;
    Score: Double;
    Active: Boolean;
    Big: QWord;
    Role: TRole;
    Home: TAddress;
    Tags: array of string;
    Matrix: array of array of Integer;
    Others: array of TAddress;
    [Skip] Cache: Pointer;
  end;

  [Json]
  TTeam = partial class
  public
    Title: string;
    Lead: TTeam;
    Members: array of TPerson;
  end;

implementation

end.
