unit libpq;

{ PostgreSQL's libpq, imported from the C headers (src/interfaces/libpq/
  libpq-fe.h, libpq-events.h and postgres_ext.h of the PostgreSQL sources,
  the API behind postgresql.org/docs/current/libpq.html): the routines in
  the libpq that most systems carry, as `cdecl; external 'pq'` declarations
  — naming the library after `external` links the program with -lpq.

  libpq works its own way, unlike most C APIs: every command answers a
  PGresult the program owns until PQclear (PQexec, PQexecParams,
  PQexecPrepared, PQprepare, PQdescribe* and PQclose* all give one); a
  statement's parameters go with the call (paramValues as pchar array,
  paramTypes as Oids, paramFormats 0 = text, 1 = binary); values read out
  of a result are characters (or bytes with the binary format) that live
  while the result does; and errors are strings in PQerrorMessage or
  PQresultErrorField, not codes. Asynchronous callers run PQsendQuery (or
  the other PQsend*), then Poll/Wait on PQsocket feeding PQconsumeInput
  until PQisBusy goes false and PQgetResult answers; rows may already be
  taken with PQsetSingleRowMode or the chunked one before consuming.

  Callbacks. p3 routines reach C by their name (no @): the callback
  parameters of PQsetNoticeReceiver, PQsetNoticeProcessor,
  PQregisterThreadLock, PQsetAuthDataHook,
  PQsetSSLKeyPassHook_OpenSSL, PQregisterEventProc and the two routines
  an OAUTH bearer request carries take the routine's bare name, and the
  routine is program-level or unit-level `cdecl`. The null of each type is
  one of the PQ_NULL_ constants (PQsetNotice* restore the default, the
  hook setters take theirs out); a routine variable takes nil directly,
  where writing a bare nil into a parameter would not compile.

  Strings. The pchar parameters take a literal, and pchar(Q) takes a
  string variable — build the text in a string first: a pchar() cast of a
  string expression does not compile. Results are libpq's (char * from
  libpq's own memory: free with PQfreemem the ones the docs say to, and
  PQfreeCancel, PQconninfoFree, PQclear the structures).

  The one winsock-shaped difference: PQcancelSocket and the altsock an
  OAUTH request's async routine writes are TPGSocket — an int on unix,
  the SOCKET (a qword) on Windows, as the C PQ_SOCKTYPE is.

  Not imported: libpq-int.h internals, ssl.h, and fe-auth callbacks beyond
  what libpq-fe.h exposes. TOid (the C Oid) is a cardinal; PQlibVersion
  tells the library's own version, not this import's. }

{$H+}
{$IFDEF MSWINDOWS}{$DEFINE LIBPQ_SOCKET_QWORD}{$ENDIF}

interface

