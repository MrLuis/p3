program sqlite;
{ A small SQLite demo: a people table in a database file (or in memory),
  filled with a few rows, queried with parameters, updated, and summarized.

    sqlite [DATABASE] [REGION]

  DATABASE defaults to :memory: (a database that vanishes at exit); give a
  file name to keep it. REGION defaults to north and is used by the
  parameterized queries and the update. }
{$H+}
uses sysutils, sqlite3, sqlitedb;

procedure SeedPeople(db: TDatabase);
begin
  db.Exec('BEGIN');
  try
    db.Exec`INSERT INTO people (name, region, balance) VALUES (${'Ann'}, ${'north'}, ${1200.50})`;
    db.Exec`INSERT INTO people (name, region, balance) VALUES (${'Bo'}, ${'north'}, ${850.00})`;
    db.Exec`INSERT INTO people (name, region, balance) VALUES (${'Cy'}, ${'south'}, ${430.25})`;
    db.Exec`INSERT INTO people (name, region, balance) VALUES (${'Dee'}, ${'west'}, ${2100.00})`;
    db.Exec`INSERT INTO people (name, region, balance) VALUES (${'Mia'}, ${'south'}, ${nil})`;
    db.Exec('COMMIT');
  except
    db.Exec('ROLLBACK');
    raise;
  end;
end;

procedure ShowEveryone(db: TDatabase);
var
  rows: TResultSet owned;
begin
  rows := db.Query`
    SELECT name, region, balance
    FROM people
    ORDER BY region, name`;
  Writeln('Everyone');
  Writeln('name':12, 'region':9, 'balance':12);
  for var row in rows do
  begin
    Write(row[0].AsString:12, row[1].AsString:9);
    if row[2].IsNull then
      Writeln('n/a':12)
    else
      Writeln(row[2].AsReal:12:2);
  end;
end;

procedure ShowRegion(db: TDatabase; const Region: string);
var
  rows: TResultSet owned;
  total: Double = 0;
  count: Integer = 0;
begin
  rows := db.Query`
    SELECT name, balance
    FROM people
    WHERE region = ${Region}
    ORDER BY balance DESC`;
  Writeln;
  Writeln(`People in ${Region}`);
  for var row in rows do
  begin
    Write('  ', row[0].AsString:10);
    if row[1].IsNull then
      Writeln('n/a':12)
    else
    begin
      Writeln(row[1].AsReal:12:2);
      total := total + row[1].AsReal;
      count := count + 1;
    end;
  end;
  Writeln(`  total: ${total:0:2} over ${count} person(s)`);
end;

procedure GiveRaise(db: TDatabase; const Region: string; Amount: Double);
var
  changed: longint;
begin
  db.Exec`UPDATE people SET balance = COALESCE(balance, 0) + ${Amount} WHERE region = ${Region}`;
  changed := db.Changes;
  Writeln;
  Writeln(`Raised ${changed} person(s) in ${Region} by ${Amount:0:2}`);
end;

procedure Hire(db: TDatabase; const Name, Region: string; Salary: Double);
var
  id: Int64;
begin
  db.Exec`INSERT INTO people (name, region, balance) VALUES (${Name}, ${Region}, ${Salary})`;
  id := db.LastInsertRowId;
  Writeln(`Added ${Name} in ${Region} with id ${id}`);
end;

procedure ShowSummary(db: TDatabase);
begin
  var (people, total) := db.QueryPair`SELECT COUNT(*), SUM(balance) FROM people`;
  Writeln;
  if total.IsNull then
    Writeln(`Database now has ${people.AsInteger} people and no balances`)
  else
    Writeln(`Database now has ${people.AsInteger} people, ${total.AsReal:0:2} in balances`);
end;

procedure Run(const DbPath, Region: string);
var
  db: TDatabase owned;
  count: Int64;
begin
  db := TDatabase.Open(DbPath, ReadOnly := false);
  Writeln(`SQLite ${sqlite3_libversion()} demo, database ${DbPath}`);
  db.Exec`
    CREATE TABLE IF NOT EXISTS people (
      id INTEGER PRIMARY KEY,
      name TEXT NOT NULL,
      region TEXT NOT NULL,
      balance REAL
    )`;
  count := db.QueryValue`SELECT COUNT(*) FROM people`.AsInteger;
  if count = 0 then
  begin
    SeedPeople(db);
    Writeln('Seeded the sample people');
  end;
  ShowEveryone(db);
  ShowRegion(db, Region);
  GiveRaise(db, Region, 100.0);
  ShowRegion(db, Region);
  Hire(db, 'Eve', Region, 900.0);
  ShowSummary(db);
end;

var
  DbPath, Region: string;
begin
  DbPath := if ParamCount >= 1 then ParamStr(1) else ':memory:';
  Region := if ParamCount >= 2 then ParamStr(2) else 'north';
  Run(DbPath, Region);
end.
