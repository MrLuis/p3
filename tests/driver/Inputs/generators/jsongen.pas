generator JsonGenerator for Serializable;
{ ToJson for a record or class: an object with its fields, skipping those
  marked [Skip]. }
uses p3gen, sysutils;

function JsonValue(const F: TFieldModel): string;
var
  T: string;
begin
  T := LowerCase(F.TypeName);
  if (T = 'integer') or (T = 'int64') or (T = 'longint') then
    Result := 'IntToStr(' + F.Name + ')'
  else if T = 'string' then
    Result := '''"'' + ' + F.Name + ' + ''"'''
  else if T = 'boolean' then
    Result := 'BoolToJson(' + F.Name + ')'
  else
    Result := '';
end;

procedure Generate(const T: TTypeModel; W: TSourceWriter);
var
  I: Integer;
  F: TFieldModel;
  V: string;
  First: Boolean;
begin
  W.Member('function ToJson: string;');
  W.Line('function ' + T.Name + '.ToJson: string;');
  W.Line('begin');
  W.Line('  Result := ''{''');
  First := True;
  for I := 0 to Length(T.Fields) - 1 do
  begin
    F := T.Fields[I];
    if F.HasAttribute('Skip') then
      Continue;
    V := JsonValue(F);
    if V = '' then
    begin
      W.Error(F, 'type ' + F.TypeName + ' cannot be serialized, mark the field [Skip]');
      Continue;
    end;
    if First then
      W.Line('    + ''"' + F.Name + '": '' + ' + V)
    else
      W.Line('    + '', "' + F.Name + '": '' + ' + V);
    First := False;
  end;
  W.Line('    + ''}'';');
  W.Line('end;');
end;

begin
  RunGenerator(@Generate);
end.
