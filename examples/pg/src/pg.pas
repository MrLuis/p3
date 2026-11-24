program pg;

{ PostgreSQL's demo, written with the library's own calls: the RTL's libpq
  unit declares the C API (external 'pq' links -lpq), and the program
  connects to the conninfo on the command line (the default is localhost's
  postgres database), makes a table, inserts a row with a parameter and
  takes the inserted name back, prepares the select and runs it, then
  prints the table. A refused connection prints libpq's error and stops. }
{$H+}

uses libpq;

var
  Info: string;
  conn: PPGConn;
  res: PPGResult;
  q, s1, s2: string;
  i: longint;

  { The parameter list of PQexecParams: one $1, a nil-ended values array. }
  Values: array[0..0] of pchar;

procedure Fail(conn: PPGConn; Res: PPGResult);
begin
  writeln(PQresStatus(PQresultStatus(Res)), ': ',
          pchar(PQresultErrorField(Res, PG_DIAG_MESSAGE_PRIMARY)),
          ' (sqlstate ', PQresultErrorField(Res, PG_DIAG_SQLSTATE), ')');
  PQclear(Res);
  PQfinish(conn);
  halt(1);
end;

begin
  if ParamCount > 0 then
    Info := ParamStr(1)
  else
    Info := 'host=127.0.0.1 port=5432 dbname=postgres connect_timeout=2';
  writeln('libpq, version ', PQlibVersion, ' threadsafe ', PQisthreadsafe);

  { Connection: the whole call, one failure point. }
  conn := PQconnectdb(pchar(Info));
  if PQstatus(conn) <> CONNECTION_OK then
  begin
    writeln('connect failed: ', PQerrorMessage(conn));
    PQfinish(conn);
    halt(1);
  end;
  writeln('connected: db ', PQdb(conn), ' at ', PQhost(conn), ':', PQport(conn),
          ', server version ', PQserverVersion(conn));

  { One command, no parameters: the table. }
  q := 'create table if not exists people (name text, balance numeric)';
  res := PQexec(conn, pchar(q));
  if PQresultStatus(res) <> PGRES_COMMAND_OK then Fail(conn, res);
  PQclear(res);

  { An insert with a $1 parameter, the name coming back. }
  s1 := 'Ann';
  Values[0] := pchar(s1);
  q := 'insert into people (name) values ($1) returning name';
  res := PQexecParams(conn, pchar(q), 1, nil, PPChar(@Values[0]), nil, nil, 0);
  if PQresultStatus(res) <> PGRES_TUPLES_OK then Fail(conn, res);
  writeln('inserted row for ', PQgetvalue(res, 0, 0), ' (command status ',
          PQcmdStatus(res), ')');
  PQclear(res);

  { A prepared statement: describe, then run twice through execPrepared. }
  q := 'select name, balance from people where name = $1';
  res := PQprepare(conn, 'demo.select', pchar(q), 1, nil);
  if PQresultStatus(res) <> PGRES_COMMAND_OK then Fail(conn, res);
  PQclear(res);
  res := PQdescribePrepared(conn, 'demo.select');
  if PQresultStatus(res) <> PGRES_COMMAND_OK then Fail(conn, res);
  writeln('prepared ', PQnfields(res), '-column result, ',
          PQnparams(res), ' parameter of type id ', PQparamtype(res, 0));
  PQclear(res);

  s2 := 'Bo';
  Values[0] := pchar(s2);
  res := PQexecPrepared(conn, 'demo.select', 1, PPChar(@Values[0]),
                        nil, nil, 0);
  if PQresultStatus(res) <> PGRES_TUPLES_OK then Fail(conn, res);
  writeln('select: ', PQntuples(res), ' row(s), column ', PQfname(res, 0),
          ', NULLs by PQgetisnull');
  PQclear(res);

  { What the table then holds. }
  q := 'select name from people';
  res := PQexec(conn, pchar(q));
  if PQresultStatus(res) <> PGRES_TUPLES_OK then Fail(conn, res);
  for i := 0 to PQntuples(res) - 1 do
    if PQgetisnull(res, i, 0) = 0 then
      writeln('  row: ', PQgetvalue(res, i, 0))
    else
     writeln('  row: <null>');
  PQclear(res);

  { The demo's own count, one more exec. }
  q := 'select count(*) from people';
  res := PQexec(conn, pchar(q));
  if PQresultStatus(res) <> PGRES_TUPLES_OK then Fail(conn, res);
  writeln('people now: ', PQgetvalue(res, 0, 0));
  PQclear(res);

  PQfinish(conn);
end.
