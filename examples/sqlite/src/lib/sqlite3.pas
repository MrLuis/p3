unit sqlite3;
{ The SQLite 3 C API, as far as this example uses it. Declaring a routine
  external 'sqlite3' links the program with -lsqlite3; see docs/README.md
  ("External routines"). }
{$H+}
interface

type
  { Opaque C handles: sqlite3 * and sqlite3_stmt *. }
  PSQLite3 = pointer;
  PSQLite3Stmt = pointer;

const
  SQLITE_OK = 0;             { successful result }
  SQLITE_ROW = 100;          { sqlite3_step: another row is ready }
  SQLITE_DONE = 101;         { sqlite3_step: the statement has finished }

  SQLITE_OPEN_READONLY = 1;
  SQLITE_OPEN_READWRITE = 2;
  SQLITE_OPEN_CREATE = 4;

  SQLITE_INTEGER = 1;        { sqlite3_column_type }
  SQLITE_FLOAT = 2;
  SQLITE_TEXT = 3;
  SQLITE_BLOB = 4;
  SQLITE_NULL = 5;

function sqlite3_libversion: pchar; cdecl; external 'sqlite3';
function sqlite3_open_v2(filename: pchar; var db: PSQLite3; flags: longint;
                         vfs: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_close(db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_errmsg(db: PSQLite3): pchar; cdecl; external 'sqlite3';
function sqlite3_exec(db: PSQLite3; sql: pchar; callback: pointer; arg: pointer;
                      var errmsg: pchar): longint; cdecl; external 'sqlite3';
procedure sqlite3_free(p: pointer); cdecl; external 'sqlite3';

function sqlite3_prepare_v2(db: PSQLite3; sql: pchar; nByte: longint;
                            var stmt: PSQLite3Stmt; var tail: pchar): longint;
  cdecl; external 'sqlite3';
function sqlite3_step(stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
function sqlite3_finalize(stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';

function sqlite3_column_count(stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
function sqlite3_column_name(stmt: PSQLite3Stmt; n: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_type(stmt: PSQLite3Stmt; n: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_column_int64(stmt: PSQLite3Stmt; n: longint): int64;
  cdecl; external 'sqlite3';
function sqlite3_column_double(stmt: PSQLite3Stmt; n: longint): double;
  cdecl; external 'sqlite3';
function sqlite3_column_text(stmt: PSQLite3Stmt; n: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_blob(stmt: PSQLite3Stmt; n: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_bytes(stmt: PSQLite3Stmt; n: longint): longint;
  cdecl; external 'sqlite3';

function sqlite3_bind_int64(stmt: PSQLite3Stmt; n: longint; value: int64): longint;
  cdecl; external 'sqlite3';
function sqlite3_bind_double(stmt: PSQLite3Stmt; n: longint; value: double): longint;
  cdecl; external 'sqlite3';
function sqlite3_bind_text(stmt: PSQLite3Stmt; n: longint; text: pchar;
                           nByte: longint; destroy: pointer): longint;
  cdecl; external 'sqlite3';
function sqlite3_bind_null(stmt: PSQLite3Stmt; n: longint): longint;
  cdecl; external 'sqlite3';

function sqlite3_last_insert_rowid(db: PSQLite3): int64; cdecl; external 'sqlite3';
function sqlite3_changes(db: PSQLite3): longint; cdecl; external 'sqlite3';

implementation

end.
