{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
unit p3gen;
{ Source generators: what a generator program uses.

  A generator is a program that starts `generator Name for Attr;`, uses this
  unit, declares  procedure Generate(const T: TTypeModel; W: TSourceWriter)
  and calls RunGenerator(@Generate) in its main block. The compiler builds
  and runs it before compiling the project (p3 --generator=FILE ...): it
  reads, on its standard input, a description of every type marked [Attr],
  and writes, on its standard output, what Generate says through the
  writer. The compiler includes that Pascal source in the unit (or program)
  that declares the type: the member declarations W.Member(...) go in a
  `partial` half of the type, the lines W.Line(...) after the unit's other
  declarations (so they are method bodies, as in an implementation part),
  and the lines W.Declaration(...) next to the type (a type helper, for an
  enumeration, which cannot be partial).

  The types are records, classes and enumerations (with their values).
  The description is syntactic: names, types as written, attributes with
  literal arguments, source locations. It is the same for every run, and
  so is a generator's output. }

interface

type
  { an attribute's argument: Kind is 'int', 'real', 'string', 'bool' or
    'ident' (a name, or any other expression, as written) }
  TAttributeArg = record
    Kind: string;
    Text: string;
  end;

  TAttributeModel = class
  public
    Name: string;
    Line, Col: Integer;
    Args: array of TAttributeArg;
    function ArgCount: Integer;
    { the text of argument I (the value of a string argument) }
    function Text(I: Integer): string;
    function IntValue(I: Integer): Int64;
    function BoolValue(I: Integer): Boolean;
  end;

  TParamModel = class
  public
    Name: string;
    TypeName: string;
    Mode: string; { 'value', 'var', 'const' or 'out' }
  end;

  { what a type, field and method have in common }
  TMemberModel = class
  public
    Name: string;
    Visibility: string; { 'public', 'private', 'protected', ... }
    Line, Col: Integer;
    Attributes: array of TAttributeModel;
    function HasAttribute(const AName: string): Boolean;
    { nil if there is none; the name is compared without regard to case }
    function FindAttribute(const AName: string): TAttributeModel;
  end;

  TFieldModel = class(TMemberModel)
  public
    TypeName: string; { as written }
  end;

  TMethodModel = class(TMemberModel)
  public
    Kind: string; { 'procedure', 'function', 'constructor', 'destructor' }
    ResultType: string; { as written; empty for a procedure }
    IsStatic: Boolean; { a class method }
    Params: array of TParamModel;
  end;

  TTypeModel = class(TMemberModel)
  public
    Kind: string; { 'record', 'class' or 'enum' }
    Parent: string; { the written parent class, if any }
    Fields: array of TFieldModel;
    Methods: array of TMethodModel;
    Values: array of TMemberModel; { an enumeration's, in order }
  end;

  TSourceWriter = class
  private
    FType: TTypeModel;
  public
    { a declaration in the generated half of the type: 'function ToJson: string;' }
    procedure Member(const Decl: string);
    { a line of source after the unit's declarations: a method body }
    procedure Line(const S: string);
    { a line of a declaration next to the type (in the interface of its
      unit), such as a type helper for an enumeration, which cannot be
      partial: 'type TColorHelper = type helper for TColor' }
    procedure Declaration(const S: string);
    { a diagnostic at the declaration of M (a type, field or method) }
    procedure Error(M: TMemberModel; const Msg: string);
    procedure Warning(M: TMemberModel; const Msg: string);
  end;

  TGenerateProc = procedure(const T: TTypeModel; W: TSourceWriter);

{ Reads the model on the standard input, calls Gen for each type, and
  writes the result on the standard output. }
procedure RunGenerator(Gen: TGenerateProc);

implementation

function LowerStr(const S: string): string;
var
  I: Integer;
begin
  Result := S;
  for I := 1 to Length(Result) do
    if (Result[I] >= 'A') and (Result[I] <= 'Z') then
      Result[I] := Chr(Ord(Result[I]) + 32);
end;

function ToInt(const S: string): Int64;
var
  I: Integer;
  Neg: Boolean;
begin
  Result := 0;
  Neg := False;
  for I := 1 to Length(S) do
    if (I = 1) and (S[I] = '-') then
      Neg := True
    else if (S[I] >= '0') and (S[I] <= '9') then
      Result := Result * 10 + (Ord(S[I]) - 48);
  if Neg then
    Result := -Result;
end;

function TAttributeModel.ArgCount: Integer;
begin
  Result := Length(Args);
end;

function TAttributeModel.Text(I: Integer): string;
begin
  Result := Args[I].Text;
end;

function TAttributeModel.IntValue(I: Integer): Int64;
begin
  Result := ToInt(Args[I].Text);
end;

function TAttributeModel.BoolValue(I: Integer): Boolean;
begin
  Result := Args[I].Text = 'true';
end;

function TMemberModel.FindAttribute(const AName: string): TAttributeModel;
var
  I: Integer;
  L: string;
begin
  L := LowerStr(AName);
  Result := nil;
  for I := 0 to Length(Attributes) - 1 do
    if LowerStr(Attributes[I].Name) = L then
      Exit(Attributes[I]);
end;

function TMemberModel.HasAttribute(const AName: string): Boolean;
begin
  Result := FindAttribute(AName) <> nil;
end;

{ --- the output: one fact per line, tab-separated, \ tab and newline escaped }

function Escape(const S: string): string;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(S) do
    case S[I] of
      '\': Result := Result + '\\';
      #9: Result := Result + '\t';
      #10: Result := Result + '\n';
      #13: Result := Result + '\r';
    else
      Result := Result + S[I];
    end;
end;

function Unescape(const S: string): string;
var
  I: Integer;
begin
  Result := '';
  I := 1;
  while I <= Length(S) do
  begin
    if (S[I] = '\') and (I < Length(S)) then
    begin
      Inc(I);
      case S[I] of
        't': Result := Result + #9;
        'n': Result := Result + #10;
        'r': Result := Result + #13;
      else
        Result := Result + S[I];
      end;
    end
    else
      Result := Result + S[I];
    Inc(I);
  end;
end;

procedure TSourceWriter.Member(const Decl: string);
begin
  WriteLn('member'#9, Escape(Decl));
end;

procedure TSourceWriter.Declaration(const S: string);
begin
  WriteLn('decl'#9, Escape(S));
end;

procedure TSourceWriter.Line(const S: string);
begin
  WriteLn('line'#9, Escape(S));
end;

procedure TSourceWriter.Error(M: TMemberModel; const Msg: string);
begin
  WriteLn('error'#9, M.Line, #9, M.Col, #9, Escape(Msg));
end;

procedure TSourceWriter.Warning(M: TMemberModel; const Msg: string);
begin
  WriteLn('warning'#9, M.Line, #9, M.Col, #9, Escape(Msg));
end;

{ --- the input }

{ the fields of a line, split at its tabs }
procedure Split(const Line: string; var Fs: array of string; var Count: Integer);
var
  I, Start: Integer;
begin
  Count := 0;
  Start := 1;
  for I := 1 to Length(Line) + 1 do
    if (I > Length(Line)) or (Line[I] = #9) then
    begin
      if Count < Length(Fs) then
        Fs[Count] := Unescape(Copy(Line, Start, I - Start));
      Inc(Count);
      Start := I + 1;
    end;
end;

procedure RunGenerator(Gen: TGenerateProc);
var
  Line: string;
  Fs: array[0..9] of string;
  N: Integer;
  T: TTypeModel;
  Field: TFieldModel;
  Method: TMethodModel;
  Cur: TMemberModel; { what attr lines belong to }
  Value: TMemberModel;
  A: TAttributeModel;
  P: TParamModel;
  Arg: TAttributeArg;
  W: TSourceWriter;
begin
  W := TSourceWriter.Create;
  T := nil;
  Cur := nil;
  A := nil;
  while not Eof do
  begin
    ReadLn(Line);
    if Line = '' then
      Continue;
    Split(Line, Fs, N);
    if Fs[0] = 'type' then
    begin
      T := TTypeModel.Create;
      T.Name := Fs[1];
      T.Kind := Fs[2];
      T.Parent := Fs[3];
      T.Visibility := 'public';
      T.Line := Integer(ToInt(Fs[4]));
      T.Col := Integer(ToInt(Fs[5]));
      Cur := T;
    end
    else if Fs[0] = 'field' then
    begin
      Field := TFieldModel.Create;
      Field.Name := Fs[1];
      Field.TypeName := Fs[2];
      Field.Visibility := Fs[3];
      Field.Line := Integer(ToInt(Fs[4]));
      Field.Col := Integer(ToInt(Fs[5]));
      SetLength(T.Fields, Length(T.Fields) + 1);
      T.Fields[Length(T.Fields) - 1] := Field;
      Cur := Field;
    end
    else if Fs[0] = 'method' then
    begin
      Method := TMethodModel.Create;
      Method.Name := Fs[1];
      Method.Kind := Fs[2];
      Method.ResultType := Fs[3];
      Method.Visibility := Fs[4];
      Method.IsStatic := Fs[5] = '1';
      Method.Line := Integer(ToInt(Fs[6]));
      Method.Col := Integer(ToInt(Fs[7]));
      SetLength(T.Methods, Length(T.Methods) + 1);
      T.Methods[Length(T.Methods) - 1] := Method;
      Cur := Method;
    end
    else if Fs[0] = 'param' then
    begin
      P := TParamModel.Create;
      P.Name := Fs[1];
      P.TypeName := Fs[2];
      P.Mode := Fs[3];
      SetLength(Method.Params, Length(Method.Params) + 1);
      Method.Params[Length(Method.Params) - 1] := P;
    end
    else if Fs[0] = 'value' then
    begin
      Value := TMemberModel.Create;
      Value.Name := Fs[1];
      Value.Visibility := 'public';
      Value.Line := Integer(ToInt(Fs[2]));
      Value.Col := Integer(ToInt(Fs[3]));
      SetLength(T.Values, Length(T.Values) + 1);
      T.Values[Length(T.Values) - 1] := Value;
      Cur := Value;
    end
    else if Fs[0] = 'attr' then
    begin
      A := TAttributeModel.Create;
      A.Name := Fs[1];
      A.Line := Integer(ToInt(Fs[2]));
      A.Col := Integer(ToInt(Fs[3]));
      SetLength(Cur.Attributes, Length(Cur.Attributes) + 1);
      Cur.Attributes[Length(Cur.Attributes) - 1] := A;
    end
    else if Fs[0] = 'arg' then
    begin
      Arg.Kind := Fs[1];
      Arg.Text := Fs[2];
      SetLength(A.Args, Length(A.Args) + 1);
      A.Args[Length(A.Args) - 1] := Arg;
    end
    else if Fs[0] = 'endtype' then
    begin
      WriteLn('type'#9, Escape(T.Name), #9, Escape(T.Kind));
      Gen(T, W);
    end;
  end;
end;

end.