type
  { The opaque C handles. }
  PPGConn = pointer;           { PGconn *: one connection to one server }
  PPGResult = pointer;         { PGresult *: the answer of one command }
  PPGCancelConn = pointer;     { PGcancelConn *: one cancel connection }
  PPGCancel = pointer;         { PGcancel *: the old, blocking cancel }

  { The C Oid and InvalidOid, with the pointer lists the parameter calls
    take. }
  TOid = cardinal;
  POid = ^TOid;

  { Lists: a nil-ended pchar array (PChar^[i] indexes it), and plain
    longint arrays. }
  PPChar = ^pchar;
  PLongInt = ^longint;

  { The C PQ_SOCKTYPE: an int on unix, the SOCKET (a qword) on Windows. }
  TPGSocket = {$IFDEF LIBPQ_SOCKET_QWORD}qword{$ELSE}longint{$ENDIF};

  { A NOTIFY message: the condition, the sender's process id and the
    payload, and the next one PQnotifies would give. }
  PPGNotify = ^TPGNotify;
  TPGNotify = record
    Relname: pchar;            { the notification condition's name }
    BePid: longint;            { the notifying backend's process id }
    Extra: pchar;              { the notification's payload, or empty }
    Next: PPGNotify;
  end;

  { One entry of PQconndefaults, PQconninfo and PQconninfoParse: an
    option's keyword and its fallbacks, the value set (or nil) and what a
    connect dialog shows. The list ends with a nil Keyword. }
  PPQConninfoOption = ^TPQConninfoOption;
  TPQConninfoOption = record
    Keyword: pchar;
    Envvar: pchar;             { fallback environment variable's name }
    Compiled: pchar;           { fallback compiled-in default }
    Val: pchar;                { the option's current value, or nil }
    Label_: pchar;             { a connect dialog's label }
    Dispchar: pchar;           { '', '*' (hidden), 'D' (debug, not shown) }
    Dispsize: longint;         { the dialog field's width }
  end;

  { PQprint's options (PQuery output, a pqbool being a C char: 1 or 0). }
  PPQPrintOpt = ^TPQPrintOpt;
  TPQPrintOpt = record
    Header: byte;              { field headings and the row count }
    Align: byte;               { fill-aligned columns }
    Standard: byte;            { the old, brain-dead format }
    Html3: byte;               { HTML tables }
    Expanded: byte;            { rows printed vertically }
    Pager: byte;               { page the output }
    FieldSep: pchar;
    TableOpt: pchar;           { an HTML <table ...>'s options }
    Caption: pchar;            { an HTML <caption> }
    FieldName: PPChar;         { nil-ended replacement field names }
  end;

  { The arguments of PQfn: a buffer is bytes if IsInt is 0, otherwise
    IValue is the int. }
  PPQArgBlock = ^TPQArgBlock;
  TPQArgBlock = record
    Len: longint;
    IsInt: longint;
    case longint of
      0: (Ptr: pointer);       { the int's bytes when IsInt is 0 }
      1: (IValue: longint);    { the int for IsInt 1 }
  end;

  { MakeEmptyPGresult's and SetResultAttrs' column attributes. }
  PPGResAttDesc = ^TPGResAttDesc;
  TPGResAttDesc = record
    Name: pchar;               { the column's name }
    Tableid: TOid;             { its source table's Oid, or InvalidOid }
    Columnid: longint;         { the source column's number }
    Format: longint;           { 0 = text, 1 = binary }
    Typid: TOid;               { the type's Oid }
    Typlen: longint;           { the type's size (bytes), -2..-4 the special }
    Atttypmod: longint;        { the type's modifier }
  end;

  { The OAUTH device prompt an auth-data hook shows the user. }
  PPGPromptOAuthDevice = ^TPGPromptOAuthDevice;
  TPGPromptOAuthDevice = record
    VerificationUri: pchar;    { the URI to visit }
    UserCode: pchar;           { the code to enter there }
    VerificationUriComplete: pchar; { URI + code in one, or nil }
    ExpiresIn: longint;        { seconds until the code expires }
  end;

  { An OAUTH bearer token request, for the PQAUTHDATA_OAUTH_BEARER_TOKEN
    hook: the hook reads OpenidConfiguration and Scope, gives token (and
    its User data, the async and cleanup it wants) back in the record. }
  POauthBearerRequest = ^TOauthBearerRequest;
  TPQOAuthAsync = function(Conn: PPGConn;
    Request: POauthBearerRequest; var AltSock: TPGSocket): longint; cdecl;
  TPQOAuthCleanup = procedure(Conn: PPGConn;
    Request: POauthBearerRequest); cdecl;

  TOauthBearerRequest = record
    OpenidConfiguration: pchar; { the OIDC discovery URI in use }
    Scope: pchar;              { the required scope(s), or nil }
    Async: TPQOAuthAsync;      { a custom flow, or nil }
    Cleanup: TPQOAuthCleanup;  { frees token and user (recommended) }
    Token: pchar;              { the bearer token, when the flow is done }
    User: pointer;             { the hook's own data, freed by Cleanup }
  end;

  { Its second shape; Error goes into the connection's PQerrorMessage. }
  TOauthBearerRequestV2 = record
    V1: TOauthBearerRequest;
    Issuer: pchar;             { the issuer in use, from oauth_issuer }
    Error: pchar;              { the hook's own error text, or nil }
  end;

  { The notice callbacks:
      Receiver = procedure(arg: pointer; res: PPGResult); cdecl;
      Processor = procedure(arg: pointer; message: pchar); cdecl; }
  TPQNoticeReceiver = procedure(Arg: pointer; Res: PPGResult); cdecl;
  TPQNoticeProcessor = procedure(Arg: pointer; Msg: pchar); cdecl;
  { The thread lock (a C bool parameter: nonzero acquires). }
  TPQThreadLock = procedure(Acquire: longint); cdecl;
  { The auth-data hook: answers PQresPolling's PGRES_POLLING_ code. }
  TPQAuthDataHook = function(AuthType: longint; Conn: PPGConn;
    Data: pointer): longint; cdecl;
  { The SSL key's password prompt (OpenSSL builds' sslpassphrase). }
  TPQSSLKeyPassHook = function(Buf: pchar; Size: longint;
    Conn: PPGConn): longint; cdecl;
  { The events API's procedure, and the structs its PgEventProc reads: }
  TPQEventProc = function(EvtId: longint; EvtInfo: pointer;
    PassThrough: pointer): longint; cdecl;

  PPGEventRegister = ^TPGEventRegister;
  PPGEventConnReset = ^TPGEventConnReset;
  PPGEventConnDestroy = ^TPGEventConnDestroy;
  PPGEventResultCreate = ^TPGEventResultCreate;
  PPGEventResultCopy = ^TPGEventResultCopy;
  PPGEventResultDestroy = ^TPGEventResultDestroy;
  TPGEventRegister = record
    Conn: PPGConn;
  end;
  TPGEventConnReset = record
    Conn: PPGConn;
  end;
  TPGEventConnDestroy = record
    Conn: PPGConn;
  end;
  TPGEventResultCreate = record
    Conn: PPGConn;
    Result_: PPGResult;
  end;
  TPGEventResultCopy = record
    Src: PPGResult;
    Dest: PPGResult;
  end;
  TPGEventResultDestroy = record
    Result_: PPGResult;
  end;

const
  { The status PQstatus reads, in connect-polling order after the first two. }
  CONNECTION_OK = 0;
  CONNECTION_BAD = 1;
  CONNECTION_STARTED = 2;      { waiting to connect }
  CONNECTION_MADE = 3;         { connected, waiting for the server's hello }
  CONNECTION_AWAITING_RESPONSE = 4;
  CONNECTION_AUTH_OK = 5;      { authenticated, waiting for the backend start }
  CONNECTION_SETENV = 6;       { negotiating the environment, session params }
  CONNECTION_SSL_STARTUP = 7;
  CONNECTION_NEEDED = 8;       { the socket went away }
  CONNECTION_CHECK_WRITABLE = 9;
  CONNECTION_CONSUME = 10;     { consuming the connection's unread data }
  CONNECTION_GSS_STARTUP = 11;
  CONNECTION_CHECK_TARGET = 12;
  CONNECTION_CHECK_STANDBY = 13;
  CONNECTION_ALLOCATED = 14;   { PQconnectStartParams made it, unset yet }
  CONNECTION_AUTHENTICATING = 15;

  { The answers of PQconnectPoll, PQresetPoll and PQcancelPoll. Reading
    and writing say the socket is due (wait on PQcancelSocket for the
    cancel one); PGRES_POLLING_ACTIVE is obsolete and unused. }
  PGRES_POLLING_FAILED = 0;
  PGRES_POLLING_READING = 1;
  PGRES_POLLING_WRITING = 2;
  PGRES_POLLING_OK = 3;
  PGRES_POLLING_ACTIVE = 4;

  { PQresultStatus's kinds. }
  PGRES_EMPTY_QUERY = 0;       { the string was empty }
  PGRES_COMMAND_OK = 1;        { no data, a command it was (create, insert) }
  PGRES_TUPLES_OK = 2;         { data to fetch }
  PGRES_COPY_OUT = 3;          { copy out from the server }
  PGRES_COPY_IN = 4;           { copy in }
  PGRES_BAD_RESPONSE = 5;      { the server's answer made no sense }
  PGRES_NONFATAL_ERROR = 6;    { a notice }
  PGRES_FATAL_ERROR = 7;
  PGRES_COPY_BOTH = 8;         { copy in and out (replication) }
  PGRES_SINGLE_TUPLE = 9;      { one row of SingleRowMode }
  PGRES_PIPELINE_SYNC = 10;    { a pipeline's sync point }
  PGRES_PIPELINE_ABORTED = 11; { this command ran not, a pipeline failure }
  PGRES_TUPLES_CHUNK = 12;     { a chunk of rows, from ChunkedRowsMode }

  { PQtransactionStatus's states. }
  PQTRANS_IDLE = 0;
  PQTRANS_ACTIVE = 1;          { a command is running }
  PQTRANS_INTRANS = 2;         { idle, in a transaction }
  PQTRANS_INERROR = 3;         { idle, in a failed transaction }
  PQTRANS_UNKNOWN = 4;

  { PQpipelineStatus's states. }
  PQ_PIPELINE_OFF = 0;
  PQ_PIPELINE_ON = 1;
  PQ_PIPELINE_ABORTED = 2;

  { PQsetErrorVerbosity's levels. }
  PQERRORS_TERSE = 0;          { one line }
  PQERRORS_DEFAULT = 1;        { the usual fields }
  PQERRORS_VERBOSE = 2;        { all the fields, the backend's context too }
  PQERRORS_SQLSTATE = 3;       { the SQLSTATE only }

  { PQsetErrorContextVisibility's levels (the context the docs show). }
  PQSHOW_CONTEXT_NEVER = 0;
  PQSHOW_CONTEXT_ERRORS = 1;   { the default: only on error }
  PQSHOW_CONTEXT_ALWAYS = 2;

  { PQping's answers. }
  PGPING_OK = 0;               { up and taking connections }
  PGPING_REJECT = 1;           { up, refuses this one }
  PGPING_NO_RESPONSE = 2;      { no answer (down, or blocked) }
  PGPING_NO_ATTEMPT = 3;       { the conninfo made no sense }

  { PQcopyResult's what-to-copy flags. }
  PG_COPYRES_ATTRS = $1;       { the result's columns }
  PG_COPYRES_TUPLES = $2;      { and its rows (implies ATTRS) }
  PG_COPYRES_EVENTS = $4;      { its NOTIFY events }
  PG_COPYRES_NOTICEHOOKS = $8; { its notice hooks }

  { PQsetTraceFlags' flags. }
  PQTRACE_SUPPRESS_TIMESTAMPS = 1;
  PQTRACE_REGRESS_MODE = 2;

  { A limit of the protocol: parameters per statement. }
  PQ_QUERY_PARAM_MAX_LIMIT = 65535;

  { The Oid for "no such object". }
  InvalidOid = 0;

  { PQresultErrorField's field codes (the header no longer names them;
    the SQLSTATE fields of the docs). }
  PG_DIAG_SEVERITY = 83;                { 'S' }
  PG_DIAG_SEVERITY_NONLOCALIZED = 86;   { 'V' }
  PG_DIAG_SQLSTATE = 67;                { 'C' }
  PG_DIAG_MESSAGE_PRIMARY = 77;         { 'M' }
  PG_DIAG_MESSAGE_DETAIL = 68;          { 'D' }
  PG_DIAG_MESSAGE_HINT = 72;            { 'H' }
  PG_DIAG_STATEMENT_POSITION = 80;      { 'P' }
  PG_DIAG_INTERNAL_POSITION = 112;      { 'p' }
  PG_DIAG_INTERNAL_QUERY = 113;         { 'q' }
  PG_DIAG_CONTEXT = 87;                 { 'W' }
  PG_DIAG_SCHEMA_NAME = 115;            { 's' }
  PG_DIAG_TABLE_NAME = 116;             { 't' }
  PG_DIAG_COLUMN_NAME = 99;             { 'c' }
  PG_DIAG_DATATYPE_NAME = 100;          { 'd' }
  PG_DIAG_CONSTRAINT_NAME = 110;        { 'n' }
  PG_DIAG_SOURCE_FILE = 70;             { 'F' }
  PG_DIAG_SOURCE_LINE = 76;             { 'L' }
  PG_DIAG_SOURCE_FUNCTION = 82;         { 'R' }

  { The events API's event ids. The rest of what PgEventProc reads is the
    struct above the call: REGISTER/CONNRESET/CONNDESTROY give
    TPGEventRegister and friends, RESULTCREATE a TPGEventResultCreate. }
  PGEVT_REGISTER = 0;
  PGEVT_CONNRESET = 1;
  PGEVT_CONNDESTROY = 2;
  PGEVT_RESULTCREATE = 3;
  PGEVT_RESULTCOPY = 4;
  PGEVT_RESULTDESTROY = 5;

  { The auth-data hook's kinds (the OAUTH ones the header names now). }
  PQAUTHDATA_PROMPT_OAUTH_DEVICE = 0;
  PQAUTHDATA_OAUTH_BEARER_TOKEN = 1;
  PQAUTHDATA_OAUTH_BEARER_TOKEN_V2 = 2;

  { Null, when a callback may be left out: the constant for each callback
    type this unit's parameters take. For passing, not storing. }
  PQ_NULL_NOTICE_RECEIVER: TPQNoticeReceiver = nil;
  PQ_NULL_NOTICE_PROCESSOR: TPQNoticeProcessor = nil;
  PQ_NULL_THREADLOCK: TPQThreadLock = nil;
  PQ_NULL_AUTHDATA_HOOK: TPQAuthDataHook = nil;
  PQ_NULL_SSLKEY_HOOK: TPQSSLKeyPassHook = nil;

{ --- Making and breaking connections.

      A conninfo is keyword=value pairs (host, port, dbname, user,
      password, sslmode, ...); PQconnectdb is the whole call (nil conn
      says the connection failed, PQerrorMessage says why),
      connectdbParams takes the keywords and values as equal-length nil-
      ended pchar lists. The Start/Poll pairs are for one's own event loop:
      Start makes the conn and answers one status, poll until
      PGRES_POLLING_OK, waiting on PQsocket's readability. }

function PQconnectStart(Conninfo: pchar): PPGConn; cdecl; external 'pq';
function PQconnectStartParams(Keywords, Values: PPChar;
  ExpandDbname: longint): PPGConn; cdecl; external 'pq';
function PQconnectPoll(Conn: PPGConn): longint; cdecl; external 'pq';
function PQconnectdb(Conninfo: pchar): PPGConn; cdecl; external 'pq';
function PQconnectdbParams(Keywords, Values: PPChar;
  ExpandDbname: longint): PPGConn; cdecl; external 'pq';
{ PQsetdbLogin: the old parameters-in-positions form; Port, Options, Tty
  may be nil or the empty string. }
function PQsetdbLogin(Pghost, Pgport, Pgoptions, Pgtty, DbName, Login,
  Pwd: pchar): PPGConn; cdecl; external 'pq';
procedure PQfinish(Conn: PPGConn); cdecl; external 'pq';
procedure PQreset(Conn: PPGConn); cdecl; external 'pq';
function PQresetStart(Conn: PPGConn): longint; cdecl; external 'pq';
function PQresetPoll(Conn: PPGConn): longint; cdecl; external 'pq';

{ --- What the connection is: its open parameters, its status, its
      server's version, the socket (to wait on), the encoding. }

function PQdb(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQuser(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQpass(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQhost(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQhostaddr(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQport(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQtty(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQoptions(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQstatus(Conn: PPGConn): longint; cdecl; external 'pq';
function PQtransactionStatus(Conn: PPGConn): longint; cdecl; external 'pq';
function PQparameterStatus(Conn: PPGConn; ParamName: pchar): pchar;
  cdecl; external 'pq';
function PQprotocolVersion(Conn: PPGConn): longint; cdecl; external 'pq';
function PQfullProtocolVersion(Conn: PPGConn): longint; cdecl; external 'pq';
function PQserverVersion(Conn: PPGConn): longint; cdecl; external 'pq';
function PQerrorMessage(Conn: PPGConn): pchar; cdecl; external 'pq';
function PQsocket(Conn: PPGConn): longint; cdecl; external 'pq';
function PQbackendPID(Conn: PPGConn): longint; cdecl; external 'pq';
function PQpipelineStatus(Conn: PPGConn): longint; cdecl; external 'pq';
function PQconnectionNeedsPassword(Conn: PPGConn): longint; cdecl; external 'pq';
function PQconnectionUsedPassword(Conn: PPGConn): longint; cdecl; external 'pq';
function PQconnectionUsedGSSAPI(Conn: PPGConn): longint; cdecl; external 'pq';
function PQclientEncoding(Conn: PPGConn): longint; cdecl; external 'pq';
function PQsetClientEncoding(Conn: PPGConn; Encoding: pchar): longint;
  cdecl; external 'pq';

{ --- SSL and GSS.

      PQsslInUse says TLS; the attribute table carries the names of
      PQsslAttribute's values (library, version, key_bits, cipher,
      compression, protocol); PQgetssl and PQgetgssctx give the library's
      own handles (OpenSSL's SSL *, a gss_ctx_id_t). }

function PQsslInUse(Conn: PPGConn): longint; cdecl; external 'pq';
function PQsslStruct(Conn: PPGConn; StructName: pchar): pointer;
  cdecl; external 'pq';
function PQsslAttribute(Conn: PPGConn; AttributeName: pchar): pchar;
  cdecl; external 'pq';
function PQsslAttributeNames(Conn: PPGConn): PPChar; cdecl; external 'pq';
{ The obsolete PQgetssl, which PQsslStruct('OpenSSL') replaces. }
function PQgetssl(Conn: PPGConn): pointer; cdecl; external 'pq';
procedure PQinitSSL(DoInit: longint); cdecl; external 'pq';
procedure PQinitOpenSSL(DoSSL, DoCrypto: longint); cdecl; external 'pq';
function PQgssEncInUse(Conn: PPGConn): longint; cdecl; external 'pq';
function PQgetgssctx(Conn: PPGConn): pointer; cdecl; external 'pq';

{ --- Notices, errors and tracing: verbosity and context; a receiver sees
      the PGresult of each notice, a processor the text only (Processor =
      procedure(arg: pointer; message: pchar); cdecl;); both return the
      one they replaced. PQtrace writes the protocol in FPort, the C
      FILE * of fopen. }

function PQsetErrorVerbosity(Conn: PPGConn; Verbosity: longint): longint;
  cdecl; external 'pq';
function PQsetErrorContextVisibility(Conn: PPGConn;
  ShowContext: longint): longint; cdecl; external 'pq';
{ Both answer the one they replaced, as a routine pointer (of Pascal's
  own kind? no: pass it back here, never call through it). }
function PQsetNoticeReceiver(Conn: PPGConn; NoticeProc: TPQNoticeReceiver;
  Arg: pointer): pointer; cdecl; external 'pq';
function PQsetNoticeProcessor(Conn: PPGConn; NoticeProc: TPQNoticeProcessor;
  Arg: pointer): pointer; cdecl; external 'pq';
procedure PQtrace(Conn: PPGConn; DebugPort: pointer); cdecl; external 'pq';
procedure PQuntrace(Conn: PPGConn); cdecl; external 'pq';
procedure PQsetTraceFlags(Conn: PPGConn; Flags: longint); cdecl; external 'pq';

{ --- The connection's option table: defaults (PQSconninfoFree after), the
      connection's, and a conninfo string parsed open. }

function PQconndefaults: PPQConninfoOption; cdecl; external 'pq';
function PQconninfo(Conn: PPGConn): PPQConninfoOption; cdecl; external 'pq';
function PQconninfoParse(Conninfo: pchar;
  var Errmsg: pchar): PPQConninfoOption; cdecl; external 'pq';
procedure PQconninfoFree(ConnOptions: PPQConninfoOption); cdecl; external 'pq';

{ --- Running: PQexec answers one PGresult even for a script (the last
      command's, with all the results' notices shown); a failing
      connection or command gives nil, PQresultStatus's the truth.
      PQexecParams runs one statement with $1.. parameters: ParamTypes
      may be nil (typed by the server), each Values[i] a pchar or, with
      Formats[i] 1, bytes ParamLengths[i] long (Lengths nil means them
      null-ended); the result's format is one Format too. }

function PQexec(Conn: PPGConn; Query: pchar): PPGResult; cdecl; external 'pq';
function PQexecParams(Conn: PPGConn; Command: pchar; NParams: longint;
  ParamTypes: POid; ParamValues: PPChar; ParamLengths: PLongInt;
  ParamFormats: PLongInt; ResultFormat: longint): PPGResult;
  cdecl; external 'pq';
{ The two-step form: prepare once by name, run the prepared any number of
  times (describePrepared's result carries PQnparams and PQparamtype). }
function PQprepare(Conn: PPGConn; StmtName, Query: pchar; NParams: longint;
  ParamTypes: POid): PPGResult; cdecl; external 'pq';
function PQexecPrepared(Conn: PPGConn; StmtName: pchar; NParams: longint;
  ParamValues: PPChar; ParamLengths, ParamFormats: PLongInt;
  ResultFormat: longint): PPGResult; cdecl; external 'pq';

{ --- Running without waiting: send now, poll PQsocket, consume, and take
      each PGresult with PQgetResult. SingleRowMode and ChunkedRowsMode
      apply to the next PQsendQuery's command. }

function PQsendQuery(Conn: PPGConn; Query: pchar): longint; cdecl; external 'pq';
function PQsendQueryParams(Conn: PPGConn; Command: pchar; NParams: longint;
  ParamTypes: POid; ParamValues: PPChar; ParamLengths,
  ParamFormats: PLongInt; ResultFormat: longint): longint; cdecl; external 'pq';
function PQsendPrepare(Conn: PPGConn; StmtName, Query: pchar;
  NParams: longint; ParamTypes: POid): longint; cdecl; external 'pq';
function PQsendQueryPrepared(Conn: PPGConn; StmtName: pchar; NParams: longint;
  ParamValues: PPChar; ParamLengths, ParamFormats: PLongInt;
  ResultFormat: longint): longint; cdecl; external 'pq';
function PQsetSingleRowMode(Conn: PPGConn): longint; cdecl; external 'pq';
function PQsetChunkedRowsMode(Conn: PPGConn; ChunkSize: longint): longint;
  cdecl; external 'pq';
function PQgetResult(Conn: PPGConn): PPGResult; cdecl; external 'pq';
{ Busy: nonzero says the connection's read buffer is not done, and the
  caller should wait (on PQsocket) rather than call PQgetResult yet. }
function PQisBusy(Conn: PPGConn): longint; cdecl; external 'pq';
function PQconsumeInput(Conn: PPGConn): longint; cdecl; external 'pq';

{ --- Pipelining: many commands in one round trip. In pipeline mode the
      PQsend* run without waiting, PQpipelineSync marks the batch
      (PQgetResult then answers PQRES_PIPELINE_SYNC names), and errors
      leave the queue abandoned until the next sync. }
function PQenterPipelineMode(Conn: PPGConn): longint; cdecl; external 'pq';
function PQexitPipelineMode(Conn: PPGConn): longint; cdecl; external 'pq';
function PQpipelineSync(Conn: PPGConn): longint; cdecl; external 'pq';
function PQsendPipelineSync(Conn: PPGConn): longint; cdecl; external 'pq';
function PQsendFlushRequest(Conn: PPGConn): longint; cdecl; external 'pq';

{ --- NOTIFY: PQnotifies gives one message (or nil), taking it off the
      list; ConsumeInput is what brings them in. }
function PQnotifies(Conn: PPGConn): PPGNotify; cdecl; external 'pq';

{ --- COPY. The stream one: PQputCopyData writes (0 = buffer not drained,
      -1 = an error), PQputCopyEnd ends (with a message to fail it),
      getCopyData reads (-1 = done, 1 = the buffer got bytes, 0 = keep
      polling, Async 1 says do not block; the buffer is libpq's until the
      next call, and bytes of -1 say the error needs PQgetResult).
      The line one is the old, text-only pair. }

function PQputCopyData(Conn: PPGConn; Buffer: pchar;
  NBytes: longint): longint; cdecl; external 'pq';
function PQputCopyEnd(Conn: PPGConn; Errormsg: pchar): longint;
  cdecl; external 'pq';
function PQgetCopyData(Conn: PPGConn; var Buffer: pchar;
  Async: longint): longint; cdecl; external 'pq';
function PQgetline(Conn: PPGConn; Buffer: pchar;
  Length: longint): longint; cdecl; external 'pq';
function PQputline(Conn: PPGConn; S: pchar): longint; cdecl; external 'pq';
function PQgetlineAsync(Conn: PPGConn; Buffer: pchar;
  Bufsize: longint): longint; cdecl; external 'pq';
function PQputnbytes(Conn: PPGConn; Buffer: pchar;
  NBytes: longint): longint; cdecl; external 'pq';
function PQendcopy(Conn: PPGConn): longint; cdecl; external 'pq';

{ --- Blocking and flushing: the non-blocking one needs the caller to
      flush (PQflush answers 1 while bytes are still buffered). Threadsafe
      tells whether libpq is safe across threads of one's own; PQping
      tries a connection's handshake. }
function PQsetnonblocking(Conn: PPGConn; Arg: longint): longint;
  cdecl; external 'pq';
function PQisnonblocking(Conn: PPGConn): longint; cdecl; external 'pq';
function PQisthreadsafe: longint; cdecl; external 'pq';
function PQping(Conninfo: pchar): longint; cdecl; external 'pq';
function PQpingParams(Keywords, Values: PPChar;
  ExpandDbname: longint): longint; cdecl; external 'pq';
function PQflush(Conn: PPGConn): longint; cdecl; external 'pq';

{ --- The server's fast-path call: a function id (the pg_proc's Oid) with
      PQArgBlock arguments; the obsolescent binary way, for the protocol's
      function call messages. }
function PQfn(Conn: PPGConn; FnId: longint; var ResultBuf: longint;
  var ResultLen: longint; ResultIsInt: longint; Args: PPQArgBlock;
  NArgs: longint): PPGResult; cdecl; external 'pq';

{ --- Cancelling a running command: the async way is
      cancelCreate, then cancelStart and cancelPoll like a connection
      (or cancelBlocking once, in a thread of its own); the old one is
      getCancel, then one PQcancel into the buffer (or requestCancel's
      connection only). }

function PQcancelCreate(Conn: PPGConn): PPGCancelConn; cdecl; external 'pq';
function PQcancelStart(CancelConn: PPGCancelConn): longint; cdecl; external 'pq';
function PQcancelBlocking(CancelConn: PPGCancelConn): longint;
  cdecl; external 'pq';
function PQcancelPoll(CancelConn: PPGCancelConn): longint; cdecl; external 'pq';
function PQcancelStatus(CancelConn: PPGCancelConn): longint;
  cdecl; external 'pq';
function PQcancelSocket(CancelConn: PPGCancelConn): TPGSocket;
  cdecl; external 'pq';
function PQcancelErrorMessage(CancelConn: PPGCancelConn): pchar;
  cdecl; external 'pq';
procedure PQcancelReset(CancelConn: PPGCancelConn); cdecl; external 'pq';
procedure PQcancelFinish(CancelConn: PPGCancelConn); cdecl; external 'pq';
function PQgetCancel(Conn: PPGConn): PPGCancel; cdecl; external 'pq';
procedure PQfreeCancel(Cancel: PPGCancel); cdecl; external 'pq';
function PQcancel(Cancel: PPGCancel; Errbuf: pchar;
  Errbufsize: longint): longint; cdecl; external 'pq';
function PQrequestCancel(Conn: PPGConn): longint; cdecl; external 'pq';

{ --- The descriptor APIs, and the close ones: prepared statements'
      describe (its PGresult carries the parameter and column types) and
      portals'. }

function PQdescribePrepared(Conn: PPGConn; Stmt: pchar): PPGResult;
  cdecl; external 'pq';
function PQdescribePortal(Conn: PPGConn; Portal: pchar): PPGResult;
  cdecl; external 'pq';
function PQsendDescribePrepared(Conn: PPGConn; Stmt: pchar): longint;
  cdecl; external 'pq';
function PQsendDescribePortal(Conn: PPGConn; Portal: pchar): longint;
  cdecl; external 'pq';
function PQclosePrepared(Conn: PPGConn; Stmt: pchar): PPGResult;
  cdecl; external 'pq';
function PQclosePortal(Conn: PPGConn; Portal: pchar): PPGResult;
  cdecl; external 'pq';
function PQsendClosePrepared(Conn: PPGConn; Stmt: pchar): longint;
  cdecl; external 'pq';
function PQsendClosePortal(Conn: PPGConn; Portal: pchar): longint;
  cdecl; external 'pq';

{ --- The PGresult: status, errors (verbose rebuilds the message at a
      verbosity), status of the command's text, the count of rows and
      columns and each column's names and types. }

function PQresultStatus(Res: PPGResult): longint; cdecl; external 'pq';
function PQresStatus(Status: longint): pchar; cdecl; external 'pq';
function PQresultErrorMessage(Res: PPGResult): pchar; cdecl; external 'pq';
function PQresultVerboseErrorMessage(Res: PPGResult; Verbosity: longint;
  ShowContext: longint): pchar; cdecl; external 'pq';
function PQresultErrorField(Res: PPGResult; Fieldcode: longint): pchar;
  cdecl; external 'pq';

function PQntuples(Res: PPGResult): longint; cdecl; external 'pq';
function PQnfields(Res: PPGResult): longint; cdecl; external 'pq';
function PQbinaryTuples(Res: PPGResult): longint; cdecl; external 'pq';
function PQfname(Res: PPGResult; FieldNum: longint): pchar;
  cdecl; external 'pq';
function PQfnumber(Res: PPGResult; FieldName: pchar): longint;
  cdecl; external 'pq';
function PQftable(Res: PPGResult; FieldNum: longint): TOid; cdecl; external 'pq';
function PQftablecol(Res: PPGResult; FieldNum: longint): longint;
  cdecl; external 'pq';
function PQfformat(Res: PPGResult; FieldNum: longint): longint;
  cdecl; external 'pq';
function PQftype(Res: PPGResult; FieldNum: longint): TOid; cdecl; external 'pq';
function PQfsize(Res: PPGResult; FieldNum: longint): longint;
  cdecl; external 'pq';
function PQfmod(Res: PPGResult; FieldNum: longint): longint;
  cdecl; external 'pq';

{ --- The values: getvalue's text (or binary bytes for a 1-format column,
      null ended as "" and PQgetisnull's judgment), getlength's bytes and
      PQgetisnull's null. A value stays while the PGresult does: copy
      what the program keeps. }
function PQgetvalue(Res: PPGResult; TupNum, FieldNum: longint): pchar;
  cdecl; external 'pq';
function PQgetlength(Res: PPGResult; TupNum, FieldNum: longint): longint;
  cdecl; external 'pq';
function PQgetisnull(Res: PPGResult; TupNum, FieldNum: longint): longint;
  cdecl; external 'pq';
{ The prepared statement's parameters, as its describe result has them. }
function PQnparams(Res: PPGResult): longint; cdecl; external 'pq';
function PQparamtype(Res: PPGResult; ParamNum: longint): TOid;
  cdecl; external 'pq';

{ --- The statement- and portal-level texts: PQcmdStatus (INSERT, SELECT,
      ...), PQoidValue the inserted row's Oid (only for a single INSERT's
      result) and PQcmdTuples the row count text (a command's result
      only), and the obsolete PQoidStatus. }
function PQcmdStatus(Res: PPGResult): pchar; cdecl; external 'pq';
function PQoidStatus(Res: PPGResult): pchar; cdecl; external 'pq';
function PQoidValue(Res: PPGResult): TOid; cdecl; external 'pq';
function PQcmdTuples(Res: PPGResult): pchar; cdecl; external 'pq';

{ --- Results the program makes itself, for the events API or an
      extension: MakeEmptyPGresult (a command's status), copyResult
      (the PG_COPYRES_ flags), SetResultAttrs from a TPGResAttDesc list,
      resultAlloc (freed by PQclear), Setvalue to put a value in, and
      resultMemorySize's bytes. }
function PQmakeEmptyPGresult(Conn: PPGConn; Status: longint): PPGResult;
  cdecl; external 'pq';
function PQcopyResult(Src: PPGResult; Flags: longint): PPGResult;
  cdecl; external 'pq';
function PQsetResultAttrs(Res: PPGResult; NumAttributes: longint;
  AttDescs: PPGResAttDesc): longint; cdecl; external 'pq';
function PQresultAlloc(Res: PPGResult; NBytes: qword): pointer;
  cdecl; external 'pq';
function PQresultMemorySize(Res: PPGResult): qword; cdecl; external 'pq';
function PQsetvalue(Res: PPGResult; TupNum, FieldNum: longint;
  Value: pchar; Len: longint): longint; cdecl; external 'pq';

{ --- Freeing, and the strings the escape and encrypt routines give. }
procedure PQclear(Res: PPGResult); cdecl; external 'pq';
procedure PQfreemem(Ptr: pointer); cdecl; external 'pq';

{ --- Escaping: a connection-aware copy of Text with the server's
      encoding and quoting (answers the written bytes into To, Error 1 on
      bad bytes), literal and identifier with the quotes, and the bytea
      text hex form (PQfreemem after); the connless PQescapeString and
      PQescapeBytea take the compiled-in encoding. UnescapeBytea takes
      the text of it (StrToBytes' work too) and answers the bytes. }
function PQescapeStringConn(Conn: PPGConn; To_: pchar; From: pchar;
  Length: qword; var Error: longint): qword; cdecl; external 'pq';
function PQescapeLiteral(Conn: PPGConn; Str: pchar;
  Len: qword): pchar; cdecl; external 'pq';
function PQescapeIdentifier(Conn: PPGConn; Str: pchar;
  Len: qword): pchar; cdecl; external 'pq';
function PQescapeByteaConn(Conn: PPGConn; From: pointer;
  FromLength: qword; var ToLength: qword): pointer; cdecl; external 'pq';
function PQunescapeBytea(Strtext: pointer;
  var Retbuflen: qword): pointer; cdecl; external 'pq';
function PQescapeString(To_: pchar; From: pchar;
  Length: qword): qword; cdecl; external 'pq';
function PQescapeBytea(From: pointer; FromLength: qword;
  var ToLength: qword): pointer; cdecl; external 'pq';

{ --- Encrypted passwords and the change-password command; the algorithm
      nil means the server's default. }
function PQencryptPassword(Passwd, User: pchar): pchar; cdecl; external 'pq';
function PQencryptPasswordConn(Conn: PPGConn; Passwd, User,
  Algorithm: pchar): pchar; cdecl; external 'pq';
function PQchangePassword(Conn: PPGConn; User, Passwd: pchar): PPGResult;
  cdecl; external 'pq';

{ --- Print the result on a FILE * (Fout, the C fopen's pointer):
      PQprint with its options, and the two older, fixed formats. }
procedure PQprint(Fout: pointer; Res: PPGResult;
  Po: PPQPrintOpt); cdecl; external 'pq';
procedure PQdisplayTuples(Res: PPGResult; Fp: pointer; FillAlign: longint;
  FieldSep: pchar; PrintHeader: longint; Quiet: longint);
  cdecl; external 'pq';
procedure PQprintTuples(Res: PPGResult; Fout: pointer;
  PrintAttNames: longint; TerseOutput: longint; ColWidth: longint);
  cdecl; external 'pq';

{ --- Version, clock and polling (for its own socket, a TPGSocket of
      windows'): PQsocketPoll waits until a socket is readable or
      writable by EndTime (or -1 to wait), and answers 1 ready, 0 not,
      -1 error; getCurrentTimeUSec is the usec clock. }
function PQlibVersion: longint; cdecl; external 'pq';
function PQsocketPoll(Sock: TPGSocket; ForRead, ForWrite: longint;
  EndTime: int64): longint; cdecl; external 'pq';
function PQgetCurrentTimeUSec: int64; cdecl; external 'pq';

{ --- Character-length helpers for UTF-8 and friends (encoding is the
      PQsetClientEncoding's value): how many bytes the character at the
      front of S is, and how wide it prints; env2encoding reads $PGCLIENTENCODING. }
function PQmblen(S: pchar; Encoding: longint): longint; cdecl; external 'pq';
function PQmblenBounded(S: pchar; Encoding: longint): longint;
  cdecl; external 'pq';
function PQdsplen(S: pchar; Encoding: longint): longint; cdecl; external 'pq';
function PQenv2encoding: longint; cdecl; external 'pq';

{ --- Encodings of one's own: name in, its number out, and the reverse. }
function pg_char_to_encoding(Name: pchar): longint; cdecl; external 'pq';
function pg_encoding_to_char(Encoding: longint): pchar; cdecl; external 'pq';
function pg_valid_server_encoding_id(Encoding: longint): longint;
  cdecl; external 'pq';

{ --- Thread locks (the libpq-internal lock of code one calls libpq
      from): register one, or PQ_NULL_THREADLOCK to take it out;
      getThreadLock needs the LIBPQ_HAS_GET_THREAD_LOCK library. }
{ The one it replaced (a routine pointer, to pass back, not to call). }
function PQregisterThreadLock(NewHandler: TPQThreadLock): pointer;
  cdecl; external 'pq';
function PQgetThreadLock: pointer; cdecl; external 'pq';

{ --- The auth-data hook of every connection: it answers for the
      PQAUTHDATA_ kinds with the data the kind's struct carries (an OAUTH
      device prompt, a bearer request). }
procedure PQsetAuthDataHook(Hook: TPQAuthDataHook); cdecl; external 'pq';
function PQgetAuthDataHook: pointer; cdecl; external 'pq';
{ The default hook: answers 0 for everything, for hook code to call. }
function PQdefaultAuthDataHook(AuthType: longint; Conn: PPGConn;
  Data: pointer): longint; cdecl; external 'pq';

{ --- The SSL key file's password prompt (OpenSSL); nil takes it out.
      The default (for one's own hook to call) reads the sslpassword
      connection parameter. }
function PQgetSSLKeyPassHook_OpenSSL: pointer; cdecl; external 'pq';
procedure PQsetSSLKeyPassHook_OpenSSL(
  Hook: TPQSSLKeyPassHook); cdecl; external 'pq';
function PQdefaultSSLKeyPassHook_OpenSSL(Buf: pchar; Size: longint;
  Conn: PPGConn): longint; cdecl; external 'pq';

{ --- The events API: register a proc (named, with pass-through data),
      connect and result instance data per event proc. The proc answers
      nonzero to fail (a CREATE/COPY failure fails the result's work). }
function PQregisterEventProc(Conn: PPGConn; Proc: TPQEventProc;
  Name: pchar; PassThrough: pointer): longint; cdecl; external 'pq';
function PQsetInstanceData(Conn: PPGConn; Proc: TPQEventProc;
  Data: pointer): longint; cdecl; external 'pq';
function PQinstanceData(Conn: PPGConn; Proc: TPQEventProc): pointer;
  cdecl; external 'pq';
function PQresultSetInstanceData(Res: PPGResult; Proc: TPQEventProc;
  Data: pointer): longint; cdecl; external 'pq';
function PQresultInstanceData(Res: PPGResult;
  Proc: TPQEventProc): pointer; cdecl; external 'pq';
function PQfireResultCreateEvents(Conn: PPGConn;
  Res: PPGResult): longint; cdecl; external 'pq';

{ --- Large objects, an Oid-addressed byte store inside the server, read
      and written through the connection's descriptors. Modes: the C
      INV_READ is 0x00020000 and INV_WRITE 0x00040000 (they are the
      backend's, not here as constants). The 64 forms take the offsets. }
function lo_open(Conn: PPGConn; LobjId: TOid; Mode: longint): longint;
  cdecl; external 'pq';
function lo_close(Conn: PPGConn; Fd: longint): longint; cdecl; external 'pq';
function lo_read(Conn: PPGConn; Fd: longint; Buf: pchar;
  Len: qword): longint; cdecl; external 'pq';
function lo_write(Conn: PPGConn; Fd: longint; Buf: pchar;
  Len: qword): longint; cdecl; external 'pq';
function lo_lseek(Conn: PPGConn; Fd, Offset, Whence: longint): longint;
  cdecl; external 'pq';
function lo_lseek64(Conn: PPGConn; Fd: longint; Offset: int64;
  Whence: longint): int64; cdecl; external 'pq';
function lo_creat(Conn: PPGConn; Mode: longint): TOid; cdecl; external 'pq';
function lo_create(Conn: PPGConn; LobjId: TOid): TOid; cdecl; external 'pq';
function lo_tell(Conn: PPGConn; Fd: longint): longint; cdecl; external 'pq';
function lo_tell64(Conn: PPGConn; Fd: longint): int64; cdecl; external 'pq';
function lo_truncate(Conn: PPGConn; Fd: longint; Len: qword): longint;
  cdecl; external 'pq';
function lo_truncate64(Conn: PPGConn; Fd: longint; Len: int64): longint;
  cdecl; external 'pq';
function lo_unlink(Conn: PPGConn; LobjId: TOid): longint; cdecl; external 'pq';
function lo_import(Conn: PPGConn; Filename: pchar): TOid; cdecl; external 'pq';
function lo_import_with_oid(Conn: PPGConn; Filename: pchar;
  LobjId: TOid): TOid; cdecl; external 'pq';
function lo_export(Conn: PPGConn; LobjId: TOid;
  Filename: pchar): longint; cdecl; external 'pq';

implementation

end.
