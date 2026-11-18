unit sqlitedb;
{ A small, friendly wrapper around the SQLite C API: a connection class,
  results materialized as rows of TValue, and tagged SQL templates that bind
  their values as statement parameters instead of pasting them into the
  SQL. }
{$H+}
{$modeswitch advancedrecords}
interface

uses sqlite3;

type
  ESQLiteError = class(Exception);

  TValueKind = (vkNull, vkInteger, vkReal, vkText, vkBlob);

  { One column value of a result row. SQLite is dynamically typed, so a
    value carries both its kind and its value. }
  TValue = record
    Kind: TValueKind;
    IntValue: Int64;
    RealValue: Double;
    TextValue: string;
    function IsNull: boolean;
    function AsString: string;
    function AsInteger: Int64;
    function AsReal: Double;
  end;

  TRow = array of TValue;

  TColumn = record
    Name: string;
    Kind: TValueKind;
  end;

  TRowEnumerator = class
  private
    FRows: array of TRow;
    FIndex: Integer;
    function GetCurrent: TRow;
  public
    constructor Create(const ARows: array of TRow);
    function MoveNext: boolean;
    property Current: TRow read GetCurrent;
  end;

  TResultSet = class
  private
    FColumns: array of TColumn;
    FRows: array of TRow;
    function GetRow(Index: Integer): TRow;
    function GetRowCount: Integer;
    function GetColumnCount: Integer;
  public
    function GetEnumerator: TRowEnumerator;
    function ColumnName(Index: Integer): string;
    function ColumnIndex(const Name: string): Integer;
    function Value(Row, Column: Integer): TValue;
    property RowCount: Integer read GetRowCount;
    property ColumnCount: Integer read GetColumnCount;
    property Rows[Index: Integer]: TRow read GetRow; default;
  end;

  TDatabase = class
  private
    FHandle: PSQLite3;
    procedure Check(Code: longint; const Context: string);
    function RunQuery(const SQL: string; const Values: array of const): TResultSet;
    procedure RunExec(const SQL: string; const Values: array of const);
  public
    { The file name, or ':memory:'. ReadOnly opens an existing database
      without creating it. }
    class function Open(const FileName: string; ReadOnly: boolean = false): TDatabase;
    destructor Destroy; override;
    { Execute one or more statements, with no parameters. }
    procedure Exec(const SQL: string); overload;
    { Execute one statement, with the values bound to its ? parameters. }
    procedure Exec(const Parts: array of string; const Values: array of const); overload;
    { Run one query with no parameters. }
    function Query(const SQL: string): TResultSet; overload;
    { Run one query whose text and values come from a tagged template:
        db.Query(SQL text with $Region, [Region]) }
    function Query(const Parts: array of string; const Values: array of const): TResultSet; overload;
    { The first column of the first row (vkNull if there is none). }
    function QueryValue(const SQL: string): TValue; overload;
    function QueryValue(const Parts: array of string; const Values: array of const): TValue; overload;
    { The first two columns of the first row, for tuple destructuring. }
    function QueryPair(const SQL: string): (First: TValue, Second: TValue); overload;
    function QueryPair(const Parts: array of string; const Values: array of const):
      (First: TValue, Second: TValue); overload;
    function LastInsertRowId: Int64;
    function Changes: longint;
    property Handle: PSQLite3 read FHandle;
  end;

implementation

uses sysutils;

function NullValue: TValue;
begin
  Result.Kind := vkNull;
  Result.IntValue := 0;
  Result.RealValue := 0;
  Result.TextValue := '';
end;

function TValue.IsNull: boolean;
begin
  IsNull := Kind = vkNull;
end;

function TValue.AsString: string;
begin
  case Kind of
    vkInteger: Result := IntToStr(IntValue);
    vkReal: Result := FloatToStr(RealValue);
    vkText, vkBlob: Result := TextValue;
  else
    Result := '';
  end;
end;

function TValue.AsInteger: Int64;
begin
  case Kind of
    vkInteger: Result := IntValue;
    vkReal: Result := trunc(RealValue);
    vkText: Result := StrToInt64(TextValue);
  else
    Result := 0;
  end;
end;

function TValue.AsReal: Double;
begin
  case Kind of
    vkInteger: Result := IntValue;
    vkReal: Result := RealValue;
    vkText: Result := StrToFloat(TextValue);
  else
    Result := 0;
  end;
