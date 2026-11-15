(* Template strings with classes (objfpc): a method as a tag, values that
   are method calls and properties, an exception raised by a value, and
   templates as the values of variables declared among the statements
   and in a generic routine.
   RUN: %pcc --mode=objfpc %s -o %t
   RUN: %t | diff - %S/template-classes.expected
   RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
   RUN: %t.fast | diff - %S/template-classes.expected
*)
program templateclasses;
{$H+}

type
  TQuery = class
    text: string;
    params: array of string;
    function Describe: string;
  end;

  TDatabase = class
  private
    FName: string;
    FQueries: longint;
  public
    constructor Create(const name: string);
    { a tag: the values become numbered parameters }
    function Sql(const parts: array of string;
                 const values: array of const): TQuery;
    class function Shout(const parts: array of string;
                         const values: array of const): string;
    function Fail(n: longint): longint;
    property Name: string read FName;
    property Queries: longint read FQueries;
  end;

  EBroken = class(Exception)
  end;

function TQuery.Describe: string;
var
  i: longint;
begin
  Result := `text = "${text}"`;
  for i := 0 to high(params) do
    Result := `${Result}, p${i + 1} = ${params[i]}`;
end;

constructor TDatabase.Create(const name: string);
begin
  FName := name;
end;

function TDatabase.Sql(const parts: array of string;
                       const values: array of const): TQuery;
var
  i: longint;
begin
  FQueries := FQueries + 1;
  Result := TQuery.Create;
  setlength(Result.params, high(values) + 1);
  for i := 0 to high(parts) do
  begin
    Result.text := Result.text + parts[i];
    if i <= high(values) then
    begin
      Result.text := `${Result.text}:p${i + 1}`;
      case values[i].vtype of
        vtInteger: Result.params[i] := `${values[i].vinteger}`;
        vtAnsiString: Result.params[i] := ansistring(values[i].vansistring);
      else
        Result.params[i] := '?';
      end;
    end;
  end;
end;

class function TDatabase.Shout(const parts: array of string;
                               const values: array of const): string;
var
  i: longint;
begin
  Result := '';
  for i := 0 to high(parts) do
    Result := Result + '<' + parts[i] + '>';
  Result := `${Result} (${high(values) + 1})`;
end;

function TDatabase.Fail(n: longint): longint;
begin
  if n > 1 then
    raise EBroken.Create(`broken at ${n}`);
  Result := n;
end;

{ a generic routine's tokens are parsed again for each type }
generic function Show<T>(const v: T): string;
begin
  Result := `
    value ${v:6}
      shown
    `;
end;

var
  db: TDatabase;
  q: TQuery;
  region: string;
  rank, i: longint;

begin
  db := TDatabase.Create('main');
  region := 'north';
  rank := 5;

  q := db.Sql`
    SELECT id, name
    FROM customers
    WHERE region = ${region} AND rank > ${rank + 1}
    `;
  writeln(q.Describe);
  q.Free;
  q := db.Sql`SELECT 1`;
  writeln(q.Describe);
  q.Free;
  writeln(`${db.Name} ran ${db.Queries} queries`);
  writeln(TDatabase.Shout`loud ${rank} and clear`);

  { an exception raised by a value: the text so far is dropped, and the
    templates that follow start clean }
  for i := 1 to 3 do
    try
      writeln(`before ${i} [${db.Fail(i)}] after`);
    except
      on e: EBroken do
        writeln(`caught "${e.Message}" in round ${i}`);
    end;
  writeln(`still ${'fine'}`);

  var line := `inline ${rank}`;
  var both := `${line} and ${`inner ${region}`}`;
  writeln(both);
  for var k := 1 to 2 do
  begin
    var row := `
      row ${k}
        of ${2}
      `;
    writeln(row);
  end;
  writeln(specialize Show<longint>(42));
  writeln(specialize Show<boolean>(rank > 3));
  line += ` and ${rank + 1}`;
  writeln(line);
  db.Free;
end.
