program jsonbench;
{ Unit json on the JSON benchmark files (data/, see README.md): MB/s for
  reading every token, parsing into a document, writing a document back,
  finding one late value lazily, and (canada.json) reading into records.

    jsonbench DATADIR [SECONDS] }
{$H+}
uses sysutils, json, canadatypes;

function c_clock: Int64; cdecl; external 'c' name 'clock';

var
  Budget: Double = 1.0;

function Now: Double;
begin
  Result := c_clock / 1000000;
end;

type
  TOp = procedure;

function LoadFile(const Path: string): string;
var
  F: file;
  S: string;
begin
  Assign(F, Path);
  Reset(F, 1);
  SetLength(S, FileSize(F));
  if Length(S) > 0 then
    BlockRead(F, S[1], Length(S));
  Close(F);
  Result := S;
end;

var
  Text: string;
  Sink: Int64;

procedure ReadAll;
var
  R: TJsonReader;
begin
  R := TJsonReader.Create(Text);
  try
    while R.Read do
      case R.Token of
        jtString, jtKey: Inc(Sink, Length(R.AsString));
        jtNumber: if R.AsDouble > 0 then Inc(Sink);
      end;
  finally
    R.Free;
  end;
end;

procedure ParseDom;
var
  D: TJsonDocument;
begin
  D := JsonParse(Text);
  Inc(Sink, D.Root.Count);
  D.Free;
end;

var
  WriteDoc: TJsonDocument;

procedure WriteDom;
begin
  Inc(Sink, Length(WriteDoc.ToJson));
end;

var
  LazyPath: array of TJsonKey;

procedure Lazy;
begin
  Inc(Sink, Length(JsonGet(Text, LazyPath).Raw));
end;

procedure Typed;
var
  C: TFeatureCollection;
begin
  C := TFeatureCollection.FromJson(Text);
  Inc(Sink, Length(C.features[0].geometry.coordinates));
end;

procedure TypedWrite;
var
  C: TFeatureCollection;
begin
  C := TFeatureCollection.FromJson(Text);
  Inc(Sink, Length(C.ToJson));
end;

{ the best time of runs within the budget, as MB/s }
procedure Measure(const Name: string; Op: TOp);
var
  Start, T, Best: Double;
  Runs: Integer;
begin
  Best := 1e9;
  Runs := 0;
  Start := Now;
  repeat
    T := Now;
    Op();
    T := Now - T;
    if T < Best then
      Best := T;
    Inc(Runs);
  until (Now - Start > Budget) and (Runs >= 3);
  WriteLn(Format('  %-14s %8.1f MB/s  (%6.2f ms)', [Name,
          Length(Text) / Best / 1e6, Best * 1000]));
end;

procedure Bench(const Dir, Name: string; const Path: array of TJsonKey);
var
  I: Integer;
begin
  Text := LoadFile(Dir + '/' + Name);
  WriteLn(Name, ' (', Length(Text), ' bytes)');
  SetLength(LazyPath, Length(Path));
  for I := 0 to High(Path) do
    LazyPath[I] := Path[I];
  Measure('read tokens', @ReadAll);
  Measure('parse dom', @ParseDom);
  WriteDoc := JsonParse(Text);
  Measure('write dom', @WriteDom);
  WriteDoc.Free;
  Measure('lazy get', @Lazy);
  if Name = 'canada.json' then
  begin
    Measure('typed read', @Typed);
    Measure('typed write', @TypedWrite);
  end;
end;

{ an object of 20000 members built with Put (every Put looks its key up),
  then every member read with Get; ms per round }
procedure PutGet;
var
  D: TJsonDocument;
  O: TJsonValue;
  I: Integer;
begin
  D := TJsonDocument.Create;
  O := D.NewObject;
  for I := 0 to 19999 do
    O.Put('member' + IntToStr(I), Int64(I));
  for I := 0 to 19999 do
    Inc(Sink, O.Get('member' + IntToStr(I)).AsInt64);
  D.Free;
end;

procedure Synthetic;
var
  Start, T, Best: Double;
  Runs: Integer;
begin
  WriteLn('object, 20000 members');
  Best := 1e9;
  Runs := 0;
  Start := Now;
  repeat
    T := Now;
    PutGet;
    T := Now - T;
    if T < Best then
      Best := T;
    Inc(Runs);
  until (Now - Start > Budget) and (Runs >= 3);
  WriteLn(Format('  %-14s %8.2f ms', ['Put + Get', Best * 1000]));
end;

begin
  if ParamCount < 1 then
  begin
    WriteLn('usage: jsonbench DATADIR [SECONDS]');
    Halt(2);
  end;
  if ParamCount > 1 then
    Budget := StrToFloat(ParamStr(2));
  Bench(ParamStr(1), 'twitter.json', ['statuses', 99, 'user', 'screen_name']);
  Bench(ParamStr(1), 'citm_catalog.json', ['performances', 242, 'id']);
  Bench(ParamStr(1), 'canada.json', ['features', 0, 'geometry', 'coordinates', 479, 0, 0]);
  Synthetic;
  if Sink = 42 then
    WriteLn;
end.