end;

constructor TRowEnumerator.Create(const ARows: array of TRow);
var
  I: Integer;
begin
  SetLength(FRows, length(ARows));
  for I := 0 to High(ARows) do
    FRows[I] := ARows[I];
  FIndex := -1;
end;

function TRowEnumerator.MoveNext: boolean;
begin
  FIndex := FIndex + 1;
  MoveNext := FIndex < length(FRows);
end;

function TRowEnumerator.GetCurrent: TRow;
begin
  GetCurrent := FRows[FIndex];
end;

function TResultSet.GetEnumerator: TRowEnumerator;
begin
  GetEnumerator := TRowEnumerator.Create(FRows);
end;

function TResultSet.GetRow(Index: Integer): TRow;
begin
  GetRow := FRows[Index];
end;

function TResultSet.GetRowCount: Integer;
begin
  GetRowCount := length(FRows);
end;

function TResultSet.GetColumnCount: Integer;
begin
  GetColumnCount := length(FColumns);
end;

function TResultSet.ColumnName(Index: Integer): string;
begin
  ColumnName := FColumns[Index].Name;
end;

function TResultSet.ColumnIndex(const Name: string): Integer;
var
  I: Integer;
begin
  for I := 0 to High(FColumns) do
    if SameText(FColumns[I].Name, Name) then
    begin
      ColumnIndex := I;
      exit;
    end;
  ColumnIndex := -1;
end;

function TResultSet.Value(Row, Column: Integer): TValue;
begin
  Value := FRows[Row][Column];
end;

