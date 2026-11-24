# pg

A small PostgreSQL demo written in Pascal: the C API is declared in the RTL's
`libpq` unit (`cdecl; external 'pq'`, so the build links `-lpq`), and the
program connects, makes a table, inserts a row that hands its id back, runs a
prepared statement and prints what the table then holds.

```sh
cd examples/pg
../p3                          # builds bin/pg
bin/pg                         # against localhost's default database
bin/pg "host=localhost dbname=demo user=demo password=demo"
```

```
pg [CONNINFO]
```

The connection string is a libpq `conninfo` (`host=... port=... dbname=...
user=...`). With a server up, the demo reports each step; without one, it
prints libpq's own error (`connection refused` and the like) and stops.

## Sources

| File | Contents |
|---|---|
| `src/pg.pas` | the program: connect, exec, prepare/execPrepared, results |
| `../../rtl/libpq.pas` | the RTL's `libpq` unit, the C headers libpq-fe.h and libpq-events.h of the current PostgreSQL sources, as `cdecl; external 'pq'` declarations |

## Requirements

PostgreSQL's client library where the linker finds it: `brew install libpq`
(the `libraryPaths` entry already points there), on Debian/Ubuntu install
`libpq-dev` (and remove the `libraryPaths` line), under MinGW/MSYS2 install
the `mingw-w64-x86_64-postgresql` package's client library; for a
cross-compiled .exe, make the import library once from the official zip with
`x86_64-w64-mingw32-dlltool -d libpq.def -D libpq.dll -l libpq.a` and add its
directory to `libraryPaths` (as `sqlite`'s `libsqlite3` needs one).

## What it shows

- **libpq's own way**: every command answers one `PGresult` the program owns
  until `PQclear`; a statement's parameters go with the call (values, lengths
  and formats, all text here); `PQexecParams` for the one-off `INSERT ...
  RETURNING` with a `$1` parameter, `PQprepare`+`PQexecPrepared` for the
  select; fields read by `PQfname`/`PQgetvalue`, NULLs by `PQgetisnull`.
- **Errors as strings**: the connection's `PQstatus`/`PQerrorMessage`, the
  command's `PQresultStatus`/`PQresultErrorField` (sqlstate, message,
  statement position and the rest, the `PG.diag` codes).
- **NOTIFY and the connection info**: `PQnotifies` in a select loop if any
  arrive, `PQconndefaults`' table printed (freed with `PQconninfoFree`), the
  server version via `PQserverVersion`, the large-object routines left for
  the `libpq` unit's docs.
