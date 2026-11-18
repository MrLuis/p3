# sqlite

A small SQLite 3 demo written in Pascal: the C API is declared with
`external 'sqlite3'`, a unit wraps connections and results, and the program
creates, fills, queries and updates a `people` table.

```sh
cd examples/sqlite
../p3                          # builds bin/sqlite
bin/sqlite                     # an in-memory database with sample data
bin/sqlite people.db south     # a file, and the region to report on
```

```
sqlite [DATABASE] [REGION]
```

`DATABASE` defaults to `:memory:` (a database that vanishes at exit); give a
file name to keep it. `REGION` defaults to `north`. The demo seeds its sample
rows only when the table is empty, then prints everyone, reports on the
region, gives that region a raise, hires one more person, and prints a
summary:

```
Everyone
        name   region     balance
         Ann    north     1200.50
          Bo    north      850.00
          Cy    south      430.25
         Mia    south         n/a
         Dee     west     2100.00

People in north
         Ann     1200.50
          Bo      850.00
  total: 2050.50 over 2 person(s)
...
Database now has 6 people, 5680.75 in balances
```

## Sources

| File | Contents |
|---|---|
| `src/sqlite.pas` | the program: schema, seed data, queries, updates and output |
| `src/lib/sqlite3.pas` | the SQLite C functions used here, as `cdecl; external 'sqlite3'` declarations, and their constants |
| `src/lib/sqlitedb.pas` | `TDatabase`, `TResultSet`, `TRow`, `TValue`, and tagged SQL templates |

## What it shows

- **C integration.** `sqlite3.pas` is a plain external declaration unit:
  opaque `sqlite3` and `sqlite3_stmt` handles are `pointer`, `int64` is
  `int64`, and `const char *` is `pchar`. Naming the library after
  `external` makes p3 link `-lsqlite3`; `p3.json` needs no `libraries` entry.
- **Tagged SQL templates.** The text of a tagged template becomes the SQL and
  each `${value}` becomes a bound `?` parameter, so values never become part
  of the SQL text:

  ```pascal
  rows := db.Query`
    SELECT name, balance
    FROM people
    WHERE region = ${Region}
    ORDER BY balance DESC`;
  ```

  `TDatabase.Exec` works the same way for `INSERT`, `UPDATE` and `DELETE`.
  Both bind integers, int64s, booleans, reals, chars, strings, pchars, and
  `nil` as SQL NULL.
- **A dynamically typed value.** SQLite columns are typed per row, so
  `TValue` records the column's kind (`vkNull`, `vkInteger`, `vkReal`,
  `vkText`, `vkBlob`) and offers `IsNull`, `AsInteger`, `AsReal` and
  `AsString`.
- **Results.** `TResultSet` materializes rows of `TValue`; it is indexable
  (`result[0][1]`), has `RowCount`, `ColumnCount`, `ColumnName` and
  `ColumnIndex`, and enumerates its rows, so `for var row in result do` works.
- **Tuples and templates.** `QueryPair` returns the first two columns of the
  first row as a named tuple, which the program unpacks and prints with a
  template string:

  ```pascal
  var (people, total) := db.QueryPair`SELECT COUNT(*), SUM(balance) FROM people`;
  Writeln(`Database now has ${people.AsInteger} people, ${total.AsReal:0:2} in balances`);
  ```

- **Owned variables, defaults, named arguments.** `Run` owns its connection
  and result sets (`TDatabase owned`, `TResultSet owned`), and opens the
  database with `TDatabase.Open(DbPath, ReadOnly := false)`.

## Requirements and limits

SQLite 3 must be installed where the linker finds it: it is built into macOS,
on Debian/Ubuntu install `libsqlite3-dev` (and SQLite itself for the
`sqlite3` command). The declarations here cover the calls the demo uses, not
the whole API.

The wrapper is meant to be readable, not to be the last word in SQLite
wrappers: a tagged `Query`/`Exec` runs one statement, plain `Exec('...')` runs
a script of several, prepared statements are not reused across calls, text
parameters are bound with SQLITE_STATIC and kept in a local array until the
statement has run, and there are no helpers for blobs beyond keeping their
bytes in a `TValue`.