{ The text between the tagged template's values, with a ? for each value. }
function TaggedSQL(const Parts: array of string; ValueCount: Integer): string;
var
  I: Integer;
begin
  if length(Parts) <> ValueCount + 1 then
    raise ESQLiteError.Create('a tagged SQL template needs one more text part than values');
  Result := Parts[0];
  for I := 0 to ValueCount - 1 do
    Result := Result + '?' + Parts[I + 1];
end;

{ An SQLite TEXT or BLOB (n bytes at p) as an ansistring. }
function TextAt(p: pchar; n: longint): string;
begin
  Result := '';
  if (p <> nil) and (n > 0) then
  begin
    SetLength(Result, n);
    Move(p^, Result[1], n);
  end;
end;

function ColumnValue(Stmt: PSQLite3Stmt; Column: Integer): TValue;
var
  P: pchar;
  N: longint;
begin
  case sqlite3_column_type(Stmt, Column) of
    SQLITE_INTEGER:
      begin
        Result := NullValue;
        Result.Kind := vkInteger;
        Result.IntValue := sqlite3_column_int64(Stmt, Column);
      end;
    SQLITE_FLOAT:
      begin
        Result := NullValue;
        Result.Kind := vkReal;
        Result.RealValue := sqlite3_column_double(Stmt, Column);
      end;
    SQLITE_TEXT:
      begin
        Result := NullValue;
        Result.Kind := vkText;
        P := sqlite3_column_text(Stmt, Column);
        N := sqlite3_column_bytes(Stmt, Column);
        Result.TextValue := TextAt(P, N);
      end;
    SQLITE_BLOB:
      begin
        Result := NullValue;
        Result.Kind := vkBlob;
        P := sqlite3_column_blob(Stmt, Column);
        N := sqlite3_column_bytes(Stmt, Column);
        Result.TextValue := TextAt(P, N);
      end;
  else
    Result := NullValue;
  end;
end;

class function TDatabase.Open(const FileName: string; ReadOnly: boolean): TDatabase;
var
  DB: TDatabase;
  Flags, Code: longint;
  Message: string;
begin
  DB := TDatabase.Create;
  if ReadOnly then
    Flags := SQLITE_OPEN_READONLY
  else
    Flags := SQLITE_OPEN_READWRITE or SQLITE_OPEN_CREATE;
  Code := sqlite3_open_v2(pchar(FileName), DB.FHandle, Flags, nil);
  if Code <> SQLITE_OK then
  begin
    Message := string(sqlite3_errmsg(DB.FHandle));
    sqlite3_close(DB.FHandle);
    DB.Free;
    raise ESQLiteError.Create('cannot open ' + FileName + ': ' + Message);
  end;
  Open := DB;
end;

destructor TDatabase.Destroy;
begin
  if FHandle <> nil then
    sqlite3_close(FHandle);
  inherited Destroy;
end;

procedure TDatabase.Check(Code: longint; const Context: string);
begin
  if Code <> SQLITE_OK then
    raise ESQLiteError.Create(Context + ': ' + string(sqlite3_errmsg(FHandle)) +
                              ' (SQLite error ' + IntToStr(Code) + ')');
end;

{ Binds Values to Stmt's ? parameters 1..n, keeping text values alive in
  Keep until the statement has been stepped and finalized. }
procedure BindValues(Stmt: PSQLite3Stmt; const Values: array of const;
                     var Keep: array of string);
var
  I: Integer;
  Code: longint;
begin
  for I := 0 to High(Values) do
  begin
    case Values[I].VType of
      vtInteger:
        Code := sqlite3_bind_int64(Stmt, I + 1, Values[I].VInteger);
      vtInt64:
        Code := sqlite3_bind_int64(Stmt, I + 1, Values[I].VInt64^);
      vtBoolean:
        if Values[I].VBoolean then
          Code := sqlite3_bind_int64(Stmt, I + 1, 1)
        else
          Code := sqlite3_bind_int64(Stmt, I + 1, 0);
      vtExtended:
        Code := sqlite3_bind_double(Stmt, I + 1, Values[I].VExtended^);
      vtChar:
        begin
          Keep[I] := Values[I].VChar;
          Code := sqlite3_bind_text(Stmt, I + 1, pchar(Keep[I]), 1, nil);
        end;
      vtString:
        begin
          Keep[I] := Values[I].VString^;
          Code := sqlite3_bind_text(Stmt, I + 1, pchar(Keep[I]), length(Keep[I]), nil);
        end;
      vtAnsiString:
        begin
          Keep[I] := string(Values[I].VAnsiString);
          Code := sqlite3_bind_text(Stmt, I + 1, pchar(Keep[I]), length(Keep[I]), nil);
        end;
      vtPChar:
        Code := sqlite3_bind_text(Stmt, I + 1, Values[I].VPChar, -1, nil);
      vtPointer:
        if Values[I].VPointer = nil then
          Code := sqlite3_bind_null(Stmt, I + 1)
        else
          raise ESQLiteError.Create('only nil pointers can be bound as SQL values');
    else
      raise ESQLiteError.Create('value ' + IntToStr(I + 1) + ' has a type SQLite cannot bind');
    end;
    if Code <> SQLITE_OK then
      raise ESQLiteError.Create('cannot bind value ' + IntToStr(I + 1) +
                                ' (SQLite error ' + IntToStr(Code) + ')');
  end;
end;

function TDatabase.RunQuery(const SQL: string; const Values: array of const): TResultSet;
var
  Stmt: PSQLite3Stmt;
  Tail: pchar;
  Code: longint;
  N, C: Integer;
  Row: TRow;
  Keep: array of string;
  ResultSet: TResultSet;
begin
  Stmt := nil;
  Tail := nil;
  Code := sqlite3_prepare_v2(FHandle, pchar(SQL), length(SQL), Stmt, Tail);
  if Code <> SQLITE_OK then
    Check(Code, 'prepare');
  try
    SetLength(Keep, length(Values));
    BindValues(Stmt, Values, Keep);
    ResultSet := TResultSet.Create;
    try
      N := sqlite3_column_count(Stmt);
      SetLength(ResultSet.FColumns, N);
      for C := 0 to N - 1 do
      begin
        ResultSet.FColumns[C].Name := string(sqlite3_column_name(Stmt, C));
        ResultSet.FColumns[C].Kind := vkNull;
      end;
      repeat
        Code := sqlite3_step(Stmt);
        if Code = SQLITE_ROW then
        begin
          SetLength(Row, N);
          for C := 0 to N - 1 do
            Row[C] := ColumnValue(Stmt, C);
          SetLength(ResultSet.FRows, length(ResultSet.FRows) + 1);
          ResultSet.FRows[High(ResultSet.FRows)] := Row;
        end
        else if Code <> SQLITE_DONE then
          Check(Code, 'step');
      until Code <> SQLITE_ROW;
      { Fill in the column kinds from the first row, when there is one. }
      if length(ResultSet.FRows) > 0 then
        for C := 0 to N - 1 do
          ResultSet.FColumns[C].Kind := ResultSet.FRows[0][C].Kind;
    except
      ResultSet.Free;
      raise;
    end;
  finally
    sqlite3_finalize(Stmt);
  end;
  RunQuery := ResultSet;
end;

procedure TDatabase.RunExec(const SQL: string; const Values: array of const);
var
  Stmt: PSQLite3Stmt;
  Tail: pchar;
  Code: longint;
  Keep: array of string;
begin
  Stmt := nil;
  Tail := nil;
  Code := sqlite3_prepare_v2(FHandle, pchar(SQL), length(SQL), Stmt, Tail);
  if Code <> SQLITE_OK then
    Check(Code, 'prepare');
  try
    SetLength(Keep, length(Values));
    BindValues(Stmt, Values, Keep);
    repeat
      Code := sqlite3_step(Stmt);
    until Code <> SQLITE_ROW;
    if Code <> SQLITE_DONE then
      Check(Code, 'step');
  finally
    sqlite3_finalize(Stmt);
  end;
end;

procedure TDatabase.Exec(const SQL: string);
var
  Code: longint;
  Message: pchar;
  Detail: string;
begin
  Message := nil;
  Code := sqlite3_exec(FHandle, pchar(SQL), nil, nil, Message);
  if Code <> SQLITE_OK then
  begin
    if Message <> nil then
    begin
      Detail := string(Message);
      sqlite3_free(Message);
    end
    else
      Detail := string(sqlite3_errmsg(FHandle));
    raise ESQLiteError.Create('exec: ' + Detail + ' (SQLite error ' + IntToStr(Code) + ')');
  end;
end;

procedure TDatabase.Exec(const Parts: array of string; const Values: array of const);
begin
  RunExec(TaggedSQL(Parts, length(Values)), Values);
end;

function TDatabase.Query(const SQL: string): TResultSet;
begin
  Query := RunQuery(SQL, []);
end;

function TDatabase.Query(const Parts: array of string;
                         const Values: array of const): TResultSet;
begin
  Query := RunQuery(TaggedSQL(Parts, length(Values)), Values);
end;

function TDatabase.QueryValue(const SQL: string): TValue;
var
  ResultSet: TResultSet;
begin
  ResultSet := Query(SQL);
  try
    if ResultSet.RowCount = 0 then
      QueryValue := NullValue
    else
      QueryValue := ResultSet.FRows[0][0];
  finally
    ResultSet.Free;
  end;
end;

function TDatabase.QueryValue(const Parts: array of string;
                              const Values: array of const): TValue;
var
  ResultSet: TResultSet;
begin
  ResultSet := RunQuery(TaggedSQL(Parts, length(Values)), Values);
  try
    if ResultSet.RowCount = 0 then
      QueryValue := NullValue
    else
      QueryValue := ResultSet.FRows[0][0];
  finally
    ResultSet.Free;
  end;
end;

function TDatabase.QueryPair(const SQL: string): (First: TValue, Second: TValue);
var
  ResultSet: TResultSet;
begin
  ResultSet := Query(SQL);
  try
    if (ResultSet.RowCount = 0) or (ResultSet.ColumnCount < 2) then
      QueryPair := (NullValue, NullValue)
    else
      QueryPair := (ResultSet.FRows[0][0], ResultSet.FRows[0][1]);
  finally
    ResultSet.Free;
  end;
end;

function TDatabase.QueryPair(const Parts: array of string;
                             const Values: array of const): (First: TValue, Second: TValue);
var
  ResultSet: TResultSet;
begin
  ResultSet := RunQuery(TaggedSQL(Parts, length(Values)), Values);
  try
    if (ResultSet.RowCount = 0) or (ResultSet.ColumnCount < 2) then
      QueryPair := (NullValue, NullValue)
    else
      QueryPair := (ResultSet.FRows[0][0], ResultSet.FRows[0][1]);
  finally
    ResultSet.Free;
  end;
end;

function TDatabase.LastInsertRowId: Int64;
begin
  LastInsertRowId := sqlite3_last_insert_rowid(FHandle);
end;

function TDatabase.Changes: longint;
begin
  Changes := sqlite3_changes(FHandle);
end;

end.
