program main;
{$H+}
uses sysutils, json, people;
var
  P, Q: TPerson;
  T, U: TTeam;
  S: string;
begin
  P.Id := 7;
  P.Name := 'Ada "the Countess"';
  P.Score := 99.5;
  P.Active := True;
  P.Big := 18446744073709551615;
  P.Role := Editor;
  P.Home.City := 'London';
  P.Home.Zip := 'NW1';
  SetLength(P.Tags, 2);
  P.Tags[0] := 'math';
  P.Tags[1] := 'poetry';
  SetLength(P.Matrix, 2);
  SetLength(P.Matrix[0], 2);
  P.Matrix[0][0] := 1;
  P.Matrix[0][1] := 2;
  SetLength(P.Matrix[1], 1);
  P.Matrix[1][0] := 3;
  SetLength(P.Others, 1);
  P.Others[0].City := 'Paris';
  S := P.ToJson;
  WriteLn(S);
  Q := TPerson.FromJson(S);
  WriteLn(Q.ToJson = S, ' ', Q.Name, ' ', Q.Role = Editor, ' ', Q.Matrix[1][0],
          ' ', Q.Others[0].City, ' ', Q.Big);
  { unknown keys skipped, missing ones zero, null arrays empty }
  Q := TPerson.FromJson('{"Name": "Bo", "extra": {"x": [1, {"y": 2}]},' +
                        ' "Tags": null, "Role": "Admin", "name": "ignored"}');
  WriteLn(Q.Name, ' ', Q.Id, ' ', Length(Q.Tags), ' ', Q.Role = Admin, ' ',
          Q.Home.City = '');
  T := TTeam.Create;
  T.Title := 'core';
  SetLength(T.Members, 1);
  T.Members[0] := P;
  T.Lead := TTeam.Create;
  T.Lead.Title := 'lead';
  WriteLn(T.ToJson(True));
  U := TTeam.FromJson(T.ToJson);
  WriteLn(U.Title, ' ', U.Lead.Title, ' ', U.Lead.Lead = nil, ' ',
          U.Members[0].Home.Zip);
  try
    TPerson.FromJson('{"Role": "Owner"}');
  except
    on E: EJsonError do
      WriteLn(E.Message);
  end;
  try
    TPerson.FromJson('{"Id": "seven"}');
  except
    on E: EJsonError do
      WriteLn(E.Message);
  end;
  try
    TPerson.FromJson('[1]');
  except
    on E: EJsonError do
      WriteLn(E.Message);
  end;
end.
