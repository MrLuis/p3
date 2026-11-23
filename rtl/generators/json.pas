generator JsonGenerator for Json;
{ Typed JSON for unit json: for each record, class or enumeration marked
  [Json], code that writes it with a TJsonWriter and reads it with a
  TJsonReader, straight into the value (no document in between), as
  sonic-rs and serde do. Use it with  --generator=json  or, in p3.json,
  "generators": ["json"]; the unit declaring the types uses json.

  A record or class T gets

    class procedure JsonWrite(W: TJsonWriter; const V: T); static;
    class function JsonRead(R: TJsonReader): T; static;
    function ToJson(Pretty: Boolean = False): AnsiString;
    class function FromJson(const S: AnsiString): T; static;

  (JsonRead starts at the value's first token, read already). An
  enumeration gets a type helper with JsonWrite and JsonRead (its values by
  name).

  Fields: integers, QWord/UInt64, reals, strings, booleans, dynamic arrays
  of these and of [Json] types, and [Json] types (a class: null for nil).
  [Skip] leaves a field out, [JsonName('n')] names it in the JSON. Reading,
  keys the type does not have are skipped and missing ones leave the
  field's zero value. Other field types are an error at the field. }
uses p3gen, sysutils;

type
  TKind = (tkInt, tkUInt, tkReal, tkString, tkBool, tkArray, tkNamed, tkOther);

var
  { the locals of the routine being written: J0.., N0.. }
  Depth, MaxDepth: Integer;

function Normalize(const S: string): string;
var
  I: Integer;
  Space: Boolean;
begin
  Result := '';
  Space := False;
  for I := 1 to Length(S) do
    if S[I] in [' ', #9, #10, #13] then
      Space := Result <> ''
    else
    begin
      if Space then
        Result := Result + ' ';
      Space := False;
      Result := Result + S[I];
    end;
end;

function KindOf(const TypeName: string; out Elem: string): TKind;
var
  T: string;
  I: Integer;
  Ident: Boolean;
begin
  T := LowerCase(Normalize(TypeName));
  Elem := '';
  if Pos(',' + T + ',', ',integer,longint,int64,shortint,smallint,byte,word,' +
         'cardinal,longword,dword,int8,int16,int32,uint8,uint16,uint32,' +
         'nativeint,sizeint,ptrint,') > 0 then
    Exit(tkInt);
  if Pos(',' + T + ',', ',qword,uint64,nativeuint,sizeuint,ptruint,') > 0 then
    Exit(tkUInt);
  if Pos(',' + T + ',', ',real,double,single,extended,') > 0 then
    Exit(tkReal);
  if Pos(',' + T + ',', ',string,ansistring,shortstring,utf8string,rawbytestring,') > 0 then
    Exit(tkString);
  if T = 'boolean' then
    Exit(tkBool);
  if Pos(',' + T + ',', ',pointer,pchar,pansichar,pwidechar,char,ansichar,' +
         'widechar,variant,tobject,') > 0 then
    Exit(tkOther);
  if Copy(T, 1, 9) = 'array of ' then
  begin
    Elem := Copy(Normalize(TypeName), 10, MaxInt);
    Exit(tkArray);
  end;
  Ident := T <> '';
  for I := 1 to Length(T) do
    if not (T[I] in ['a'..'z', '0'..'9', '_', '.']) then
      Ident := False;
  if Ident then
    Result := tkNamed
  else
    Result := tkOther;
end;

{ Can values of TypeName be written? (the reason in Why if not) }
function Supported(const TypeName: string; out Why: string): Boolean;
var
  Elem: string;
begin
  Why := '';
  case KindOf(TypeName, Elem) of
    tkArray: Result := Supported(Elem, Why);
    tkOther:
      begin
        Why := 'a field of type ' + Normalize(TypeName) +
               ' cannot be read or written as JSON (only numbers, strings, ' +
               'booleans, dynamic arrays and [Json] types); mark it [Skip]';
        Result := False;
      end;
  else
    Result := True;
  end;
end;

{ a line, unless W is nil (a first pass, counting the locals) }
procedure Put(W: TSourceWriter; const S: string);
begin
  if W <> nil then
    W.Line(S);
end;

procedure NeedLocals;
begin
  Inc(Depth);
  if Depth > MaxDepth then
    MaxDepth := Depth;
end;

{ lines that write E (of type TypeName) }
procedure EmitWrite(W: TSourceWriter; const TypeName, E, Ind: string);
var
  Elem, J: string;
begin
  case KindOf(TypeName, Elem) of
    tkInt: Put(W, Ind + 'W.Value(Int64(' + E + '));');
    tkUInt: Put(W, Ind + 'W.ValueUInt64(' + E + ');');
    tkReal: Put(W, Ind + 'W.Value(' + E + ');');
    tkString: Put(W, Ind + 'W.Value(' + E + ');');
    tkBool: Put(W, Ind + 'W.Value(' + E + ');');
    tkNamed: Put(W, Ind + Normalize(TypeName) + '.JsonWrite(W, ' + E + ');');
    tkArray:
      begin
        J := 'J' + IntToStr(Depth);
        NeedLocals;
        Put(W, Ind + 'W.BeginArray;');
        Put(W, Ind + 'for ' + J + ' := 0 to High(' + E + ') do');
        Put(W, Ind + 'begin');
        EmitWrite(W, Elem, E + '[' + J + ']', Ind + '  ');
        Put(W, Ind + 'end;');
        Put(W, Ind + 'W.EndArray;');
        Dec(Depth);
      end;
  end;
end;

{ lines that read the current value into T (of type TypeName) }
procedure EmitRead(W: TSourceWriter; const TypeName, T, Ind: string);
var
  Elem, N: string;
begin
  case KindOf(TypeName, Elem) of
    tkInt: Put(W, Ind + T + ' := R.AsInt64;');
    tkUInt: Put(W, Ind + T + ' := R.AsUInt64;');
    tkReal: Put(W, Ind + T + ' := R.AsDouble;');
    tkString: Put(W, Ind + T + ' := R.AsString;');
    tkBool: Put(W, Ind + T + ' := R.AsBoolean;');
    tkNamed: Put(W, Ind + T + ' := ' + Normalize(TypeName) + '.JsonRead(R);');
    tkArray:
      begin
        N := 'N' + IntToStr(Depth);
        NeedLocals;
        Put(W, Ind + 'SetLength(' + T + ', 0);');
        Put(W, Ind + 'if R.Token <> jtNull then');
        Put(W, Ind + 'begin');
        Put(W, Ind + '  if R.Token <> jtBeginArray then');
        Put(W, Ind + '    R.Expected(''an array'');');
        Put(W, Ind + '  ' + N + ' := 0;');
        Put(W, Ind + '  while R.Read and (R.Token <> jtEndArray) do');
        Put(W, Ind + '  begin');
        Put(W, Ind + '    if ' + N + ' = Length(' + T + ') then');
        Put(W, Ind + '      SetLength(' + T + ', ' + N + ' * 2 + 4);');
        EmitRead(W, Elem, T + '[' + N + ']', Ind + '    ');
        Put(W, Ind + '    Inc(' + N + ');');
        Put(W, Ind + '  end;');
        Put(W, Ind + '  SetLength(' + T + ', ' + N + ');');
        Put(W, Ind + 'end;');
        Dec(Depth);
      end;
  end;
end;

function JsonNameOf(F: TFieldModel): string;
var
  A: TAttributeModel;
begin
  A := F.FindAttribute('JsonName');
  if (A <> nil) and (A.ArgCount > 0) then
    Result := A.Text(0)
  else
    Result := F.Name;
end;

{ Name as a Pascal string literal }
function Quoted(const Name: string): string;
begin
  Result := '''' + StringReplace(Name, '''', '''''', [rfReplaceAll]) + '''';
end;

{ the var part of a routine with MaxDepth levels of arrays: its counters
  Prefix0, Prefix1, ... }
procedure EmitLocals(W: TSourceWriter; const Prefix: string);
var
  I: Integer;
  S: string;
begin
  S := '';
  for I := 0 to MaxDepth - 1 do
  begin
    if S <> '' then
      S := S + ', ';
    S := S + Prefix + IntToStr(I);
  end;
  if S <> '' then
  begin
    W.Line('var');
    W.Line('  ' + S + ': Integer;');
  end;
end;

procedure GenerateEnum(const T: TTypeModel; W: TSourceWriter);
var
  I: Integer;
  H, V: string;
begin
  H := T.Name + 'Json';
  W.Declaration('{$modeswitch typehelpers}');
  W.Declaration('type');
  W.Declaration('  ' + H + ' = type helper for ' + T.Name);
  W.Declaration('    class procedure JsonWrite(W: TJsonWriter; V: ' + T.Name +
                '); static;');
  W.Declaration('    class function JsonRead(R: TJsonReader): ' + T.Name +
                '; static;');
  W.Declaration('  end;');

  W.Line('class procedure ' + H + '.JsonWrite(W: TJsonWriter; V: ' + T.Name + ');');
  W.Line('begin');
  W.Line('  case V of');
  for I := 0 to Length(T.Values) - 1 do
  begin
    V := T.Values[I].Name;
    W.Line('    ' + T.Name + '.' + V + ': W.Value(' + Quoted(V) + ');');
  end;
  W.Line('  end;');
  W.Line('end;');

  W.Line('class function ' + H + '.JsonRead(R: TJsonReader): ' + T.Name + ';');
  W.Line('begin');
  W.Line('  if R.Token <> jtString then');
  W.Line('    R.Expected(''a string (' + T.Name + ')'');');
  for I := 0 to Length(T.Values) - 1 do
  begin
    V := T.Values[I].Name;
    W.Line('  if R.StringEquals(' + Quoted(V) + ') then');
    W.Line('    Exit(' + T.Name + '.' + V + ');');
  end;
  W.Line('  R.Error(''unknown ' + T.Name + ' value '''''' + R.AsString + '''''''', R.TokenStart - 1);');
  W.Line('  Result := Low(' + T.Name + ');');
  W.Line('end;');
end;

procedure Generate(const T: TTypeModel; W: TSourceWriter);
var
  I: Integer;
  F: TFieldModel;
  Why, Target: string;
  IsClass, First: Boolean;
  Fields: array of TFieldModel;
begin
  if T.Kind = 'enum' then
  begin
    GenerateEnum(T, W);
    Exit;
  end;
  IsClass := T.Kind = 'class';
  SetLength(Fields, 0);
  for I := 0 to Length(T.Fields) - 1 do
  begin
    F := T.Fields[I];
    if F.HasAttribute('Skip') then
      Continue;
    if not Supported(F.TypeName, Why) then
    begin
      W.Error(F, Why);
      Continue;
    end;
    SetLength(Fields, Length(Fields) + 1);
    Fields[High(Fields)] := F;
  end;

  W.Member('class procedure JsonWrite(W: TJsonWriter; const V: ' + T.Name +
           '); static;');
  W.Member('class function JsonRead(R: TJsonReader): ' + T.Name + '; static;');
  W.Member('function ToJson(Pretty: Boolean = False): AnsiString;');
  W.Member('class function FromJson(const S: AnsiString): ' + T.Name + '; static;');

  { JsonWrite }
  Depth := 0;
  MaxDepth := 0;
  W.Line('class procedure ' + T.Name + '.JsonWrite(W: TJsonWriter; const V: ' +
         T.Name + ');');
  { (the body first, to know its locals) }
  for I := 0 to Length(Fields) - 1 do
    EmitWrite(nil, Fields[I].TypeName, 'V.' + Fields[I].Name, '  ');
  EmitLocals(W, 'J');
  W.Line('begin');
  if IsClass then
  begin
    W.Line('  if V = nil then');
    W.Line('  begin');
    W.Line('    W.Null;');
    W.Line('    Exit;');
    W.Line('  end;');
  end;
  W.Line('  W.BeginObject;');
  for I := 0 to Length(Fields) - 1 do
  begin
    W.Line('  W.Key(' + Quoted(JsonNameOf(Fields[I])) + ');');
    EmitWrite(W, Fields[I].TypeName, 'V.' + Fields[I].Name, '  ');
  end;
  W.Line('  W.EndObject;');
  W.Line('end;');

  { JsonRead }
  Depth := 0;
  MaxDepth := 0;
  for I := 0 to Length(Fields) - 1 do
    EmitRead(nil, Fields[I].TypeName, 'V.' + Fields[I].Name, '');
  W.Line('class function ' + T.Name + '.JsonRead(R: TJsonReader): ' + T.Name + ';');
  EmitLocals(W, 'N');
  W.Line('var');
  W.Line('  V: ' + T.Name + ';');
  W.Line('begin');
  if IsClass then
  begin
    W.Line('  if R.Token = jtNull then');
    W.Line('    Exit(nil);');
    W.Line('  V := ' + T.Name + '.Create;');
  end
  else
    W.Line('  FillChar(V, SizeOf(V), 0);');
  W.Line('  if R.Token <> jtBeginObject then');
  W.Line('    R.Expected(''an object (' + T.Name + ')'');');
  W.Line('  while R.Read and (R.Token = jtKey) do');
  First := True;
  for I := 0 to Length(Fields) - 1 do
  begin
    F := Fields[I];
    if First then
      W.Line('    if R.StringEquals(' + Quoted(JsonNameOf(F)) + ') then')
    else
      W.Line('    else if R.StringEquals(' + Quoted(JsonNameOf(F)) + ') then');
    First := False;
    W.Line('    begin');
    W.Line('      R.Read;');
    Target := 'V.' + F.Name;
    EmitRead(W, F.TypeName, Target, '      ');
    W.Line('    end');
  end;
  if First then
    W.Line('    R.Skip;')
  else
  begin
    W.Line('    else');
    W.Line('      R.Skip;');
  end;
  W.Line('  Result := V;');
  W.Line('end;');

  { ToJson, FromJson }
  W.Line('function ' + T.Name + '.ToJson(Pretty: Boolean): AnsiString;');
  W.Line('var');
  W.Line('  W: TJsonWriter;');
  W.Line('begin');
  W.Line('  W := TJsonWriter.Create(Pretty);');
  W.Line('  try');
  W.Line('    JsonWrite(W, Self);');
  W.Line('    Result := W.Text;');
  W.Line('  finally');
  W.Line('    W.Free;');
  W.Line('  end;');
  W.Line('end;');
  W.Line('class function ' + T.Name + '.FromJson(const S: AnsiString): ' + T.Name + ';');
  W.Line('var');
  W.Line('  R: TJsonReader;');
  W.Line('begin');
  W.Line('  R := TJsonReader.Create(S);');
  W.Line('  try');
  W.Line('    R.Read;');
  W.Line('    Result := JsonRead(R);');
  W.Line('    while R.Read do');
  W.Line('      ;');
  W.Line('  finally');
  W.Line('    R.Free;');
  W.Line('  end;');
  W.Line('end;');
end;

begin
  RunGenerator(@Generate);
end.
