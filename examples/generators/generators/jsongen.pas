generator JsonGenerator for Json;
{ For a record or class marked [Json]: ToJson, an object with its fields.
  [Skip] leaves a field out; [JsonName('n')] names it in the JSON. A field
  can be an integer, real, string or boolean, an enumeration marked
  [EnumNames] (its name) or another [Json] type (an object); an array or
  a pointer is reported at the field. The checks happen while compiling,
  not when the program runs. }
uses p3gen, sysutils;

function IsOneOf(const T, List: string): Boolean;
begin
  Result := Pos(',' + T + ',', List) > 0;
end;

{ the Pascal expression for field F's JSON value, '' if there is none }
function JsonValue(F: TFieldModel): string;
var
  T: string;
begin
  T := LowerCase(F.TypeName);
  if IsOneOf(T, ',integer,longint,int64,byte,word,cardinal,smallint,') then
    Result := 'IntToStr(' + F.Name + ')'
  else if IsOneOf(T, ',double,real,single,') then
    Result := 'FloatToStr(' + F.Name + ')'
  else if T = 'string' then
    Result := 'JsonString(' + F.Name + ')'
  else if T = 'boolean' then
    Result := 'JsonBool(' + F.Name + ')'
  else if (T <> '') and (Pos('[', T) = 0) and (Pos(' ', T) = 0) then
    { another type: an enumeration marked [EnumNames] or a type marked
      [Json], which both have ToJsonValue (the compiler says if it has
      not) }
    Result := F.Name + '.ToJsonValue'
  else
    Result := '';
end;

procedure Generate(const T: TTypeModel; W: TSourceWriter);
var
  I: Integer;
  F: TFieldModel;
  Name, V, Sep: string;
  A: TAttributeModel;
begin
  if T.Kind = 'enum' then
  begin
    W.Error(T, '[Json] is for records and classes; mark an enumeration [EnumNames]');
    Exit;
  end;
  W.Member('function ToJson: string;');
  { (as a field of another [Json] type) }
  W.Member('function ToJsonValue: string;');
  W.Line('function ' + T.Name + '.ToJsonValue: string;');
  W.Line('begin');
  W.Line('  Result := ToJson;');
  W.Line('end;');
  W.Line('function ' + T.Name + '.ToJson: string;');
  W.Line('begin');
  W.Line('  Result := ''{''');
  Sep := '';
  for I := 0 to Length(T.Fields) - 1 do
  begin
    F := T.Fields[I];
    if F.HasAttribute('Skip') then
      Continue;
    V := JsonValue(F);
    if V = '' then
    begin
      W.Error(F, 'a field of type ' + F.TypeName +
                 ' cannot be written as JSON; mark it [Skip]');
      Continue;
    end;
    Name := F.Name;
    A := F.FindAttribute('JsonName');
    if A <> nil then
      Name := A.Text(0);
    W.Line('    + ''' + Sep + '"' + Name + '": '' + ' + V);
    Sep := ', ';
  end;
  W.Line('    + ''}'';');
  W.Line('end;');
end;

begin
  RunGenerator(@Generate);
end.
