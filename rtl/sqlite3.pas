{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
unit sqlite3;

{ SQLite 3, imported from the C API reference (sqlite.org/capi3ref.html,
  the API of sqlite3.h 3.54.1): the routines in the libsqlite3 that most
  systems carry, as `cdecl; external 'sqlite3'` declarations — naming the
  library after `external` links the program with -lsqlite3. The opaque
  handles (sqlite3 *, sqlite3_stmt *, ...) are `pointer` aliases, C `int` is
  `longint`, C `unsigned` is `cardinal`; the callback types users have to
  write against are here too (TSQLite3ExecCallback, TSQLite3ScalarCallback,
  the TSQLite3Module fields, the TSQLite3Vfs and io methods, ...).

  Callbacks. p3 routines reach C by their name (no @): a callback parameter
  takes the routine's bare name, and the routine is program-level or
  unit-level `cdecl`:
      function Authoriser(Data: pointer; action: longint; arg1, arg2,
        arg3, arg4: pchar): longint; cdecl;
      sqlite3_set_authorizer(db, Authoriser, nil);
  The signature each parameter wants is in its comment (or is one of the
  types named above; every parameter's type says it too). The null of each
  callback type is one of the SQLITE_NULL_ constants here (SQLITE_NULL_EXEC
  for exec, ...); a routine variable or a struct field (Module.xStep := nil)
  takes nil directly, where writing a bare nil into a parameter would not
  compile.

  Strings. The pchar parameters take a literal, and pchar(Q) takes a
  string variable — build the text in a string first: a pchar() cast of a
  string expression does not compile.

  Destructors. The trailing destructor parameter of the bind_ and result_
  text, blob and carray routines (and every xDestroy) is SQLITE_STATIC
  (keep the bytes as they are), SQLITE_TRANSIENT (have sqlite copy them)
  or a destructor by name: the C `void (*)(void*)` cast of -1 cannot be a
  Pascal constant, so SQLITE_TRANSIENT is the unit variable the
  initialization loads that word into.

  printf-like: sqlite3_mprintf, sqlite3_snprintf, sqlite3_str_appendf and
  sqlite3_log take their C formats; %s for pchars, %q, %Q and %w put SQL
  into the text too. sqlite3_config keeps C's varargs form (pass what the
  operation wants: numbers, pointers); sqlite3_config_log is the typed
  entry point of the LOG operation — varargs cannot carry a routine.

  Not imported: the session and changeset routines (extra libraries many
  builds leave out, on session.html), the preupdate hooks, snapshots and
  the R-tree geometry callbacks (SQLITE_ENABLE_PREUPDATE_HOOK /
  SQLITE_ENABLE_SNAPSHOT / SQLITE_ENABLE_RTREE builds only), the win32-only
  sqlite3_win32_set_directory, the va_list forms (sqlite3_vmprintf,
  sqlite3_vsnprintf, sqlite3_str_vappendf), the routines 3.54 dropped
  (trace, profile, soft_heap_limit, aggregate_count, expired,
  transfer_bindings, global_recover, thread_cleanup, memory_alarm —
  trace_v2 and a -1 to sqlite3_soft_heap_limit64 supersede the common
  ones) and sqlite3_normalized_sql (an SQLITE_ENABLE_NORMALIZE build
  only). SQLITE_VERSION and SQLITE_VERSION_NUMBER describe this import;
  what the program runs against comes from sqlite3_libversion and
  sqlite3_libversion_number. }

{$H+}

interface

type
  { The opaque C handles. }
  PSQLite3 = pointer;          { sqlite3 *: a database connection }
  PSQLite3Stmt = pointer;      { sqlite3_stmt *: a prepared statement }
  PSQLite3Value = pointer;     { sqlite3_value *: one typed value }
  PSQLite3Context = pointer;   { sqlite3_context *: a SQL function's call }
  PSQLite3Backup = pointer;    { sqlite3_backup *: an online backup }
  PSQLite3Blob = pointer;      { sqlite3_blob *: an open BLOB }
  PSQLite3Mutex = pointer;     { sqlite3_mutex * }
  PSQLite3Str = pointer;       { sqlite3_str *: a string accumulator }
  PSQLite3PCache = pointer;    { sqlite3_pcache *: a page cache }
  PSQLite3FileName = pchar;    { sqlite3_filename, a name with parameters }

  { Value pointers and pchar lists reach routines and callbacks as arrays. }
  PPChar = ^pchar;
  PPointer = ^pointer;
  PPSQLite3Value = ^PSQLite3Value;

  { Callback routine types, for the application's routines to declare. }
  { A scalar SQL function's xFunc (also a module's xFindFunction pxFunc). }
  TSQLite3ScalarCallback = procedure(Ctx: PSQLite3Context; NCol: longint;
                                     Argv: PPSQLite3Value); cdecl;
  { An aggregate's xStep (also a window function's xInverse). }
  TSQLite3StepCallback = procedure(Ctx: PSQLite3Context; NCol: longint;
                                   Argv: PPSQLite3Value); cdecl;
  { An aggregate's xFinal and a window function's xValue. }
  TSQLite3FinalCallback = procedure(Ctx: PSQLite3Context); cdecl;
  { The C sqlite3_destructor_type: every destructor parameter's type. }
  TSQLite3Destructor = procedure(AWhat: pointer); cdecl;
  { sqlite3_exec's row callback: nonzero ends the statement (SQLITE_ABORT). }
  TSQLite3ExecCallback = function(Data: pointer; NCol: longint;
    Values, Names: PPChar): longint; cdecl;
  { The busy handler: nonzero waits again. }
  TSQLite3BusyCallback = function(Data: pointer; Count: longint): longint; cdecl;
  { The authorizer: SQLITE_OK, SQLITE_DENY or SQLITE_IGNORE. }
  TSQLite3AuthorizerCallback = function(Data: pointer; Action: longint;
    Arg1, Arg2, Arg3, Arg4: pchar): longint; cdecl;
  { sqlite3_progress_handler's callback: nonzero interrupts. }
  TSQLite3ProgressCallback = function(Data: pointer): longint; cdecl;
  { sqlite3_trace_v2's callback. }
  TSQLite3TraceCallback = function(Mask: cardinal; Ctx, P1, P2: pointer): longint; cdecl;
  { The commit and rollback hooks; the commit hook's nonzero aborts. }
  TSQLite3CommitCallback = function(Data: pointer): longint; cdecl;
  TSQLite3RollbackCallback = procedure(Data: pointer); cdecl;
  { sqlite3_autovacuum_pages' callback: the pages it would free. }
  TSQLite3AutoVacuumCallback = function(Data: pointer; Db: pchar;
    Pages, FreePages, PageSize: cardinal): cardinal; cdecl;
  { The update hook: one action on one row of one table. }
  TSQLite3UpdateCallback = procedure(Data: pointer; Action: longint;
    Db, Table: pchar; RowId: int64); cdecl;
  { The WAL hook; nonzero turns checkpointing off. }
  TSQLite3WalCallback = function(Data: pointer; Db: PSQLite3; DbName: pchar;
    Pages: longint): longint; cdecl;
  { A collation's comparison and the collation_needed callback. }
  TSQLite3CollationCallback = function(Data: pointer; LenA: longint; A: pointer;
    LenB: longint; B: pointer): longint; cdecl;
  TSQLite3CollationNeededCallback = procedure(Data: pointer; Db: PSQLite3;
    Encoding: longint; Name: pchar); cdecl;
  { sqlite3_auto_extension's entry point. }
  TSQLite3AutoExtensionCallback = procedure; cdecl;
  { sqlite3_config's LOG callback (and sqlite3_log's own listener). }
  TSQLite3LogCallback = procedure(Data: pointer; Code: longint;
    Msg: pchar); cdecl;
  { sqlite3_unlock_notify's callback: the waiting connections, unblocked. }
  TSQLite3UnlockNotifyCallback = procedure(ArgV: PPointer; NArg: longint); cdecl;

  { --- Virtual tables: the module the program registers, the tables and
        cursors SQLite hands back, and what xBestIndex reads and writes. }

  PSQLite3Module = ^TSQLite3Module;
  PSQLite3Vtab = ^TSQLite3Vtab;
  PSQLite3VtabCursor = ^TSQLite3VtabCursor;
  PSQLite3IndexInfo = ^TSQLite3IndexInfo;
  PSQLite3IndexConstraint = ^TSQLite3IndexConstraint;
  PSQLite3IndexOrderBy = ^TSQLite3IndexOrderBy;
  PSQLite3IndexConstraintUsage = ^TSQLite3IndexConstraintUsage;

  TSQLite3ModuleConnect = function(Db: PSQLite3; Aux: pointer; ArgC: longint;
    ArgV: PPChar; var VTab: PSQLite3Vtab; var ErrMsg: pchar): longint; cdecl;
  TSQLite3ModuleBestIndex = function(VTab: PSQLite3Vtab;
    Info: PSQLite3IndexInfo): longint; cdecl;
  TSQLite3ModuleDisconnect = function(VTab: PSQLite3Vtab): longint; cdecl;
  TSQLite3ModuleOpen = function(VTab: PSQLite3Vtab;
    var Cursor: PSQLite3VtabCursor): longint; cdecl;
  TSQLite3ModuleClose = function(Cursor: PSQLite3VtabCursor): longint; cdecl;
  TSQLite3ModuleFilter = function(Cursor: PSQLite3VtabCursor; IdxNum: longint;
    IdxStr: pchar; ArgC: longint; ArgV: PPSQLite3Value): longint; cdecl;
  TSQLite3ModuleNext = function(Cursor: PSQLite3VtabCursor): longint; cdecl;
  TSQLite3ModuleColumn = function(Cursor: PSQLite3VtabCursor;
    Ctx: PSQLite3Context; NCol: longint): longint; cdecl;
  TSQLite3ModuleRowid = function(Cursor: PSQLite3VtabCursor;
    var RowId: int64): longint; cdecl;
  TSQLite3ModuleUpdate = function(VTab: PSQLite3Vtab; ArgC: longint;
    ArgV: PPSQLite3Value; var RowId: int64): longint; cdecl;
  TSQLite3ModuleSimple = function(VTab: PSQLite3Vtab): longint; cdecl;
  TSQLite3ModuleFindFunction = function(VTab: PSQLite3Vtab; NArg: longint;
    Name: pchar; var PXFunc: TSQLite3ScalarCallback;
    var PPArg: pointer): longint; cdecl;
  TSQLite3ModuleRename = function(VTab: PSQLite3Vtab; ZNew: pchar): longint; cdecl;
  TSQLite3ModuleSavepoint = function(VTab: PSQLite3Vtab;
    N: longint): longint; cdecl;
  TSQLite3ModuleShadowName = function(Name: pchar): longint; cdecl;
  TSQLite3ModuleIntegrity = function(VTab: PSQLite3Vtab; Schema, TabName: pchar;
    Flags: longint; var PZErr: pchar): longint; cdecl;

  { iVersion says how deep (4 now): 1 through xRename, 2 through
    xRollbackTo, 3 through xShadowName, 4 through xIntegrity. }
  TSQLite3Module = record
    IVersion: longint;
    XCreate: TSQLite3ModuleConnect;      { nil: eponymous, read-only }
    XConnect: TSQLite3ModuleConnect;
    XBestIndex: TSQLite3ModuleBestIndex;
    XDisconnect: TSQLite3ModuleDisconnect;
    XDestroy: TSQLite3ModuleDisconnect;
    XOpen: TSQLite3ModuleOpen;
    XClose: TSQLite3ModuleClose;
    XFilter: TSQLite3ModuleFilter;
    XNext: TSQLite3ModuleNext;
    XEof: TSQLite3ModuleNext;
    XColumn: TSQLite3ModuleColumn;
    XRowid: TSQLite3ModuleRowid;
    XUpdate: TSQLite3ModuleUpdate;
    XBegin: TSQLite3ModuleSimple;
    XSync: TSQLite3ModuleSimple;
    XCommit: TSQLite3ModuleSimple;
    XRollback: TSQLite3ModuleSimple;
    XFindFunction: TSQLite3ModuleFindFunction;
    XRename: TSQLite3ModuleRename;
    XSavepoint: TSQLite3ModuleSavepoint;
    XRelease: TSQLite3ModuleSavepoint;
    XRollbackTo: TSQLite3ModuleSavepoint;
    XShadowName: TSQLite3ModuleShadowName;
    XIntegrity: TSQLite3ModuleIntegrity;
  end;

  TSQLite3IndexConstraint = record
    IColumn: longint;       { the constrained column, -1 for ROWID }
    Op: byte;               { one of the SQLITE_INDEX_CONSTRAINT_ codes }
    Usable: byte;           { nonzero if the right side can be evaluated }
    ITermOffset: longint;   { internal; xBestIndex ignores it }
  end;
  TSQLite3IndexOrderBy = record
    IColumn: longint;
    Desc: byte;             { nonzero for DESC }
  end;
  TSQLite3IndexConstraintUsage = record
    ArgvIndex: longint;     { >0: the constraint becomes xFilter's argv }
    Omit: byte;             { nonzero: the virtual table takes it on }
  end;
  TSQLite3IndexInfo = record
    { Inputs }
    NConstraint: longint;
    AConstraint: PSQLite3IndexConstraint;
    NOrderBy: longint;
    AOrderBy: PSQLite3IndexOrderBy;
    { Outputs }
    AConstraintUsage: PSQLite3IndexConstraintUsage;
    IdxNum: longint;        { to xFilter, with IdxStr }
    IdxStr: pchar;          { freed by sqlite3_free if NeedToFreeIdxStr }
    NeedToFreeIdxStr: longint;
    OrderByConsumed: longint;
    EstimatedCost: double;  { ~rows of a linear scan; 1.0 for a unique index }
    EstimatedRows: int64;
    IdxFlags: longint;      { a mask of SQLITE_INDEX_SCAN_ codes }
    ColUsed: qword;         { bit i: column i is read; bit 63: any of 63+ }
  end;

  TSQLite3Vtab = record
    PModule: PSQLite3Module;
    NRef: longint;          { the cursors now open on this table }
    ZErrMsg: pchar;         { from sqlite3_mprintf: freed and zeroed after }
  end;
  TSQLite3VtabCursor = record
    PVtab: PSQLite3Vtab;
  end;

  { --- The VFS layer: sqlite3_file (its first field points at its
        methods), the methods, and the file-system object itself. }

  PSQLite3File = ^TSQLite3File;
  PSQLite3IoMethods = ^TSQLite3IoMethods;
  PSQLite3Vfs = ^TSQLite3Vfs;

  TSQLite3FileSimple = function(F: PSQLite3File): longint; cdecl;
  TSQLite3IoBytes = function(F: PSQLite3File; Buf: pointer; IAmt: longint;
    IOfst: int64): longint; cdecl;    { xRead and xWrite }
  TSQLite3IoTruncate = function(F: PSQLite3File; Size: int64): longint; cdecl;
  TSQLite3IoSync = function(F: PSQLite3File; Flags: longint): longint; cdecl;
  TSQLite3IoFileSize = function(F: PSQLite3File; var PSize: int64): longint; cdecl;
  TSQLite3IoLock = function(F: PSQLite3File; Lock: longint): longint; cdecl;
  TSQLite3IoCheckReservedLock = function(F: PSQLite3File;
    var PResOut: longint): longint; cdecl;
  TSQLite3IoFileControl = function(F: PSQLite3File; Op: longint;
    PArg: pointer): longint; cdecl;
  TSQLite3IoShmMap = function(F: PSQLite3File; IPg, Pgsz, B: longint;
    Volatile: pointer): longint; cdecl;   { a pointer out goes in PArg }
  TSQLite3IoShmLock = function(F: PSQLite3File; Offset, N,
    Flags: longint): longint; cdecl;
  TSQLite3IoShmBarrier = procedure(F: PSQLite3File); cdecl;
  TSQLite3IoFetch = function(F: PSQLite3File; IOfst: int64; IAmt: longint;
    PP: pointer): longint; cdecl;    { xFetch; xUnfetch trades IAmt for PP }
  TSQLite3IoUnfetch = function(F: PSQLite3File; IOfst: int64;
    P: pointer): longint; cdecl;

  { Methods above the version line belong to io_methods versions 1, 2 and 3. }
  TSQLite3IoMethods = record
    IVersion: longint;
    XClose: TSQLite3FileSimple;
    XRead: TSQLite3IoBytes;
    XWrite: TSQLite3IoBytes;
    XTruncate: TSQLite3IoTruncate;
    XSync: TSQLite3IoSync;
    XFileSize: TSQLite3IoFileSize;
    XLock: TSQLite3IoLock;
    XUnlock: TSQLite3IoLock;
    XCheckReservedLock: TSQLite3IoCheckReservedLock;
    XFileControl: TSQLite3IoFileControl;
    XSectorSize: TSQLite3FileSimple;
    XDeviceCharacteristics: TSQLite3FileSimple;
    XShmMap: TSQLite3IoShmMap;           { io_methods version 2 }
    XShmLock: TSQLite3IoShmLock;
    XShmBarrier: TSQLite3IoShmBarrier;
    XShmUnmap: TSQLite3IoLock;
    XFetch: TSQLite3IoFetch;             { io_methods version 3 }
    XUnfetch: TSQLite3IoUnfetch;
  end;

  TSQLite3File = record
    PMethods: PSQLite3IoMethods;         { set by xOpen; xClose may still run }
  end;

  TSQLite3Syscall = procedure; cdecl;   { the C sqlite3_syscall_ptr }
  TSQLite3VfsOpen = function(Vfs: PSQLite3Vfs; Name: PSQLite3FileName;
    F: PSQLite3File; Flags: longint; var POutFlags: longint): longint; cdecl;
  TSQLite3VfsDelete = function(Vfs: PSQLite3Vfs; Name: pchar;
    SyncDir: longint): longint; cdecl;
  TSQLite3VfsAccess = function(Vfs: PSQLite3Vfs; Name: pchar; Flags: longint;
    var PResOut: longint): longint; cdecl;
  TSQLite3VfsFullPathname = function(Vfs: PSQLite3Vfs; Name: pchar;
    NOut: longint; ZOut: pchar): longint; cdecl;
  TSQLite3VfsDlOpen = function(Vfs: PSQLite3Vfs; Name: pchar): pointer; cdecl;
  TSQLite3VfsDlError = procedure(Vfs: PSQLite3Vfs; NByte: longint;
    ZErrMsg: pchar); cdecl;
  TSQLite3VfsDlSym = function(Vfs: PSQLite3Vfs; Handle: pointer;
    Symbol: pchar): pointer; cdecl;
  TSQLite3VfsDlClose = procedure(Vfs: PSQLite3Vfs; Handle: pointer); cdecl;
  TSQLite3VfsRandomness = function(Vfs: PSQLite3Vfs; NByte: longint;
    ZOut: pchar): longint; cdecl;
  TSQLite3VfsSleep = function(Vfs: PSQLite3Vfs; Microseconds: longint): longint; cdecl;
  TSQLite3VfsCurrentTime = function(Vfs: PSQLite3Vfs;
    var PNow: double): longint; cdecl;  { xCurrentTime, julianday days }
  TSQLite3VfsGetLastError = function(Vfs: PSQLite3Vfs; N: longint;
    Buf: pchar): longint; cdecl;
  TSQLite3VfsCurrentTimeInt64 = function(Vfs: PSQLite3Vfs;
    var PNow: int64): longint; cdecl;   { vfs version 2 }
  TSQLite3VfsSetSystemCall = function(Vfs: PSQLite3Vfs; Name: pchar;
    Call: TSQLite3Syscall): longint; cdecl;  { vfs version 3 }
  { The C xGetSystemCall answers a routine pointer, not to call through. }
  TSQLite3VfsGetSystemCall = function(Vfs: PSQLite3Vfs;
    Name: pchar): pointer; cdecl;
  TSQLite3VfsNextSystemCall = function(Vfs: PSQLite3Vfs; Name: pchar): pchar; cdecl;

  TSQLite3Vfs = record
    IVersion: longint;      { 3 now }
    SzOsFile: longint;      { the size of the TSQLite3File subclass }
    MxPathname: longint;
    PNext: PSQLite3Vfs;
    ZName: pchar;
    PAppData: pointer;
    XOpen: TSQLite3VfsOpen;
    XDelete: TSQLite3VfsDelete;
    XAccess: TSQLite3VfsAccess;
    XFullPathname: TSQLite3VfsFullPathname;
    XDlOpen: TSQLite3VfsDlOpen;
    XDlError: TSQLite3VfsDlError;
    XDlSym: TSQLite3VfsDlSym;
    XDlClose: TSQLite3VfsDlClose;
    XRandomness: TSQLite3VfsRandomness;
    XSleep: TSQLite3VfsSleep;
    XCurrentTime: TSQLite3VfsCurrentTime;
    XGetLastError: TSQLite3VfsGetLastError;
    XCurrentTimeInt64: TSQLite3VfsCurrentTimeInt64;   { vfs version 2 }
    XSetSystemCall: TSQLite3VfsSetSystemCall;         { vfs version 3 }
    XGetSystemCall: TSQLite3VfsGetSystemCall;
    XNextSystemCall: TSQLite3VfsNextSystemCall;
  end;

  { --- The memory allocator, mutexes and page cache sqlite3_config can
        replace (else runtime/sys.c's threads carry them). }

  TSQLite3MemMalloc = function(NBytes: longint): pointer; cdecl;
  TSQLite3MemFree = procedure(P: pointer); cdecl;
  TSQLite3MemRealloc = function(P: pointer; NBytes: longint): pointer; cdecl;
  TSQLite3MemSize = function(P: pointer): longint; cdecl;   { xSize }
  TSQLite3MemRoundup = function(NBytes: longint): longint; cdecl;
  TSQLite3MemInit = function(PAppData: pointer): longint; cdecl;
  TSQLite3MemShutdown = procedure(PAppData: pointer); cdecl;

  TSQLite3MemMethods = record
    XMalloc: TSQLite3MemMalloc;
    XFree: TSQLite3MemFree;
    XRealloc: TSQLite3MemRealloc;
    XSize: TSQLite3MemSize;
    XRoundup: TSQLite3MemRoundup;
    XInit: TSQLite3MemInit;
    XShutdown: TSQLite3MemShutdown;
    PAppData: pointer;
  end;

  TSQLite3MutexAlloc = function(Which: longint): PSQLite3Mutex; cdecl;
  TSQLite3MutexEnter = procedure(Mutex: PSQLite3Mutex); cdecl;
  TSQLite3MutexTry = function(Mutex: PSQLite3Mutex): longint; cdecl;
  TSQLite3MutexHeldP = function(Mutex: PSQLite3Mutex): longint; cdecl;

  TSQLite3MutexMethods = record
    XMutexInit: function: longint; cdecl;
    XMutexEnd: procedure; cdecl;
    XMutexAlloc: TSQLite3MutexAlloc;
    XMutexFree: procedure(Mutex: PSQLite3Mutex); cdecl;
    XMutexEnter: TSQLite3MutexEnter;
    XMutexTry: TSQLite3MutexTry;
    XMutexLeave: TSQLite3MutexEnter;
    XMutexHeld: TSQLite3MutexHeldP;
    XMutexNotheld: TSQLite3MutexHeldP;
  end;

  TSQLite3PCachePage = record
    PBuf: pointer;                { the page's bytes }
    PExtra: pointer;              { the page's extra bytes }
  end;

  PSQLite3PCachePage = ^TSQLite3PCachePage;

  TSQLite3CacheInit = function(PArg: pointer): longint; cdecl;
  TSQLite3CacheShutdown = procedure(PArg: pointer); cdecl;
  TSQLite3CacheCreate = function(SzPage, SzExtra,
    BPurgeable: longint): PSQLite3PCache; cdecl;
  TSQLite3CacheCachesize = procedure(PCache: PSQLite3PCache; NCachesize: longint); cdecl;
  TSQLite3CachePagecount = function(PCache: PSQLite3PCache): longint; cdecl;
  TSQLite3CacheFetch = function(PCache: PSQLite3PCache; Key: cardinal;
    CreateFlag: longint): PSQLite3PCachePage; cdecl;
  TSQLite3CacheUnpin = procedure(PCache: PSQLite3PCache;
    Page: PSQLite3PCachePage; Discard: longint); cdecl;
  TSQLite3CacheRekey = procedure(PCache: PSQLite3PCache;
    Page: PSQLite3PCachePage; OldKey, NewKey: cardinal); cdecl;
  TSQLite3CacheTruncate = procedure(PCache: PSQLite3PCache; ILimit: cardinal); cdecl;
  TSQLite3CacheDestroy = procedure(PCache: PSQLite3PCache); cdecl;

  TSQLite3PCacheMethods2 = record
    IVersion: longint;
    PArg: pointer;
    XInit: TSQLite3CacheInit;
    XShutdown: TSQLite3CacheShutdown;
    XCreate: TSQLite3CacheCreate;
    XCachesize: TSQLite3CacheCachesize;
    XPagecount: TSQLite3CachePagecount;
    XFetch: TSQLite3CacheFetch;
    XUnpin: TSQLite3CacheUnpin;
    XRekey: TSQLite3CacheRekey;
    XTruncate: TSQLite3CacheTruncate;
    XDestroy: TSQLite3CacheDestroy;
    XShrink: procedure(PCache: PSQLite3PCache); cdecl;
  end;

  { The obsolete pcache_methods, for sqlite3_config(SQLITE_CONFIG_PCACHE). }
  TSQLite3CacheCreate1 = function(SzPage, BPurgeable: longint): PSQLite3PCache; cdecl;
  TSQLite3CacheFetch1 = function(PCache: PSQLite3PCache; Key: cardinal;
    CreateFlag: longint): pointer; cdecl;

  TSQLite3PCacheMethods = record
    PArg: pointer;
    XInit: TSQLite3CacheInit;
    XShutdown: TSQLite3CacheShutdown;
    XCreate: TSQLite3CacheCreate1;
    XCachesize: TSQLite3CacheCachesize;
    XPagecount: TSQLite3CachePagecount;
    XFetch: TSQLite3CacheFetch1;
    XUnpin: procedure(PCache: PSQLite3PCache; Page: pointer;
      Discard: longint); cdecl;
    XRekey: procedure(PCache: PSQLite3PCache; Page: pointer;
      OldKey, NewKey: cardinal); cdecl;
    XTruncate: TSQLite3CacheTruncate;
    XDestroy: TSQLite3CacheDestroy;
  end;

const
  { The API this unit imports; sqlite3_libversion tells the library's own. }
  SQLITE_VERSION = '3.54.1';
  SQLITE_VERSION_NUMBER = 3054001;
  SQLITE_SOURCE_ID = '2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76';

  { Result codes, and the extended codes after their primaries. }
  SQLITE_OK = 0;
  SQLITE_ERROR = 1;
  SQLITE_INTERNAL = 2;          { unused, internal only }
  SQLITE_PERM = 3;
  SQLITE_ABORT = 4;
  SQLITE_BUSY = 5;
  SQLITE_LOCKED = 6;
  SQLITE_NOMEM = 7;
  SQLITE_READONLY = 8;
  SQLITE_INTERRUPT = 9;
  SQLITE_IOERR = 10;
  SQLITE_CORRUPT = 11;
  SQLITE_NOTFOUND = 12;
  SQLITE_FULL = 13;
  SQLITE_CANTOPEN = 14;
  SQLITE_PROTOCOL = 15;
  SQLITE_EMPTY = 16;            { unused }
  SQLITE_SCHEMA = 17;
  SQLITE_TOOBIG = 18;
  SQLITE_CONSTRAINT = 19;
  SQLITE_MISMATCH = 20;
  SQLITE_MISUSE = 21;
  SQLITE_NOLFS = 22;
  SQLITE_AUTH = 23;
  SQLITE_FORMAT = 24;           { unused }
  SQLITE_RANGE = 25;
  SQLITE_NOTADB = 26;
  SQLITE_NOTICE = 27;
  SQLITE_WARNING = 28;
  SQLITE_ROW = 100;
  SQLITE_DONE = 101;

  SQLITE_ERROR_MISSING_COLLSEQ = 257;
  SQLITE_ERROR_RETRY = 513;
  SQLITE_ERROR_SNAPSHOT = 769;
  SQLITE_ERROR_RESERVESIZE = 1025;
  SQLITE_ERROR_KEY = 1281;
  SQLITE_ERROR_UNABLE = 1537;
  SQLITE_IOERR_READ = 266;
  SQLITE_IOERR_SHORT_READ = 522;
  SQLITE_IOERR_WRITE = 778;
  SQLITE_IOERR_FSYNC = 1034;
  SQLITE_IOERR_DIR_FSYNC = 1290;
  SQLITE_IOERR_TRUNCATE = 1546;
  SQLITE_IOERR_FSTAT = 1802;
  SQLITE_IOERR_UNLOCK = 2058;
  SQLITE_IOERR_RDLOCK = 2314;
  SQLITE_IOERR_DELETE = 2570;
  SQLITE_IOERR_BLOCKED = 2826;
  SQLITE_IOERR_NOMEM = 3082;
  SQLITE_IOERR_ACCESS = 3338;
  SQLITE_IOERR_CHECKRESERVEDLOCK = 3594;
  SQLITE_IOERR_LOCK = 3850;
  SQLITE_IOERR_CLOSE = 4106;
  SQLITE_IOERR_DIR_CLOSE = 4362;
  SQLITE_IOERR_SHMOPEN = 4618;
  SQLITE_IOERR_SHMSIZE = 4874;
  SQLITE_IOERR_SHMLOCK = 5130;
  SQLITE_IOERR_SHMMAP = 5386;
  SQLITE_IOERR_SEEK = 5642;
  SQLITE_IOERR_DELETE_NOENT = 5898;
  SQLITE_IOERR_MMAP = 6154;
  SQLITE_IOERR_GETTEMPPATH = 6410;
  SQLITE_IOERR_CONVPATH = 6666;
  SQLITE_IOERR_VNODE = 6922;
  SQLITE_IOERR_AUTH = 7178;
  SQLITE_IOERR_BEGIN_ATOMIC = 7434;
  SQLITE_IOERR_COMMIT_ATOMIC = 7690;
  SQLITE_IOERR_ROLLBACK_ATOMIC = 7946;
  SQLITE_IOERR_DATA = 8202;
  SQLITE_IOERR_CORRUPTFS = 8458;
  SQLITE_IOERR_IN_PAGE = 8714;
  SQLITE_IOERR_BADKEY = 8970;
  SQLITE_IOERR_CODEC = 9226;
  SQLITE_LOCKED_SHAREDCACHE = 262;
  SQLITE_LOCKED_VTAB = 518;
  SQLITE_BUSY_RECOVERY = 261;
  SQLITE_BUSY_SNAPSHOT = 517;
  SQLITE_BUSY_TIMEOUT = 773;
  SQLITE_CANTOPEN_NOTEMPDIR = 270;
  SQLITE_CANTOPEN_ISDIR = 526;
  SQLITE_CANTOPEN_FULLPATH = 782;
  SQLITE_CANTOPEN_CONVPATH = 1038;
  SQLITE_CANTOPEN_DIRTYWAL = 1294;
  SQLITE_CANTOPEN_SYMLINK = 1550;
  SQLITE_CORRUPT_VTAB = 267;
  SQLITE_CORRUPT_SEQUENCE = 523;
  SQLITE_CORRUPT_INDEX = 779;
  SQLITE_READONLY_RECOVERY = 264;
  SQLITE_READONLY_CANTLOCK = 520;
  SQLITE_READONLY_ROLLBACK = 776;
  SQLITE_READONLY_DBMOVED = 1032;
  SQLITE_READONLY_CANTINIT = 1288;
  SQLITE_READONLY_DIRECTORY = 1544;
  SQLITE_ABORT_ROLLBACK = 516;
  SQLITE_CONSTRAINT_CHECK = 275;
  SQLITE_CONSTRAINT_COMMITHOOK = 531;
  SQLITE_CONSTRAINT_FOREIGNKEY = 787;
  SQLITE_CONSTRAINT_FUNCTION = 1043;
  SQLITE_CONSTRAINT_NOTNULL = 1299;
  SQLITE_CONSTRAINT_PRIMARYKEY = 1555;
  SQLITE_CONSTRAINT_TRIGGER = 1811;
  SQLITE_CONSTRAINT_UNIQUE = 2067;
  SQLITE_CONSTRAINT_VTAB = 2323;
  SQLITE_CONSTRAINT_ROWID = 2579;
  SQLITE_CONSTRAINT_PINNED = 2835;
  SQLITE_CONSTRAINT_DATATYPE = 3091;
  SQLITE_NOTICE_RECOVER_WAL = 283;
  SQLITE_NOTICE_RECOVER_ROLLBACK = 539;
  SQLITE_NOTICE_RBU = 795;
  SQLITE_WARNING_AUTOINDEX = 285;
  SQLITE_AUTH_USER = 279;
  SQLITE_OK_LOAD_PERMANENTLY = 256;
  SQLITE_OK_SYMLINK = 512;

  { sqlite3_open_v2's flags; journal and database file kinds for a VFS's
    xOpen. OR SQLITE_OPEN_URI, OPEN_MEMORY, OPEN_SHAREDCACHE or
    OPEN_PRIVATECACHE in with the access. }
  SQLITE_OPEN_READONLY = 1;
  SQLITE_OPEN_READWRITE = 2;
  SQLITE_OPEN_CREATE = 4;
  SQLITE_OPEN_DELETEONCLOSE = 8;
  SQLITE_OPEN_EXCLUSIVE = 16;
  SQLITE_OPEN_AUTOPROXY = 32;
  SQLITE_OPEN_URI = 64;
  SQLITE_OPEN_MEMORY = 128;
  SQLITE_OPEN_MAIN_DB = 256;
  SQLITE_OPEN_TEMP_DB = 512;
  SQLITE_OPEN_TRANSIENT_DB = 1024;
  SQLITE_OPEN_MAIN_JOURNAL = 2048;
  SQLITE_OPEN_TEMP_JOURNAL = 4096;
  SQLITE_OPEN_SUBJOURNAL = 8192;
  SQLITE_OPEN_SUPER_JOURNAL = 16384;
  SQLITE_OPEN_NOMUTEX = 32768;
  SQLITE_OPEN_FULLMUTEX = 65536;
  SQLITE_OPEN_SHAREDCACHE = 131072;
  SQLITE_OPEN_PRIVATECACHE = 262144;
  SQLITE_OPEN_WAL = 524288;
  SQLITE_OPEN_NOFOLLOW = 16777216;
  SQLITE_OPEN_EXRESCODE = 33554432;

  { The five types of value: sqlite3_column_type and sqlite3_value_type. }
  SQLITE_INTEGER = 1;
  SQLITE_FLOAT = 2;
  SQLITE_TEXT = 3;
  SQLITE_BLOB = 4;
  SQLITE_NULL = 5;

  { Text encodings, for create_function and the *64 routines. }
  SQLITE_UTF8 = 1;
  SQLITE_UTF16LE = 2;
  SQLITE_UTF16BE = 3;
  SQLITE_UTF16 = 4;
  SQLITE_ANY = 5;
  SQLITE_UTF16_ALIGNED = 8;
  SQLITE_UTF8_ZT = 16;

  { create_function's flags, ORed into the encoding. }
  SQLITE_DETERMINISTIC = $800;
  SQLITE_DIRECTONLY = $80000;
  SQLITE_SUBTYPE = $100000;
  SQLITE_INNOCUOUS = $200000;
  SQLITE_RESULT_SUBTYPE = $1000000;
  SQLITE_SELFORDER1 = $2000000;

  { sqlite3_stmt_scanstatus v2's flags and request codes; the ones that
    read a pchar or a longint say so in the routine's comment. }
  SQLITE_SCANSTAT_NLOOP = 0;
  SQLITE_SCANSTAT_NVISIT = 1;
  SQLITE_SCANSTAT_EST = 2;
  SQLITE_SCANSTAT_NAME = 3;
  SQLITE_SCANSTAT_EXPLAIN = 4;
  SQLITE_SCANSTAT_SELECTID = 5;
  SQLITE_SCANSTAT_PARENTID = 6;
  SQLITE_SCANSTAT_NCYCLE = 7;
  SQLITE_SCANSTAT_COMPLEX = 1;

  { sqlite3_wal_checkpoint_v2's modes. }
  SQLITE_CHECKPOINT_NOOP = -1;
  SQLITE_CHECKPOINT_PASSIVE = 0;
  SQLITE_CHECKPOINT_FULL = 1;
  SQLITE_CHECKPOINT_RESTART = 2;
  SQLITE_CHECKPOINT_TRUNCATE = 3;

  { sqlite3_trace_v2's callback topics. }
  SQLITE_TRACE_STMT = $1;
  SQLITE_TRACE_PROFILE = $2;
  SQLITE_TRACE_ROW = $4;
  SQLITE_TRACE_CLOSE = $8;

  { sqlite3_prepare_v3's flags. }
  SQLITE_PREPARE_PERSISTENT = $1;
  SQLITE_PREPARE_NORMALIZE = $2;
  SQLITE_PREPARE_NO_VTAB = $4;
  SQLITE_PREPARE_DONT_LOG = $10;
  SQLITE_PREPARE_FROM_DDL = $20;

  { Runtime limits, for sqlite3_limit. }
  SQLITE_LIMIT_LENGTH = 0;
  SQLITE_LIMIT_SQL_LENGTH = 1;
  SQLITE_LIMIT_COLUMN = 2;
  SQLITE_LIMIT_EXPR_DEPTH = 3;
  SQLITE_LIMIT_COMPOUND_SELECT = 4;
  SQLITE_LIMIT_VDBE_OP = 5;
  SQLITE_LIMIT_FUNCTION_ARG = 6;
  SQLITE_LIMIT_ATTACHED = 7;
  SQLITE_LIMIT_LIKE_PATTERN_LENGTH = 8;
  SQLITE_LIMIT_VARIABLE_NUMBER = 9;
  SQLITE_LIMIT_TRIGGER_DEPTH = 10;
  SQLITE_LIMIT_WORKER_THREADS = 11;
  SQLITE_LIMIT_PARSER_DEPTH = 12;
  SQLITE_LIMIT_SCHEMA = 13;
  SQLITE_LIMIT_TRIGGER_STEPS = 14;

  { sqlite3_db_config's operations: set the onoff ones as
    (db, CODE, onoff, nil) — passing a pvar: longint and nil's pointer —
    and the three named ones their own shapes. }
  SQLITE_DBCONFIG_MAINDBNAME = 1000;
  SQLITE_DBCONFIG_LOOKASIDE = 1001;
  SQLITE_DBCONFIG_ENABLE_FKEY = 1002;
  SQLITE_DBCONFIG_ENABLE_TRIGGER = 1003;
  SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER = 1004;
  SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION = 1005;
  SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE = 1006;
  SQLITE_DBCONFIG_ENABLE_QPSG = 1007;
  SQLITE_DBCONFIG_TRIGGER_EQP = 1008;
  SQLITE_DBCONFIG_RESET_DATABASE = 1009;
  SQLITE_DBCONFIG_DEFENSIVE = 1010;
  SQLITE_DBCONFIG_WRITABLE_SCHEMA = 1011;
  SQLITE_DBCONFIG_LEGACY_ALTER_TABLE = 1012;
  SQLITE_DBCONFIG_DQS_DML = 1013;
  SQLITE_DBCONFIG_DQS_DDL = 1014;
  SQLITE_DBCONFIG_ENABLE_VIEW = 1015;
  SQLITE_DBCONFIG_LEGACY_FILE_FORMAT = 1016;
  SQLITE_DBCONFIG_TRUSTED_SCHEMA = 1017;
  SQLITE_DBCONFIG_STMT_SCANSTATUS = 1018;
  SQLITE_DBCONFIG_REVERSE_SCANORDER = 1019;
  SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE = 1020;
  SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE = 1021;
  SQLITE_DBCONFIG_ENABLE_COMMENTS = 1022;
  SQLITE_DBCONFIG_FP_DIGITS = 1023;
  SQLITE_DBCONFIG_MAX = 1023;

  { sqlite3_status's operations. }
  SQLITE_STATUS_MEMORY_USED = 0;
  SQLITE_STATUS_PAGECACHE_USED = 1;
  SQLITE_STATUS_PAGECACHE_OVERFLOW = 2;
  SQLITE_STATUS_SCRATCH_USED = 3;
  SQLITE_STATUS_SCRATCH_OVERFLOW = 4;
  SQLITE_STATUS_MALLOC_SIZE = 5;
  SQLITE_STATUS_PARSER_STACK = 6;
  SQLITE_STATUS_PAGECACHE_SIZE = 7;
  SQLITE_STATUS_SCRATCH_SIZE = 8;
  SQLITE_STATUS_MALLOC_COUNT = 9;

  { sqlite3_db_status's operations. }
  SQLITE_DBSTATUS_LOOKASIDE_USED = 0;
  SQLITE_DBSTATUS_CACHE_USED = 1;
  SQLITE_DBSTATUS_SCHEMA_USED = 2;
  SQLITE_DBSTATUS_STMT_USED = 3;
  SQLITE_DBSTATUS_LOOKASIDE_HIT = 4;
  SQLITE_DBSTATUS_LOOKASIDE_MISS_SIZE = 5;
  SQLITE_DBSTATUS_LOOKASIDE_MISS_FULL = 6;
  SQLITE_DBSTATUS_CACHE_HIT = 7;
  SQLITE_DBSTATUS_CACHE_MISS = 8;
  SQLITE_DBSTATUS_CACHE_WRITE = 9;
  SQLITE_DBSTATUS_DEFERRED_FKS = 10;
  SQLITE_DBSTATUS_CACHE_USED_SHARED = 11;
  SQLITE_DBSTATUS_CACHE_SPILL = 12;
  SQLITE_DBSTATUS_TEMPBUF_SPILL = 13;
  SQLITE_DBSTATUS_MAX = 13;

  { sqlite3_stmt_status's counters. }
  SQLITE_STMTSTATUS_FULLSCAN_STEP = 1;
  SQLITE_STMTSTATUS_SORT = 2;
  SQLITE_STMTSTATUS_AUTOINDEX = 3;
  SQLITE_STMTSTATUS_VM_STEP = 4;
  SQLITE_STMTSTATUS_REPREPARE = 5;
  SQLITE_STMTSTATUS_RUN = 6;
  SQLITE_STMTSTATUS_FILTER_MISS = 7;
  SQLITE_STMTSTATUS_FILTER_HIT = 8;
  SQLITE_STMTSTATUS_MEMUSED = 99;

  { sqlite3_config's operations. The ones that take a struct of this unit
    pass a pointer to it (MALLOC, GETMALLOC, MUTEX, GETMUTEX, PCACHE,
    GETPCACHE, PCACHE2, GETPCACHE2); LOG takes Data and a
    procedure(data: pointer; code: longint; msg: pchar); cdecl; SQLLOG
    data and two callbacks of its own kind; HEAP five numbers, SCRATCH,
    PAGECACHE, LOOKASIDE what the C reference writes (a pchar passed to a
    varargs C routine comes through as its bytes' pointer). Call them
    before sqlite3_initialize, except LOG and PCACHE_HDRSZ. }
  SQLITE_CONFIG_SINGLETHREAD = 1;
  SQLITE_CONFIG_MULTITHREAD = 2;
  SQLITE_CONFIG_SERIALIZED = 3;
  SQLITE_CONFIG_MALLOC = 4;
  SQLITE_CONFIG_GETMALLOC = 5;
  SQLITE_CONFIG_SCRATCH = 6;
  SQLITE_CONFIG_PAGECACHE = 7;
  SQLITE_CONFIG_HEAP = 8;
  SQLITE_CONFIG_MEMSTATUS = 9;
  SQLITE_CONFIG_MUTEX = 10;
  SQLITE_CONFIG_GETMUTEX = 11;
  SQLITE_CONFIG_LOOKASIDE = 13;
  SQLITE_CONFIG_PCACHE = 14;
  SQLITE_CONFIG_GETPCACHE = 15;
  SQLITE_CONFIG_LOG = 16;
  SQLITE_CONFIG_URI = 17;
  SQLITE_CONFIG_PCACHE2 = 18;
  SQLITE_CONFIG_GETPCACHE2 = 19;
  SQLITE_CONFIG_COVERING_INDEX_SCAN = 20;
  SQLITE_CONFIG_SQLLOG = 21;
  SQLITE_CONFIG_MMAP_SIZE = 22;
  SQLITE_CONFIG_WIN32_HEAPSIZE = 23;
  SQLITE_CONFIG_PCACHE_HDRSZ = 24;
  SQLITE_CONFIG_PMASZ = 25;
  SQLITE_CONFIG_STMTJRNL_SPILL = 26;
  SQLITE_CONFIG_SMALL_MALLOC = 27;
  SQLITE_CONFIG_SORTERREF_SIZE = 28;
  SQLITE_CONFIG_MEMDB_MAXSIZE = 29;
  SQLITE_CONFIG_ROWID_IN_VIEW = 30;

  { xFileControl and sqlite3_file_control's opcodes. }
  SQLITE_FCNTL_LOCKSTATE = 1;
  SQLITE_FCNTL_GET_LOCKPROXYFILE = 2;
  SQLITE_FCNTL_SET_LOCKPROXYFILE = 3;
  SQLITE_FCNTL_LAST_ERRNO = 4;
  SQLITE_FCNTL_SIZE_HINT = 5;
  SQLITE_FCNTL_CHUNK_SIZE = 6;
  SQLITE_FCNTL_FILE_POINTER = 7;
  SQLITE_FCNTL_SYNC_OMITTED = 8;
  SQLITE_FCNTL_WIN32_AV_RETRY = 9;
  SQLITE_FCNTL_PERSIST_WAL = 10;
  SQLITE_FCNTL_OVERWRITE = 11;
  SQLITE_FCNTL_VFSNAME = 12;
  SQLITE_FCNTL_POWERSAFE_OVERWRITE = 13;
  SQLITE_FCNTL_PRAGMA = 14;
  SQLITE_FCNTL_BUSYHANDLER = 15;
  SQLITE_FCNTL_TEMPFILENAME = 16;
  SQLITE_FCNTL_MMAP_SIZE = 18;
  SQLITE_FCNTL_TRACE = 19;
  SQLITE_FCNTL_HAS_MOVED = 20;
  SQLITE_FCNTL_SYNC = 21;
  SQLITE_FCNTL_COMMIT_PHASETWO = 22;
  SQLITE_FCNTL_WIN32_SET_HANDLE = 23;
  SQLITE_FCNTL_WAL_BLOCK = 24;
  SQLITE_FCNTL_ZIPVFS = 25;
  SQLITE_FCNTL_RBU = 26;
  SQLITE_FCNTL_VFS_POINTER = 27;
  SQLITE_FCNTL_JOURNAL_POINTER = 28;
  SQLITE_FCNTL_WIN32_GET_HANDLE = 29;
  SQLITE_FCNTL_PDB = 30;
  SQLITE_FCNTL_BEGIN_ATOMIC_WRITE = 31;
  SQLITE_FCNTL_COMMIT_ATOMIC_WRITE = 32;
  SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE = 33;
  SQLITE_FCNTL_LOCK_TIMEOUT = 34;
  SQLITE_FCNTL_DATA_VERSION = 35;
  SQLITE_FCNTL_SIZE_LIMIT = 36;
  SQLITE_FCNTL_CKPT_DONE = 37;
  SQLITE_FCNTL_RESERVE_BYTES = 38;
  SQLITE_FCNTL_CKPT_START = 39;
  SQLITE_FCNTL_EXTERNAL_READER = 40;
  SQLITE_FCNTL_CKSM_FILE = 41;
  SQLITE_FCNTL_RESET_CACHE = 42;
  SQLITE_FCNTL_NULL_IO = 43;
  SQLITE_FCNTL_BLOCK_ON_CONNECT = 44;
  SQLITE_FCNTL_FILESTAT = 45;

  { The xDeviceCharacteristics and IOCAP bits. }
  SQLITE_IOCAP_ATOMIC = $1;
  SQLITE_IOCAP_ATOMIC512 = $2;
  SQLITE_IOCAP_ATOMIC1K = $4;
  SQLITE_IOCAP_ATOMIC2K = $8;
  SQLITE_IOCAP_ATOMIC4K = $10;
  SQLITE_IOCAP_ATOMIC8K = $20;
  SQLITE_IOCAP_ATOMIC16K = $40;
  SQLITE_IOCAP_ATOMIC32K = $80;
  SQLITE_IOCAP_ATOMIC64K = $100;
  SQLITE_IOCAP_SAFE_APPEND = $200;
  SQLITE_IOCAP_SEQUENTIAL = $400;
  SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN = $800;
  SQLITE_IOCAP_POWERSAFE_OVERWRITE = $1000;
  SQLITE_IOCAP_IMMUTABLE = $2000;
  SQLITE_IOCAP_BATCH_ATOMIC = $4000;
  SQLITE_IOCAP_SUBPAGE_READ = $8000;

  { xLock, xUnlock's states. }
  SQLITE_LOCK_NONE = 0;
  SQLITE_LOCK_SHARED = 1;
  SQLITE_LOCK_RESERVED = 2;
  SQLITE_LOCK_PENDING = 3;
  SQLITE_LOCK_EXCLUSIVE = 4;

  { xSync's flags. }
  SQLITE_SYNC_NORMAL = $2;
  SQLITE_SYNC_FULL = $3;
  SQLITE_SYNC_DATAONLY = $10;

  { xAccess's request codes. }
  SQLITE_ACCESS_EXISTS = 0;
  SQLITE_ACCESS_READWRITE = 1;
  SQLITE_ACCESS_READ = 2;

  { xShmLock's flags, and the lock index bound. }
  SQLITE_SHM_UNLOCK = 1;
  SQLITE_SHM_LOCK = 2;
  SQLITE_SHM_SHARED = 4;
  SQLITE_SHM_EXCLUSIVE = 8;
  SQLITE_SHM_NLOCK = 8;

  { sqlite3_mutex_alloc's mutex kinds, static ones included. }
  SQLITE_MUTEX_FAST = 0;
  SQLITE_MUTEX_RECURSIVE = 1;
  SQLITE_MUTEX_STATIC_MAIN = 2;
  SQLITE_MUTEX_STATIC_MEM = 3;
  SQLITE_MUTEX_STATIC_MEM2 = 4;
  SQLITE_MUTEX_STATIC_OPEN = 4;
  SQLITE_MUTEX_STATIC_PRNG = 5;
  SQLITE_MUTEX_STATIC_LRU = 6;
  SQLITE_MUTEX_STATIC_LRU2 = 7;
  SQLITE_MUTEX_STATIC_PMEM = 7;
  SQLITE_MUTEX_STATIC_APP1 = 8;
  SQLITE_MUTEX_STATIC_APP2 = 9;
  SQLITE_MUTEX_STATIC_APP3 = 10;
  SQLITE_MUTEX_STATIC_VFS1 = 11;
  SQLITE_MUTEX_STATIC_VFS2 = 12;
  SQLITE_MUTEX_STATIC_VFS3 = 13;
  SQLITE_MUTEX_STATIC_MASTER = 2;

  { sqlite3_test_control's options (SQLite's own tests; several gone or
    overlapping by now — TESTCTRL_FIRST through LAST name the range). }
  SQLITE_TESTCTRL_FIRST = 5;
  SQLITE_TESTCTRL_PRNG_SAVE = 5;
  SQLITE_TESTCTRL_PRNG_RESTORE = 6;
  SQLITE_TESTCTRL_PRNG_RESET = 7;
  SQLITE_TESTCTRL_FK_NO_ACTION = 7;
  SQLITE_TESTCTRL_BITVEC_TEST = 8;
  SQLITE_TESTCTRL_FAULT_INSTALL = 9;
  SQLITE_TESTCTRL_BENIGN_MALLOC_HOOKS = 10;
  SQLITE_TESTCTRL_PENDING_BYTE = 11;
  SQLITE_TESTCTRL_ASSERT = 12;
  SQLITE_TESTCTRL_ALWAYS = 13;
  SQLITE_TESTCTRL_RESERVE = 14;
  SQLITE_TESTCTRL_JSON_SELFCHECK = 14;
  SQLITE_TESTCTRL_OPTIMIZATIONS = 15;
  SQLITE_TESTCTRL_ISKEYWORD = 16;
  SQLITE_TESTCTRL_GETOPT = 16;
  SQLITE_TESTCTRL_SCRATCHMALLOC = 17;
  SQLITE_TESTCTRL_INTERNAL_FUNCTIONS = 17;
  SQLITE_TESTCTRL_LOCALTIME_FAULT = 18;
  SQLITE_TESTCTRL_EXPLAIN_STMT = 19;
  SQLITE_TESTCTRL_ONCE_RESET_THRESHOLD = 19;
  SQLITE_TESTCTRL_NEVER_CORRUPT = 20;
  SQLITE_TESTCTRL_VDBE_COVERAGE = 21;
  SQLITE_TESTCTRL_BYTEORDER = 22;
  SQLITE_TESTCTRL_ISINIT = 23;
  SQLITE_TESTCTRL_SORTER_MMAP = 24;
  SQLITE_TESTCTRL_IMPOSTER = 25;
  SQLITE_TESTCTRL_PARSER_COVERAGE = 26;
  SQLITE_TESTCTRL_RESULT_INTREAL = 27;
  SQLITE_TESTCTRL_PRNG_SEED = 28;
  SQLITE_TESTCTRL_EXTRA_SCHEMA_CHECKS = 29;
  SQLITE_TESTCTRL_SEEK_COUNT = 30;
  SQLITE_TESTCTRL_TRACEFLAGS = 31;
  SQLITE_TESTCTRL_TUNE = 32;
  SQLITE_TESTCTRL_LOGEST = 33;
  SQLITE_TESTCTRL_USELONGDOUBLE = 34;
  SQLITE_TESTCTRL_ATOF = 34;
  SQLITE_TESTCTRL_LAST = 34;

  { The authorizer's action codes; xAuth takes up to four strings for
    them (the table, column, trigger or database they name). }
  SQLITE_DENY = 1;
  SQLITE_IGNORE = 2;
  SQLITE_CREATE_INDEX = 1;
  SQLITE_CREATE_TABLE = 2;
  SQLITE_CREATE_TEMP_INDEX = 3;
  SQLITE_CREATE_TEMP_TABLE = 4;
  SQLITE_CREATE_TEMP_TRIGGER = 5;
  SQLITE_CREATE_TEMP_VIEW = 6;
  SQLITE_CREATE_TRIGGER = 7;
  SQLITE_CREATE_VIEW = 8;
  SQLITE_DELETE = 9;
  SQLITE_DROP_INDEX = 10;
  SQLITE_DROP_TABLE = 11;
  SQLITE_DROP_TEMP_INDEX = 12;
  SQLITE_DROP_TEMP_TABLE = 13;
  SQLITE_DROP_TEMP_TRIGGER = 14;
  SQLITE_DROP_TEMP_VIEW = 15;
  SQLITE_DROP_TRIGGER = 16;
  SQLITE_DROP_VIEW = 17;
  SQLITE_INSERT = 18;
  SQLITE_PRAGMA = 19;
  SQLITE_READ = 20;
  SQLITE_SELECT = 21;
  SQLITE_TRANSACTION = 22;
  SQLITE_UPDATE = 23;
  SQLITE_ATTACH = 24;
  SQLITE_DETACH = 25;
  SQLITE_ALTER_TABLE = 26;
  SQLITE_REINDEX = 27;
  SQLITE_ANALYZE = 28;
  SQLITE_CREATE_VTABLE = 29;
  SQLITE_DROP_VTABLE = 30;
  SQLITE_FUNCTION = 31;
  SQLITE_SAVEPOINT = 32;
  SQLITE_RECURSIVE = 33;

  { sqlite3_vtab_config's options. }
  SQLITE_VTAB_CONSTRAINT_SUPPORT = 1;
  SQLITE_VTAB_INNOCUOUS = 2;
  SQLITE_VTAB_DIRECTONLY = 3;
  SQLITE_VTAB_USES_ALL_SCHEMAS = 4;

  { The ON CONFLICT ways: sqlite3_vtab_on_conflict's answer, set with vtab
    config's CONSTRAINT_SUPPORT in a module's xConnect. }
  SQLITE_ROLLBACK = 1;
  SQLITE_FAIL = 3;
  SQLITE_REPLACE = 5;

  { sqlite3_txn_state's states. }
  SQLITE_TXN_NONE = 0;
  SQLITE_TXN_READ = 1;
  SQLITE_TXN_WRITE = 2;

  { sqlite3_index_info's IdxFlags, and the constraint operators its
    AConstraint[].Op takes (FUNCTION through 255: a module's overloaded
    functions; LIMIT and OFFSET have no left-hand column). }
  SQLITE_INDEX_SCAN_UNIQUE = $1;
  SQLITE_INDEX_SCAN_HEX = $2;
  SQLITE_INDEX_CONSTRAINT_EQ = 2;
  SQLITE_INDEX_CONSTRAINT_GT = 4;
  SQLITE_INDEX_CONSTRAINT_LE = 8;
  SQLITE_INDEX_CONSTRAINT_LT = 16;
  SQLITE_INDEX_CONSTRAINT_GE = 32;
  SQLITE_INDEX_CONSTRAINT_MATCH = 64;
  SQLITE_INDEX_CONSTRAINT_LIKE = 65;
  SQLITE_INDEX_CONSTRAINT_GLOB = 66;
  SQLITE_INDEX_CONSTRAINT_REGEXP = 67;
  SQLITE_INDEX_CONSTRAINT_NE = 68;
  SQLITE_INDEX_CONSTRAINT_ISNOT = 69;
  SQLITE_INDEX_CONSTRAINT_ISNOTNULL = 70;
  SQLITE_INDEX_CONSTRAINT_ISNULL = 71;
  SQLITE_INDEX_CONSTRAINT_IS = 72;
  SQLITE_INDEX_CONSTRAINT_LIMIT = 73;
  SQLITE_INDEX_CONSTRAINT_OFFSET = 74;
  SQLITE_INDEX_CONSTRAINT_FUNCTION = 150;

  { sqlite3_serialize and sqlite3_deserialize's flags. }
  SQLITE_SERIALIZE_NOCOPY = $1;
  SQLITE_DESERIALIZE_FREEONCLOSE = 1;
  SQLITE_DESERIALIZE_RESIZEABLE = 2;
  SQLITE_DESERIALIZE_READONLY = 4;

  { sqlite3_carray_bind's array kinds. }
  SQLITE_CARRAY_INT32 = 0;
  SQLITE_CARRAY_INT64 = 1;
  SQLITE_CARRAY_DOUBLE = 2;
  SQLITE_CARRAY_TEXT = 3;
  SQLITE_CARRAY_BLOB = 4;

  { Null, when a callback may be left out (the C's null): the constant of
    each callback type this unit's parameters take. These are for passing,
    not storing; the SQLITE_NULL_ constants never change. }
  SQLITE_NULL_EXEC: TSQLite3ExecCallback = nil;
  SQLITE_NULL_BUSY: TSQLite3BusyCallback = nil;
  SQLITE_NULL_AUTHORIZER: TSQLite3AuthorizerCallback = nil;
  SQLITE_NULL_PROGRESS: TSQLite3ProgressCallback = nil;
  SQLITE_NULL_TRACE: TSQLite3TraceCallback = nil;
  SQLITE_NULL_COMMIT: TSQLite3CommitCallback = nil;
  SQLITE_NULL_ROLLBACK: TSQLite3RollbackCallback = nil;
  SQLITE_NULL_UPDATE: TSQLite3UpdateCallback = nil;
  SQLITE_NULL_WAL: TSQLite3WalCallback = nil;
  SQLITE_NULL_AUTOVACUUM: TSQLite3AutoVacuumCallback = nil;
  SQLITE_NULL_COLLATION: TSQLite3CollationCallback = nil;
  SQLITE_NULL_COLLATION_NEEDED: TSQLite3CollationNeededCallback = nil;
  SQLITE_NULL_SCALAR: TSQLite3ScalarCallback = nil;       { an xFunc of nil }
  SQLITE_NULL_STEP: TSQLite3StepCallback = nil;           { an xStep, xInverse }
  SQLITE_NULL_FINAL: TSQLite3FinalCallback = nil;         { an xFinal, xValue }
  SQLITE_NULL_NOTIFY: TSQLite3UnlockNotifyCallback = nil;

var
  { The destructor parameters' two special words. SQLITE_TRANSIENT has
    sqlite copy the bytes before the call returns: the C value is a
    routine pointer cast of -1, a Pascal constant cannot be that, so the
    word is loaded at start-up — the int64 is its bytes.
    SQLITE_STATIC (keep them as they are) is its null: pass it, or pass
    your own destructor by name. }
  SQLITE_TRANSIENT_WORD: int64;
  SQLITE_TRANSIENT: TSQLite3Destructor absolute SQLITE_TRANSIENT_WORD;
  SQLITE_STATIC: TSQLite3Destructor = nil;

var
  { sqlite3_version, the string sqlite3_libversion gives. }
  sqlite3_version: pchar; external 'sqlite3' name 'sqlite3_version';

  { Where temporary files go, while the library's own leave the C value
    null: set before sqlite3_initialize, if at all. }
  sqlite3_temp_directory: pchar; external 'sqlite3' name 'sqlite3_temp_directory';
  sqlite3_data_directory: pchar; external 'sqlite3' name 'sqlite3_data_directory';

{ --- The library, its version and options. }

function sqlite3_libversion: pchar; cdecl; external 'sqlite3';
function sqlite3_sourceid: pchar; cdecl; external 'sqlite3';
function sqlite3_libversion_number: longint; cdecl; external 'sqlite3';
function sqlite3_compileoption_used(OptionName: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_compileoption_get(N: longint): pchar; cdecl; external 'sqlite3';
function sqlite3_threadsafe: longint; cdecl; external 'sqlite3';

{ --- Opening, closing and running; exec runs one SQL text or a script,
      giving Callback a row each as it goes (return it non-zero to stop
      with SQLITE_ABORT). }

function sqlite3_open(FileName: pchar; var Db: PSQLite3): longint;
  cdecl; external 'sqlite3';
function sqlite3_open16(FileName: pointer; var Db: PSQLite3): longint;
  cdecl; external 'sqlite3';
function sqlite3_open_v2(FileName: pchar; var Db: PSQLite3; Flags: longint;
  Vfs: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_close(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_close_v2(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_exec(Db: PSQLite3; Sql: pchar; Callback: TSQLite3ExecCallback;
  Data: pointer; var ErrMsg: pchar): longint; cdecl; external 'sqlite3';

{ --- The library's and one connection's configuration. }

function sqlite3_initialize: longint; cdecl; external 'sqlite3';
function sqlite3_shutdown: longint; cdecl; external 'sqlite3';
function sqlite3_os_init: longint; cdecl; external 'sqlite3';
function sqlite3_os_end: longint; cdecl; external 'sqlite3';
function sqlite3_extended_result_codes(Db: PSQLite3; OnOff: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_config(Op: longint): longint; cdecl; varargs; external 'sqlite3';
{ The typed entry point of sqlite3_config's LOG operation (a varargs call
  cannot carry a routine); it registers XLog or clears it. }
function sqlite3_config_log(Data: pointer;
  XLog: TSQLite3LogCallback): longint; cdecl; external 'sqlite3' name 'sqlite3_config';
function sqlite3_db_config(Db: PSQLite3; Op: longint): longint;
  cdecl; varargs; external 'sqlite3';
function sqlite3_last_insert_rowid(Db: PSQLite3): int64; cdecl; external 'sqlite3';
procedure sqlite3_set_last_insert_rowid(Db: PSQLite3; RowId: int64);
  cdecl; external 'sqlite3';
function sqlite3_changes(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_changes64(Db: PSQLite3): int64; cdecl; external 'sqlite3';
function sqlite3_total_changes(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_total_changes64(Db: PSQLite3): int64; cdecl; external 'sqlite3';
procedure sqlite3_interrupt(Db: PSQLite3); cdecl; external 'sqlite3';
function sqlite3_is_interrupted(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_complete(Sql: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_complete16(Sql: pointer): longint; cdecl; external 'sqlite3';
{ 3.54: how many bytes of a statement text are not yet complete. }
function sqlite3_incomplete(Sql: pchar): int64; cdecl; external 'sqlite3';
{ Set a runtime limit: pass the SQLITE_LIMIT_ code, get the old value;
  -1 says keep it while asking. }
function sqlite3_limit(Db: PSQLite3; Id: longint; NewVal: longint): longint;
  cdecl; external 'sqlite3';

{ --- Busy waits, locks and timeouts: the busy handler (return nonzero to
      wait again) or a millisecond timeout, and the v2 file-lock
      milliseconds with SQLITE_SETLK_BLOCK_ON_CONNECT (or 0). }

{ BusyCallback = TSQLite3BusyCallback. }
function sqlite3_busy_handler(Db: PSQLite3; BusyCallback: TSQLite3BusyCallback;
  Data: pointer): longint; cdecl; external 'sqlite3';
function sqlite3_busy_timeout(Db: PSQLite3; Ms: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_setlk_timeout(Db: PSQLite3; Ms: longint;
  Flags: longint): longint; cdecl; external 'sqlite3';
const SQLITE_SETLK_BLOCK_ON_CONNECT = $1;

{ --- sqlite3_get_table's whole answer in one pchar table (Rows times
      Columns+1 strings, freed with sqlite3_free_table, sqlite3_malloc
      made). }

function sqlite3_get_table(Db: PSQLite3; Sql: pchar; var Table: PPChar;
  var NRow: longint; var NCol: longint; var ErrMsg: pchar): longint;
  cdecl; external 'sqlite3';
procedure sqlite3_free_table(Table: PPChar); cdecl; external 'sqlite3';

{ --- Memory and strings: sqlite3_free frees what sqlite3_malloc and the
      errmsgs, expanded_sql, str_finish and the like hand back. Msize
      counts an allocation's bytes. }

function sqlite3_malloc(N: longint): pointer; cdecl; external 'sqlite3';
function sqlite3_malloc64(Bytes: qword): pointer; cdecl; external 'sqlite3';
function sqlite3_realloc(P: pointer; N: longint): pointer; cdecl; external 'sqlite3';
function sqlite3_realloc64(P: pointer; Bytes: qword): pointer; cdecl; external 'sqlite3';
procedure sqlite3_free(P: pointer); cdecl; external 'sqlite3';
function sqlite3_msize(P: pointer): qword; cdecl; external 'sqlite3';
function sqlite3_memory_used: int64; cdecl; external 'sqlite3';
function sqlite3_memory_highwater(ResetFlag: longint): int64; cdecl; external 'sqlite3';
procedure sqlite3_randomness(N: longint; P: pointer); cdecl; external 'sqlite3';

{ printf-like: as C's, %s a pchar, %q %Q %w SQL, %lld an int64. }
function sqlite3_mprintf(Format: pchar): pchar; cdecl; varargs; external 'sqlite3';
function sqlite3_snprintf(N: longint; ZOut: pchar; Format: pchar): pchar;
  cdecl; varargs; external 'sqlite3';

{ --- Who may do what, and what gets logged: the authorizer, the tracer,
      the per-statement progress callback.

      Authoriser = TSQLite3AuthorizerCallback (SQLITE_OK, DENY or IGNORE);
      TraceCallback = TSQLite3TraceCallback; ProgressCallback =
      TSQLite3ProgressCallback; nil takes the older one back. }

function sqlite3_set_authorizer(Db: PSQLite3;
  Authorizer: TSQLite3AuthorizerCallback; UserData: pointer): longint;
  cdecl; external 'sqlite3';
function sqlite3_trace_v2(Db: PSQLite3; UMask: cardinal;
  XCallback: TSQLite3TraceCallback; PCtx: pointer): longint;
  cdecl; external 'sqlite3';
procedure sqlite3_progress_handler(Db: PSQLite3; N: longint;
  XProgress: TSQLite3ProgressCallback; Data: pointer); cdecl; external 'sqlite3';

{ --- Names, error codes, messages and where in the SQL a text error was. }

function sqlite3_errcode(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_extended_errcode(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_errmsg(Db: PSQLite3): pchar; cdecl; external 'sqlite3';
function sqlite3_errmsg16(Db: PSQLite3): pointer; cdecl; external 'sqlite3';
function sqlite3_errstr(Code: longint): pchar; cdecl; external 'sqlite3';
function sqlite3_error_offset(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_set_errmsg(Db: PSQLite3; ErrCode: longint;
  ErrMsg: pchar): longint; cdecl; external 'sqlite3';

{ --- Prepared statements: one call per text, Tail (a const pchar out)
      left pointing at the rest of the script after it; v3 adds flags.
      Expanded_sql takes the bound values in (sqlite3_free it). }

function sqlite3_prepare(Db: PSQLite3; ZSql: pchar; NByte: longint;
  var PPStmt: PSQLite3Stmt; var PzTail: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_prepare_v2(Db: PSQLite3; ZSql: pchar; NByte: longint;
  var PPStmt: PSQLite3Stmt; var PzTail: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_prepare_v3(Db: PSQLite3; ZSql: pchar; NByte: longint;
  PrepFlags: cardinal; var PPStmt: PSQLite3Stmt;
  var PzTail: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_prepare16(Db: PSQLite3; ZSql: pointer; NByte: longint;
  var PPStmt: PSQLite3Stmt; var PzTail: pointer): longint; cdecl; external 'sqlite3';
function sqlite3_prepare16_v2(Db: PSQLite3; ZSql: pointer; NByte: longint;
  var PPStmt: PSQLite3Stmt; var PzTail: pointer): longint; cdecl; external 'sqlite3';
function sqlite3_prepare16_v3(Db: PSQLite3; ZSql: pointer; NByte: longint;
  PrepFlags: cardinal; var PPStmt: PSQLite3Stmt;
  var PzTail: pointer): longint; cdecl; external 'sqlite3';
function sqlite3_sql(Stmt: PSQLite3Stmt): pchar; cdecl; external 'sqlite3';
function sqlite3_expanded_sql(Stmt: PSQLite3Stmt): pchar; cdecl; external 'sqlite3';
function sqlite3_stmt_readonly(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
function sqlite3_stmt_isexplain(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
{ Explain's depth: 0 as the program will, 1 (sqlite3's own) or 2. }
function sqlite3_stmt_explain(Stmt: PSQLite3Stmt; EMode: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_stmt_busy(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';

{ --- Binding values, one routine per type; the N is the 1-based index.
      The destructor parameter keeps the bytes (SQLITE_STATIC), has sqlite
      copy them (SQLITE_TRANSIENT) or runs a destructor by name; a
      negative NByte says null-terminated. bind_value rebinds a
      PSQLite3Value; bind_pointer names the type the caller wants back;
      bind_parameter_* ask the statement's parameters. }

function sqlite3_bind_blob(Stmt: PSQLite3Stmt; N: longint; Value: pointer;
  NBytes: longint; DestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
function sqlite3_bind_blob64(Stmt: PSQLite3Stmt; N: longint; Value: pointer;
  NBytes: qword; DestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
function sqlite3_bind_double(Stmt: PSQLite3Stmt; N: longint;
  Value: double): longint; cdecl; external 'sqlite3';
function sqlite3_bind_int(Stmt: PSQLite3Stmt; N: longint;
  Value: longint): longint; cdecl; external 'sqlite3';
function sqlite3_bind_int64(Stmt: PSQLite3Stmt; N: longint;
  Value: int64): longint; cdecl; external 'sqlite3';
function sqlite3_bind_null(Stmt: PSQLite3Stmt; N: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_bind_text(Stmt: PSQLite3Stmt; N: longint; Text: pchar;
  NBytes: longint; DestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
function sqlite3_bind_text16(Stmt: PSQLite3Stmt; N: longint; Text: pointer;
  NBytes: longint; DestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
function sqlite3_bind_text64(Stmt: PSQLite3Stmt; N: longint; Text: pchar;
  NBytes: qword; DestroyP: TSQLite3Destructor; Encoding: byte): longint;
  cdecl; external 'sqlite3';
function sqlite3_bind_value(Stmt: PSQLite3Stmt; N: longint;
  Value: PSQLite3Value): longint; cdecl; external 'sqlite3';
function sqlite3_bind_pointer(Stmt: PSQLite3Stmt; N: longint; PArg: pointer;
  TypeName: pchar; DestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
function sqlite3_bind_zeroblob(Stmt: PSQLite3Stmt; N: longint;
  NBytes: longint): longint; cdecl; external 'sqlite3';
function sqlite3_bind_zeroblob64(Stmt: PSQLite3Stmt; N: longint;
  NBytes: qword): longint; cdecl; external 'sqlite3';
function sqlite3_bind_parameter_count(Stmt: PSQLite3Stmt): longint;
  cdecl; external 'sqlite3';
function sqlite3_bind_parameter_name(Stmt: PSQLite3Stmt; N: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_bind_parameter_index(Stmt: PSQLite3Stmt;
  ZName: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_clear_bindings(Stmt: PSQLite3Stmt): longint;
  cdecl; external 'sqlite3';

{ --- The result's columns: how many, what each is called and where it
      came from (database, table, origin and decltype names). }

function sqlite3_column_count(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
function sqlite3_column_name(Stmt: PSQLite3Stmt; N: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_name16(Stmt: PSQLite3Stmt; N: longint): pointer;
  cdecl; external 'sqlite3';
function sqlite3_column_database_name(Stmt: PSQLite3Stmt; N: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_database_name16(Stmt: PSQLite3Stmt;
  N: longint): pointer; cdecl; external 'sqlite3';
function sqlite3_column_table_name(Stmt: PSQLite3Stmt; N: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_table_name16(Stmt: PSQLite3Stmt; N: longint): pointer;
  cdecl; external 'sqlite3';
function sqlite3_column_origin_name(Stmt: PSQLite3Stmt; N: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_origin_name16(Stmt: PSQLite3Stmt; N: longint): pointer;
  cdecl; external 'sqlite3';
function sqlite3_column_decltype(Stmt: PSQLite3Stmt; N: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_decltype16(Stmt: PSQLite3Stmt; N: longint): pointer;
  cdecl; external 'sqlite3';

{ --- Stepping: SQLITE_ROW has a row to read (its values live until the
      next step, reset or finalize), SQLITE_DONE finished, or an error
      code; reset after every run, finalize once. }

function sqlite3_step(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
function sqlite3_data_count(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
function sqlite3_column_blob(Stmt: PSQLite3Stmt; ICol: longint): pointer;
  cdecl; external 'sqlite3';
function sqlite3_column_double(Stmt: PSQLite3Stmt; ICol: longint): double;
  cdecl; external 'sqlite3';
function sqlite3_column_int(Stmt: PSQLite3Stmt; ICol: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_column_int64(Stmt: PSQLite3Stmt; ICol: longint): int64;
  cdecl; external 'sqlite3';
function sqlite3_column_text(Stmt: PSQLite3Stmt; ICol: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_column_text16(Stmt: PSQLite3Stmt; ICol: longint): pointer;
  cdecl; external 'sqlite3';
function sqlite3_column_value(Stmt: PSQLite3Stmt; ICol: longint): PSQLite3Value;
  cdecl; external 'sqlite3';
function sqlite3_column_bytes(Stmt: PSQLite3Stmt; ICol: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_column_bytes16(Stmt: PSQLite3Stmt; ICol: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_column_type(Stmt: PSQLite3Stmt; ICol: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_finalize(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';
function sqlite3_reset(Stmt: PSQLite3Stmt): longint; cdecl; external 'sqlite3';

{ --- The SQL functions the program writes: name (UTF8 default), the
      number of arguments (-1 for any), an encoding with the
      SQLITE_DETERMINISTIC flags, Data, and the routines by name. A scalar
      gives SQLITE_NULL_STEP and SQLITE_NULL_FINAL, an aggregate
      SQLITE_NULL_SCALAR and the step and final routines; a window adds
      xValue (SQLITE_NULL_FINAL or the routine) and xInverse. XDestroyP
      takes SQLITE_STATIC (nil) or a destructor by name. }

function sqlite3_create_function(Db: PSQLite3; FuncName: pchar; NArg: longint;
  ETextRep: longint; PApp: pointer; XFunc: TSQLite3ScalarCallback;
  XStep: TSQLite3StepCallback; XFinal: TSQLite3FinalCallback): longint;
  cdecl; external 'sqlite3';
function sqlite3_create_function16(Db: PSQLite3; FuncName: pointer;
  NArg: longint; ETextRep: longint; PApp: pointer; XFunc: TSQLite3ScalarCallback;
  XStep: TSQLite3StepCallback; XFinal: TSQLite3FinalCallback): longint;
  cdecl; external 'sqlite3';
function sqlite3_create_function_v2(Db: PSQLite3; FuncName: pchar;
  NArg: longint; ETextRep: longint; PApp: pointer; XFunc: TSQLite3ScalarCallback;
  XStep: TSQLite3StepCallback; XFinal: TSQLite3FinalCallback;
  XDestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
function sqlite3_create_window_function(Db: PSQLite3; FuncName: pchar;
  NArg: longint; ETextRep: longint; PApp: pointer; XStep: TSQLite3StepCallback;
  XFinal: TSQLite3FinalCallback; XValue: TSQLite3FinalCallback;
  XInverse: TSQLite3StepCallback; XDestroyP: TSQLite3Destructor): longint;
  cdecl; external 'sqlite3';

{ --- Inside a function: the values, the answer, and the side data. Value
      routines answer the caller's arguments; value_nonechange (of an
      xUpdate argv) says the value was not written; value_encoding reads
      UTF8 or UTF16; xDestroy = TSQLite3Destructor or nil. }

function sqlite3_value_blob(Value: PSQLite3Value): pointer; cdecl; external 'sqlite3';
function sqlite3_value_double(Value: PSQLite3Value): double; cdecl; external 'sqlite3';
function sqlite3_value_int(Value: PSQLite3Value): longint; cdecl; external 'sqlite3';
function sqlite3_value_int64(Value: PSQLite3Value): int64; cdecl; external 'sqlite3';
function sqlite3_value_pointer(Value: PSQLite3Value;
  TypeName: pchar): pointer; cdecl; external 'sqlite3';
function sqlite3_value_text(Value: PSQLite3Value): pchar; cdecl; external 'sqlite3';
function sqlite3_value_text16(Value: PSQLite3Value): pointer; cdecl; external 'sqlite3';
function sqlite3_value_text16le(Value: PSQLite3Value): pointer; cdecl; external 'sqlite3';
function sqlite3_value_text16be(Value: PSQLite3Value): pointer; cdecl; external 'sqlite3';
function sqlite3_value_bytes(Value: PSQLite3Value): longint; cdecl; external 'sqlite3';
function sqlite3_value_bytes16(Value: PSQLite3Value): longint; cdecl; external 'sqlite3';
function sqlite3_value_type(Value: PSQLite3Value): longint; cdecl; external 'sqlite3';
function sqlite3_value_numeric_type(Value: PSQLite3Value): longint;
  cdecl; external 'sqlite3';
function sqlite3_value_nochange(Value: PSQLite3Value): longint;
  cdecl; external 'sqlite3';
function sqlite3_value_frombind(Value: PSQLite3Value): longint;
  cdecl; external 'sqlite3';
function sqlite3_value_encoding(Value: PSQLite3Value): longint;
  cdecl; external 'sqlite3';
function sqlite3_value_subtype(Value: PSQLite3Value): cardinal;
  cdecl; external 'sqlite3';
function sqlite3_value_dup(Value: PSQLite3Value): PSQLite3Value;
  cdecl; external 'sqlite3';
procedure sqlite3_value_free(Value: PSQLite3Value); cdecl; external 'sqlite3';

function sqlite3_aggregate_context(Context: PSQLite3Context;
  NBytes: longint): pointer; cdecl; external 'sqlite3';
function sqlite3_user_data(Context: PSQLite3Context): pointer;
  cdecl; external 'sqlite3';
function sqlite3_context_db_handle(Context: PSQLite3Context): PSQLite3;
  cdecl; external 'sqlite3';
{ The Nth extra value (nil-able, DestroyP as always). }
function sqlite3_get_auxdata(Context: PSQLite3Context; N: longint): pointer;
  cdecl; external 'sqlite3';
procedure sqlite3_set_auxdata(Context: PSQLite3Context; N: longint;
  Data: pointer; XDestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
{ Per-connection named values (sqlite3_get_clientdata of any name). }
function sqlite3_get_clientdata(Db: PSQLite3; Name: pchar): pointer;
  cdecl; external 'sqlite3';
function sqlite3_set_clientdata(Db: PSQLite3; Name: pchar; Data: pointer;
  XDestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';

{ --- The answers: a value per call, the text and blob ones with the
      destructor parameter's lifetime, the error ones naming a message or
      code, the subtype to carry one, and result_str to hand a whole
      string accumulator over. }

procedure sqlite3_result_blob(Context: PSQLite3Context; Value: pointer;
  NBytes: longint; DestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
procedure sqlite3_result_blob64(Context: PSQLite3Context; Value: pointer;
  NBytes: qword; DestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
procedure sqlite3_result_double(Context: PSQLite3Context; Value: double);
  cdecl; external 'sqlite3';
procedure sqlite3_result_error(Context: PSQLite3Context; ErrMsg: pchar;
  NBytes: longint); cdecl; external 'sqlite3';
procedure sqlite3_result_error16(Context: PSQLite3Context; ErrMsg: pointer;
  NBytes: longint); cdecl; external 'sqlite3';
procedure sqlite3_result_error_toobig(Context: PSQLite3Context);
  cdecl; external 'sqlite3';
procedure sqlite3_result_error_nomem(Context: PSQLite3Context);
  cdecl; external 'sqlite3';
procedure sqlite3_result_error_code(Context: PSQLite3Context; ErrCode: longint);
  cdecl; external 'sqlite3';
procedure sqlite3_result_int(Context: PSQLite3Context; Value: longint);
  cdecl; external 'sqlite3';
procedure sqlite3_result_int64(Context: PSQLite3Context; Value: int64);
  cdecl; external 'sqlite3';
procedure sqlite3_result_null(Context: PSQLite3Context); cdecl; external 'sqlite3';
procedure sqlite3_result_text(Context: PSQLite3Context; Text: pchar;
  NBytes: longint; DestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
procedure sqlite3_result_text64(Context: PSQLite3Context; Text: pchar;
  NBytes: qword; DestroyP: TSQLite3Destructor; Encoding: byte); cdecl; external 'sqlite3';
procedure sqlite3_result_text16(Context: PSQLite3Context; Text: pointer;
  NBytes: longint; DestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
procedure sqlite3_result_text16le(Context: PSQLite3Context; Text: pointer;
  NBytes: longint; DestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
procedure sqlite3_result_text16be(Context: PSQLite3Context; Text: pointer;
  NBytes: longint; DestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
procedure sqlite3_result_value(Context: PSQLite3Context;
  Value: PSQLite3Value); cdecl; external 'sqlite3';
procedure sqlite3_result_pointer(Context: PSQLite3Context; PArg: pointer;
  TypeName: pchar; DestroyP: TSQLite3Destructor); cdecl; external 'sqlite3';
procedure sqlite3_result_zeroblob(Context: PSQLite3Context; NBytes: longint);
  cdecl; external 'sqlite3';
function sqlite3_result_zeroblob64(Context: PSQLite3Context;
  NBytes: qword): longint; cdecl; external 'sqlite3';
procedure sqlite3_result_subtype(Context: PSQLite3Context;
  Subtype: cardinal); cdecl; external 'sqlite3';
procedure sqlite3_result_str(Context: PSQLite3Context; Str: PSQLite3Str;
  Flags: longint); cdecl; external 'sqlite3';

{ --- Collations: Compare = TSQLite3CollationCallback, DestroyP =
      TSQLite3Destructor or nil; collation_needed's Callback =
      TSQLite3CollationNeededCallback. create_collation16 names by UTF16. }

function sqlite3_create_collation(Db: PSQLite3; Name: pchar; ETextRep: longint;
  PArg: pointer; XCompare: TSQLite3CollationCallback): longint;
  cdecl; external 'sqlite3';
function sqlite3_create_collation_v2(Db: PSQLite3; Name: pchar;
  ETextRep: longint; PArg: pointer; XCompare,
  XDestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
function sqlite3_create_collation16(Db: PSQLite3; Name: pointer;
  ETextRep: longint; PArg: pointer;
  XCompare: TSQLite3CollationCallback): longint; cdecl; external 'sqlite3';
function sqlite3_collation_needed(Db: PSQLite3; PArg: pointer;
  XCollationNeeded: TSQLite3CollationNeededCallback): longint;
  cdecl; external 'sqlite3';
function sqlite3_collation_needed16(Db: PSQLite3; PArg: pointer;
  XCollationNeeded: TSQLite3CollationNeededCallback): longint;
  cdecl; external 'sqlite3';

{ --- Waiting: a C sleep() like wait, in milliseconds. }

function sqlite3_sleep(Ms: longint): longint; cdecl; external 'sqlite3';

{ --- One connection: its names and files, the autocommit state, the
      connection of a statement, the transaction state, the statement
      after one (nil at the end; nil Stmt starts the walk). }

function sqlite3_get_autocommit(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_db_handle(Stmt: PSQLite3Stmt): PSQLite3; cdecl; external 'sqlite3';
function sqlite3_db_name(Db: PSQLite3; N: longint): pchar; cdecl; external 'sqlite3';
function sqlite3_db_filename(Db: PSQLite3; ZDbName: pchar): PSQLite3FileName;
  cdecl; external 'sqlite3';
function sqlite3_db_readonly(Db: PSQLite3; ZDbName: pchar): longint;
  cdecl; external 'sqlite3';
function sqlite3_txn_state(Db: PSQLite3; ZSchema: pchar): longint;
  cdecl; external 'sqlite3';
function sqlite3_next_stmt(PDb: PSQLite3; Stmt: PSQLite3Stmt): PSQLite3Stmt;
  cdecl; external 'sqlite3';

{ --- Changes across the connection: commit_hook (nonzero aborts), the
      rollback hook, the update hook per row, the WAL hook (nonzero stops
      checkpointing) and autovacuum_pages' counts (DestroyP as always).
      Each returns the hook it replaced. }

function sqlite3_commit_hook(Db: PSQLite3; XCallback: TSQLite3CommitCallback;
  PArg: pointer): pointer; cdecl; external 'sqlite3';
function sqlite3_rollback_hook(Db: PSQLite3;
  XCallback: TSQLite3RollbackCallback; PArg: pointer): pointer;
  cdecl; external 'sqlite3';
function sqlite3_update_hook(Db: PSQLite3; XCallback: TSQLite3UpdateCallback;
  PArg: pointer): pointer; cdecl; external 'sqlite3';
function sqlite3_wal_hook(Db: PSQLite3; XCallback: TSQLite3WalCallback;
  PArg: pointer): pointer; cdecl; external 'sqlite3';
function sqlite3_autovacuum_pages(Db: PSQLite3; XCallback: pointer;
  PArg: pointer; XDestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';

{ --- Where the memory goes: SHARED_CACHE is the old, disabled-by-default
      cache; release_memory asks the page cache back; the heap limits
      hold memory to N (0 turns off, -1 reads). }

function sqlite3_enable_shared_cache(OnOff: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_release_memory(N: longint): longint; cdecl; external 'sqlite3';
function sqlite3_db_release_memory(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_soft_heap_limit64(N: int64): int64; cdecl; external 'sqlite3';
function sqlite3_hard_heap_limit64(N: int64): int64; cdecl; external 'sqlite3';

{ --- What a column is named and how it sorts: DataType and CollSeq go
      out (nil passes for the string outs and the four flag outs). }

function sqlite3_table_column_metadata(Db: PSQLite3; ZDbName, ZTableName,
  ZColumnName: pchar; var PzDataType: pchar; var PzCollSeq: pchar;
  var PNotNull: longint; var PPrimaryKey: longint;
  var PAutoinc: longint): longint; cdecl; external 'sqlite3';

{ --- Extensions .so files load: enable_load_extension lets them in
      (SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION for only this newer way);
      sqlite3_auto_extension adds an entry point (procedure; cdecl;) to
      run at every open, cancel takes it back. }

function sqlite3_load_extension(Db: PSQLite3; ZFile, ZProc: pchar;
  var PzErrMsg: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_enable_load_extension(Db: PSQLite3; OnOff: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_auto_extension(XEntryPoint: TSQLite3AutoExtensionCallback): longint;
  cdecl; external 'sqlite3';
function sqlite3_cancel_auto_extension(
  XEntryPoint: TSQLite3AutoExtensionCallback): longint;
  cdecl; external 'sqlite3';
procedure sqlite3_reset_auto_extension; cdecl; external 'sqlite3';

{ --- Virtual tables: register the module (a TSQLite3Module the program
      keeps; version 1..4 by the methods filled), declare its tables with
      a create table text, drop the rest, let a table call a function with
      overload_function, set a table's ways with vtab_config, and read
      what xUpdate left in the vtab's ZErrMsg and the ON CONFLICT's way.
      vtab_config takes SQLITE_VTAB_ and a 1/0, and
      vtab_on_conflict/vtab_nochange/vtab_distinct answer their counts. }

function sqlite3_create_module(Db: PSQLite3; ZName: pchar;
  PModule: PSQLite3Module; PClientData: pointer): longint;
  cdecl; external 'sqlite3';
function sqlite3_create_module_v2(Db: PSQLite3; ZName: pchar;
  PModule: PSQLite3Module; PClientData: pointer;
  XDestroyP: TSQLite3Destructor): longint; cdecl; external 'sqlite3';
{ The modules not named in AzKeep go; nil keeps none. }
function sqlite3_drop_modules(Db: PSQLite3; AzKeep: PPChar): longint;
  cdecl; external 'sqlite3';
function sqlite3_declare_vtab(Db: PSQLite3; ZSql: pchar): longint;
  cdecl; external 'sqlite3';
function sqlite3_overload_function(Db: PSQLite3; ZFuncName: pchar;
  NArg: longint): longint; cdecl; external 'sqlite3';
function sqlite3_vtab_config(Db: PSQLite3; Op: longint): longint;
  cdecl; varargs; external 'sqlite3';
function sqlite3_vtab_on_conflict(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_vtab_nochange(Context: PSQLite3Context): longint;
  cdecl; external 'sqlite3';
function sqlite3_vtab_collation(Info: PSQLite3IndexInfo;
  NConstraint: longint): pchar; cdecl; external 'sqlite3';
function sqlite3_vtab_distinct(Info: PSQLite3IndexInfo): longint;
  cdecl; external 'sqlite3';
function sqlite3_vtab_in(Info: PSQLite3IndexInfo; N: longint;
  BHandle: longint): longint; cdecl; external 'sqlite3';
function sqlite3_vtab_in_first(PVal: PSQLite3Value;
  var PPOut: PSQLite3Value): longint; cdecl; external 'sqlite3';
function sqlite3_vtab_in_next(PVal: PSQLite3Value;
  var PPOut: PSQLite3Value): longint; cdecl; external 'sqlite3';
function sqlite3_vtab_rhs_value(Info: PSQLite3IndexInfo; NConstraint: longint;
  var PPVal: PSQLite3Value): longint; cdecl; external 'sqlite3';

{ --- Incremental BLOB I/O: open one cell of one row (Flags 0 or
      SQLITE_OPEN_READONLY), read or write its bytes in place, reopen it
      over another row, close it before the statement writes again. }

function sqlite3_blob_open(Db: PSQLite3; ZDb, ZTable, ZColumn: pchar;
  IRow: int64; Flags: longint; var PPBlob: PSQLite3Blob): longint;
  cdecl; external 'sqlite3';
function sqlite3_blob_reopen(Blob: PSQLite3Blob; IRow: int64): longint;
  cdecl; external 'sqlite3';
function sqlite3_blob_close(Blob: PSQLite3Blob): longint; cdecl; external 'sqlite3';
function sqlite3_blob_bytes(Blob: PSQLite3Blob): longint; cdecl; external 'sqlite3';
function sqlite3_blob_read(Blob: PSQLite3Blob; Z: pointer;
  N, IOffset: longint): longint; cdecl; external 'sqlite3';
function sqlite3_blob_write(Blob: PSQLite3Blob; Z: pointer;
  N, IOffset: longint): longint; cdecl; external 'sqlite3';

{ --- The VFS registry: find by name (nil takes the default), add and
      take one out (a pNext of nil). }

function sqlite3_vfs_find(ZVfsName: pchar): PSQLite3Vfs; cdecl; external 'sqlite3';
function sqlite3_vfs_register(Vfs: PSQLite3Vfs; MakeDflt: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_vfs_unregister(Vfs: PSQLite3Vfs): longint; cdecl; external 'sqlite3';

{ --- Mutexes for code of its own: SQLITE_MUTEX_FAST or RECURSIVE, or a
      SQLITE_MUTEX_STATIC_ (the SQLite core owns those); mutex_held and
      mutex_notheld tell what an assert wants (SQLITE_DEBUG builds only);
      db_mutex is a connection's (its busy one). }

function sqlite3_mutex_alloc(Which: longint): PSQLite3Mutex; cdecl; external 'sqlite3';
procedure sqlite3_mutex_free(Mutex: PSQLite3Mutex); cdecl; external 'sqlite3';
procedure sqlite3_mutex_enter(Mutex: PSQLite3Mutex); cdecl; external 'sqlite3';
function sqlite3_mutex_try(Mutex: PSQLite3Mutex): longint; cdecl; external 'sqlite3';
procedure sqlite3_mutex_leave(Mutex: PSQLite3Mutex); cdecl; external 'sqlite3';
function sqlite3_mutex_held(Mutex: PSQLite3Mutex): longint; cdecl; external 'sqlite3';
function sqlite3_mutex_notheld(Mutex: PSQLite3Mutex): longint; cdecl; external 'sqlite3';
function sqlite3_db_mutex(Db: PSQLite3): PSQLite3Mutex; cdecl; external 'sqlite3';

{ --- Miscellany: one file's control opcode (the PArg the opcode wants),
      SQLite's own test switches, the keyword count, name and check, and
      the status counters. }

function sqlite3_file_control(Db: PSQLite3; ZDbName: pchar; Op: longint;
  PArg: pointer): longint; cdecl; external 'sqlite3';
function sqlite3_test_control(Op: longint): longint; cdecl; varargs;
  external 'sqlite3';
function sqlite3_keyword_count: longint; cdecl; external 'sqlite3';
function sqlite3_keyword_name(N: longint; var PzName: pchar;
  var PnName: longint): longint; cdecl; external 'sqlite3';
function sqlite3_keyword_check(ZName: pchar; NName: longint): longint;
  cdecl; external 'sqlite3';

{ --- The string accumulator: build with the appends, take it with
      str_finish (sqlite3_free it after; nil once out of memory),
      str_errcode, str_length and str_value tell the trouble.
      str_appendf is C's sqlite3_str_appendf. }

function sqlite3_str_new(Db: PSQLite3): PSQLite3Str; cdecl; external 'sqlite3';
function sqlite3_str_finish(Str: PSQLite3Str): pchar; cdecl; external 'sqlite3';
procedure sqlite3_str_free(Str: PSQLite3Str); cdecl; external 'sqlite3';
procedure sqlite3_str_appendf(Str: PSQLite3Str; ZFormat: pchar);
  cdecl; varargs; external 'sqlite3';
procedure sqlite3_str_append(Str: PSQLite3Str; ZIn: pchar; N: longint);
  cdecl; external 'sqlite3';
procedure sqlite3_str_appendall(Str: PSQLite3Str; ZIn: pchar);
  cdecl; external 'sqlite3';
procedure sqlite3_str_appendchar(Str: PSQLite3Str; N: longint; C: char);
  cdecl; external 'sqlite3';
procedure sqlite3_str_reset(Str: PSQLite3Str); cdecl; external 'sqlite3';
procedure sqlite3_str_truncate(Str: PSQLite3Str; N: longint);
  cdecl; external 'sqlite3';
function sqlite3_str_errcode(Str: PSQLite3Str): longint; cdecl; external 'sqlite3';
function sqlite3_str_length(Str: PSQLite3Str): longint; cdecl; external 'sqlite3';
function sqlite3_str_value(Str: PSQLite3Str): pchar; cdecl; external 'sqlite3';

{ --- The counters: sqlite3_status for the library, db_status per
      connection, stmt_status per statement, and the query plans'
      scanstatus — pOut takes an int64 pointer unless the SCANSTAT_ says
      otherwise (NAME and EXPLAIN a pchar, SELECTID and PARENTID a
      longint, COMPLEX a longint of flags, EST can be double). Reset ones
      start their counts again. }

function sqlite3_status(Op: longint; var PCurrent: longint;
  var PHighwater: longint; ResetFlag: longint): longint; cdecl; external 'sqlite3';
function sqlite3_status64(Op: longint; var PCurrent: int64;
  var PHighwater: int64; ResetFlag: longint): longint; cdecl; external 'sqlite3';
function sqlite3_db_status(Db: PSQLite3; Op: longint; var PCurrent: longint;
  var PHighwater: longint; ResetFlag: longint): longint; cdecl; external 'sqlite3';
function sqlite3_db_status64(Db: PSQLite3; Op: longint; var PCurrent: int64;
  var PHighwater: int64; ResetFlag: longint): longint; cdecl; external 'sqlite3';
function sqlite3_stmt_status(Stmt: PSQLite3Stmt; Op: longint;
  ResetFlag: longint): longint; cdecl; external 'sqlite3';

{ --- The online backup: init gives the copy to run (nil once out of
      memory or a bad source), step copies pages (SQLITE_DONE, BUSY or an
      error code; a BUSY retries with the same backup), finish takes the
      rest (its code to check) and frees it. }

function sqlite3_backup_init(PDest: PSQLite3; ZDestName: pchar;
  PSource: PSQLite3; ZSourceName: pchar): PSQLite3Backup; cdecl; external 'sqlite3';
function sqlite3_backup_step(P: PSQLite3Backup; NPage: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_backup_finish(P: PSQLite3Backup): longint; cdecl; external 'sqlite3';
function sqlite3_backup_remaining(P: PSQLite3Backup): longint; cdecl; external 'sqlite3';
function sqlite3_backup_pagecount(P: PSQLite3Backup): longint; cdecl; external 'sqlite3';

{ --- Unlock notify: on SQLITE_LOCKED with a busy timeout, the callback
      (TSQLite3UnlockNotifyCallback) runs once the blocking locks go;
      pNotifyArg comes back as its Data. }

function sqlite3_unlock_notify(PBlocked: PSQLite3;
  XNotify: TSQLite3UnlockNotifyCallback; PNotifyArg: pointer): longint;
  cdecl; external 'sqlite3';

{ --- The string helpers: stricmp and strnicmp compare case-folded,
      strglob is GLOB and strlike is LIKE (Escape 0 or the character). }

function sqlite3_stricmp(Z1, Z2: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_strnicmp(Z1, Z2: pchar; N: longint): longint; cdecl; external 'sqlite3';
function sqlite3_strglob(ZGlob, ZStr: pchar): longint; cdecl; external 'sqlite3';
function sqlite3_strlike(ZGlob, ZStr: pchar; CEsc: cardinal): longint;
  cdecl; external 'sqlite3';

{ --- The log: one line into the configured trace (trace_v2's
      SQLITE_TRACE_STMT callers see the same string). }

procedure sqlite3_log(IErrCode: longint; ZFormat: pchar); cdecl; varargs;
  external 'sqlite3';

{ --- WAL: the autocheckpoint page count (N 0 or 1 turns it off), a
      one-shot passive checkpoint, and the v2 with one
      SQLITE_CHECKPOINT_ mode and the pages in and out. }

function sqlite3_wal_autocheckpoint(Db: PSQLite3; N: longint): longint;
  cdecl; external 'sqlite3';
function sqlite3_wal_checkpoint(Db: PSQLite3; ZDb: pchar): longint;
  cdecl; external 'sqlite3';
function sqlite3_wal_checkpoint_v2(Db: PSQLite3; ZDb: pchar; EMode: longint;
  var PNLog: longint; var PNCkpt: longint): longint; cdecl; external 'sqlite3';

{ --- What the compiled query did: stmt_scanstatus v2 by row (v1 with no
      flags), out by what SCANSTAT_ asks (int64 unless NAME or EXPLAIN
      want a pchar or SELECTID, PARENTID, COMPLEX a longint), reset to
      start the counts again; db_cacheflush writes the dirty pages. }

function sqlite3_stmt_scanstatus(PStmt: PSQLite3Stmt; Idx: longint;
  IScanStatusOp: longint; POut: pointer): longint; cdecl; external 'sqlite3';
function sqlite3_stmt_scanstatus_v2(PStmt: PSQLite3Stmt; Idx: longint;
  IScanStatusOp, Flags: longint; POut: pointer): longint;
  cdecl; external 'sqlite3';
procedure sqlite3_stmt_scanstatus_reset(PStmt: PSQLite3Stmt);
  cdecl; external 'sqlite3';
function sqlite3_db_cacheflush(Db: PSQLite3): longint; cdecl; external 'sqlite3';

{ --- The OS's own error (errno on unix, GetLastError's code on Windows),
      and serialize / deserialize to take a database's bytes out (a
      schema name or 'main'; nil Data with SERIALIZE_NOCOPY) and back in
      (with the FREEONCLOSE, RESIZEABLE and READONLY flags). }

function sqlite3_system_errno(Db: PSQLite3): longint; cdecl; external 'sqlite3';
function sqlite3_serialize(Db: PSQLite3; ZSchema: pchar; var PiSize: int64;
  MFlags: cardinal): pointer; cdecl; external 'sqlite3';
function sqlite3_deserialize(Db: PSQLite3; ZSchema: pchar; Data: pointer;
  SzDb: int64; SzBuf: int64; MFlags: cardinal): longint;
  cdecl; external 'sqlite3';

{ --- The carray-valued function's bind: Data is the array of NData items
      of one SQLITE_CARRAY_ kind, held by SQLITE_STATIC (nil's pointer),
      SQLITE_TRANSIENT or a destroyer of its own; v2 adds the data to
      destroy with it. }

function sqlite3_carray_bind(PStmt: PSQLite3Stmt; N: longint; AData: pointer;
  NData: longint; MFlags: longint; XDel: TSQLite3Destructor): longint;
  cdecl; external 'sqlite3';
function sqlite3_carray_bind_v2(PStmt: PSQLite3Stmt; N: longint;
  AData: pointer; NData: longint; MFlags: longint; XDel: TSQLite3Destructor;
  PDel: pointer): longint; cdecl; external 'sqlite3';

{ --- A database's own file name, parameters and all: parsed out (the
      Nth name in uri_key), the database, journal and wal names of one
      file's name; database_file_object of a main-file name, and
      create_filename's result goes to free_filename. }

function sqlite3_uri_parameter(Z: PSQLite3FileName;
  ZParam: pchar): pchar; cdecl; external 'sqlite3';
function sqlite3_uri_boolean(Z: PSQLite3FileName; ZParam: pchar;
  BDefault: longint): longint; cdecl; external 'sqlite3';
function sqlite3_uri_int64(Z: PSQLite3FileName; ZParam: pchar;
  Dflt: int64): int64; cdecl; external 'sqlite3';
function sqlite3_uri_key(Z: PSQLite3FileName; N: longint): pchar;
  cdecl; external 'sqlite3';
function sqlite3_filename_database(Z: PSQLite3FileName): pchar;
  cdecl; external 'sqlite3';
function sqlite3_filename_journal(Z: PSQLite3FileName): pchar;
  cdecl; external 'sqlite3';
function sqlite3_filename_wal(Z: PSQLite3FileName): pchar;
  cdecl; external 'sqlite3';
function sqlite3_database_file_object(Z: pchar): PSQLite3File;
  cdecl; external 'sqlite3';
function sqlite3_create_filename(ZDatabase, ZJournal, ZWal: pchar;
  NParam: longint; AzParam: PPChar): PSQLite3FileName; cdecl; external 'sqlite3';
procedure sqlite3_free_filename(Z: PSQLite3FileName); cdecl; external 'sqlite3';

implementation

initialization
  { sqlite3's destructor-type word for TRANSIENT, (sqlite3_destructor_type)-1 }
  SQLITE_TRANSIENT_WORD := -1;

end.
