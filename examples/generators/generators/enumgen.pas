generator EnumNamesGenerator for EnumNames;
{ For an enumeration marked [EnumNames]: a type helper with ToString (the
  value's name), TryParse (a name, in any case, to the value) and
  ToJsonValue (the name as a JSON string, for the [Json] generator). No
  run-time type information: the names are written into the code. }
uses p3gen, sysutils;

procedure Generate(const T: TTypeModel; W: TSourceWriter);
var
  I: Integer;
  V, H: string;
begin
  if T.Kind <> 'enum' then
  begin
    W.Error(T, '[EnumNames] is for enumerations');
    Exit;
  end;
  H := T.Name + 'Names';
  W.Declaration('{$modeswitch typehelpers}');
  W.Declaration('type');
  W.Declaration('  ' + H + ' = type helper for ' + T.Name);
  W.Declaration('    function ToString: string;');
  W.Declaration('    function ToJsonValue: string;');
  W.Declaration('    class function TryParse(const S: string; out V: ' +
                T.Name + '): Boolean; static;');
  W.Declaration('  end;');

  W.Line('function ' + H + '.ToString: string;');
  W.Line('begin');
  W.Line('  case Self of');
  for I := 0 to Length(T.Values) - 1 do
  begin
    V := T.Values[I].Name;
    W.Line('    ' + T.Name + '.' + V + ': Result := ''' + V + ''';');
  end;
  W.Line('  end;');
  W.Line('end;');

  W.Line('function ' + H + '.ToJsonValue: string;');
  W.Line('begin');
  W.Line('  Result := ''"'' + ToString + ''"'';');
  W.Line('end;');

  W.Line('class function ' + H + '.TryParse(const S: string; out V: ' +
         T.Name + '): Boolean;');
  W.Line('var');
  W.Line('  L: string;');
  W.Line('begin');
  W.Line('  L := LowerCase(S);');
  W.Line('  Result := True;');
  for I := 0 to Length(T.Values) - 1 do
  begin
    V := T.Values[I].Name;
    W.Line('  if L = ''' + LowerCase(V) + ''' then begin V := ' + T.Name +
           '.' + V + '; Exit; end;');
  end;
  W.Line('  Result := False;');
  W.Line('end;');
end;

begin
  RunGenerator(@Generate);
end.
