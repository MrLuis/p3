program jsontool;
{ A small JSON tool on unit json (see README.md):

    jsontool pretty FILE          the document, indented
    jsontool compact FILE         the document on one line
    jsontool get FILE POINTER     one value (/a/0/b), found without parsing
                                  the rest of the file
    jsontool check FILE           valid, or where it is not
    jsontool stats FILE           what the document holds
    jsontool team FILE            data/team.json as records (typed) }
{$H+}
uses sysutils, json, team;

function LoadFile(const Path: string): string;
var
  F: file;
  S: string;
begin
  if not FileExists(Path) then
  begin
    WriteLn('jsontool: cannot open ''', Path, '''');
    Halt(1);
  end;
  Assign(F, Path);
  Reset(F, 1);
  SetLength(S, FileSize(F));
  if Length(S) > 0 then
    BlockRead(F, S[1], Length(S));
  Close(F);
  Result := S;
end;

procedure Usage;
begin
  WriteLn('usage: jsontool pretty|compact|check|stats|team FILE');
  WriteLn('       jsontool get FILE /json/pointer');
  Halt(2);
end;

{ counts of each kind of value, and the deepest nesting }
procedure Count(const V: TJsonValue; Depth: Integer;
                var Counts: array of Integer; var Deepest: Integer);
var
  E: TJsonValue;
begin
  Inc(Counts[Ord(V.Kind)]);
  if Depth > Deepest then
    Deepest := Depth;
  for E in JsonItems(V) do
    Count(E, Depth + 1, Counts, Deepest);
end;

procedure Stats(const Text: string);
const
  Names: array[TJsonKind] of string = ('null', 'false', 'true', 'number',
    'string', 'array', 'object');
var
  D: TJsonDocument;
  Counts: array[0..6] of Integer;
  K: TJsonKind;
  Deepest: Integer;
begin
  D := JsonParse(Text);
  try
    FillChar(Counts, SizeOf(Counts), 0);
    Deepest := 0;
    Count(D.Root, 1, Counts, Deepest);
    for K := Low(TJsonKind) to High(TJsonKind) do
      WriteLn(Names[K]:8, ' ', Counts[Ord(K)]);
    WriteLn('depth':8, ' ', Deepest);
  finally
    D.Free;
  end;
end;

procedure ShowTeam(const Text: string);
var
  T: TTeam;
  M: TMember;
  I: Integer;
begin
  T := TTeam.FromJson(Text);
  WriteLn(T.Team, ': ', Length(T.Members), ' members, budget ',
          T.Budget:0:2);
  for M in T.Members do
  begin
    Write('  ', M.Name, ' (', M.Level.ToString, ', since ', M.Year, ')');
    for I := 0 to High(M.Skills) do
      Write(' ', M.Skills[I]);
    WriteLn;
  end;
  { changed, and written back }
  T.Members[High(T.Members)].Level := Senior;
  WriteLn(T.Members[High(T.Members)].ToJson);
end;

var
  Cmd, Text: string;
  D: TJsonDocument;
  L: TJsonLazy;
begin
  if ParamCount < 2 then
    Usage;
  Cmd := ParamStr(1);
  Text := LoadFile(ParamStr(2));
  try
    if (Cmd = 'pretty') or (Cmd = 'compact') then
    begin
      D := JsonParse(Text);
      WriteLn(D.ToJson(Cmd = 'pretty'));
      D.Free;
    end
    else if Cmd = 'get' then
    begin
      if ParamCount < 3 then
        Usage;
      L := JsonGetPath(Text, ParamStr(3));
      if not L.Exists then
      begin
        WriteLn('jsontool: no value at ', ParamStr(3));
        Halt(1);
      end;
      if L.Kind = jkString then
        WriteLn(L.AsString)
      else
        WriteLn(L.Raw);
    end
    else if Cmd = 'check' then
    begin
      D := JsonParse(Text);
      D.Free;
      WriteLn(ParamStr(2), ': valid JSON');
    end
    else if Cmd = 'stats' then
      Stats(Text)
    else if Cmd = 'team' then
      ShowTeam(Text)
    else
      Usage;
  except
    on E: EJsonError do
    begin
      WriteLn(ParamStr(2), ':', E.Line, ':', E.Column, ': ', E.Reason);
      Halt(1);
    end;
  end;
end.
