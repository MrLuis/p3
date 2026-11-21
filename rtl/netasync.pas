{ netasync — awaitable BSD sockets for p3's async/await runtime.
  TCP over IPv4, on Linux and macOS (Windows: later). The entry points that
  can wait are async routines: called from a task they park it (the OS
  thread goes on with other tasks) until the socket is ready, through one
  shared poller thread and the runtime's task scheduler — so a cancelled
  token wakes a parked task with EOperationCancelled, and no thread ever
  blocks in a syscall.

  Called from plain (non-async) code, the same functions block the calling
  thread instead of parking it.

  One socket has at most one wait in flight (the usual pattern: read until
  answered, write, repeat). No fcntl/ioctl is used — p3-compiled threads
  cannot call variadic C functions — so sockets are created with
  SOCK_NONBLOCK, accepted sockets inherit the flag (Linux through accept4,
  macOS through the listener's state), and SIGPIPE is avoided with
  MSG_NOSIGNAL on Linux and SO_NOSIGPIPE on macOS. Needs --mode=objfpc. }
unit netasync;

{$modeswitch typehelpers}
{$H+}

interface

{$IFNDEF P3_OBJFPC}
uses netasync_needs_mode_objfpc;
{$ELSE}

type
  TSocket = longint;
  TNetBytes = array of Byte;

const
  InvalidSocket = -1;

type
  ENetError = class(Exception);

{ ---- blocking helpers (no parking; safe anywhere) ---- }

{ A socket listening on port (0: any free port), on every address if
  AnyAddress; InvalidSocket if that fails; the socket is non-blocking. }
function NetListen(Port, Backlog: Integer; AnyAddress: Boolean): TSocket;
{ The port a listening socket is bound to (0 if unknown). }
function NetBoundPort(S: TSocket): Integer;
procedure NetClose(S: TSocket);
procedure NetSetNoDelay(S: TSocket);
procedure NetSetKeepAlive(S: TSocket);

{ ---- async entry points (park the calling task when they would block) ---- }

{ The next connection of the listener (already non-blocking); raises
  ENetError on an error. }
function NetAccept(Listener: TSocket): async TSocket;
{ A connection to host:port (the host must be in IPv4 form for now);
  raises ENetError on an error. }
function NetConnect(const Host: string; Port: Integer): async TSocket;
{ Up to Max bytes, whichever comes first; an empty array at the end of the
  connection; raises ENetError on an error. }
function NetReadSome(S: TSocket; Max: Integer): async TNetBytes;
{ Sends every byte or raises ENetError; returns the count sent. }
function NetSendAll(S: TSocket; const Data: TNetBytes): async LongInt;

{ One recv into buf (up to Max); the count, 0 at the end of the
  connection. Parks the task at most once (the caller retries). }
function NetReadInto(S: TSocket; buf: pointer; Max: Integer): async LongInt;
{ Parks the current task until S is readable (WantRead) or writable
  (WantWrite, winning if both); also works for foreign descriptors (like
  a database driver's fd). }
procedure NetWaitReady(S: TSocket; WantRead, WantWrite: Boolean);

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}
{$IFNDEF LINUX}{$IFNDEF DARWIN}
uses netasync_needs_linux_or_darwin;
{$ENDIF}{$ENDIF}

uses
  sysutils;

type
  cint = longint;
  cuint = cardinal;
  ssize_t = int64;

const
  AF_INET = 2;
  SOCK_STREAM = 1;
{$IFDEF LINUX}
  SOL_SOCKET = 1;
  SO_REUSEADDR = 2;
  SO_KEEPALIVE = 9;
  SO_ERROR = 4;
  MSG_NOSIGNAL = $4000;
  EINPROGRESS = 115;
  EAGAIN = 11;
  EWOULDBLOCK = 11;
  EINTR = 4;
{$ENDIF}
{$IFDEF DARWIN}
  SOL_SOCKET = $ffff;
  SO_REUSEADDR = 4;
  SO_KEEPALIVE = 8;
  SO_ERROR = $1007;
  SO_NOSIGPIPE = $1022;
  MSG_NOSIGNAL = 0;
  EINPROGRESS = 36;
  EAGAIN = 35;
  EWOULDBLOCK = 35;
  EINTR = 4;
{$ENDIF}
  IPPROTO_TCP = 6;
  TCP_NODELAY = 1;

var
  errno: cint; external 'c' name 'errno';

procedure libc_close(s: cint); cdecl; external 'c' name 'close';
function socket(af, t, p: cint): cint; cdecl; external 'c' name 'socket';
function bind(s: cint; addr: pointer; len: cint): cint; cdecl; external 'c' name 'bind';
function listen(s, backlog: cint): cint; cdecl; external 'c' name 'listen';
function accept(s: cint; addr: pointer; len: pointer): cint; cdecl;
  external 'c' name 'accept';
function connect(s: cint; addr: pointer; len: cint): cint; cdecl;
  external 'c' name 'connect';
function recv(s: cint; buf: pointer; len: cint; flags: cint): ssize_t; cdecl;
  external 'c' name 'recv';
function send(s: cint; buf: pointer; len: cint; flags: cint): ssize_t; cdecl;
  external 'c' name 'send';
procedure setsockopt(s, level, optname: cint; optval: pointer; optlen: cuint); cdecl;
  external 'c' name 'setsockopt';
function getsockopt(s, level, optname: cint; optval: pointer; len: pointer): cint; cdecl;
  external 'c' name 'getsockopt';
function getsockname(s: cint; addr: pointer; len: pointer): cint; cdecl;
  external 'c' name 'getsockname';
function inet_pton(af: cint; const src: pchar; dst: pointer): cint; cdecl;
  external 'c' name 'inet_pton';
{$IFDEF LINUX}
function accept4(s: cint; addr: pointer; len: pointer; flags: cint): cint; cdecl;
  external 'c' name 'accept4';
{$ENDIF}

{ parks the current task on fd readiness (unit netasync's heart) }
procedure pas_task_wait_fd(fd: cint; want_write: cint); cdecl;
  external name '__pas_task_wait_fd';
procedure pas_fd_nonblock(fd: cint; on: cint); cdecl;
  external name '__pas_fd_nonblock';
procedure pas_fd_poller_start; cdecl; external name '__pas_fd_poller_start';

type
  TSockAddrIn = record
{$IFDEF DARWIN}
    len: byte;
    family: byte;
{$ELSE}
    family: word;
{$ENDIF}
    port: word;        { network order }
    addr: cardinal;    { network order }
    zero: array[0..7] of byte;
  end;

function HostToNet16(v: word): word; inline;
begin
  Result := (v shr 8) or ((v and $ff) shl 8);
end;

{ ---- blocking helpers ---- }

procedure NetClose(S: TSocket);
begin
  libc_close(S);
end;

procedure NetSetNoDelay(S: TSocket);
var
  one: cint;
begin
  one := 1;
  setsockopt(S, IPPROTO_TCP, TCP_NODELAY, @one, cuint(sizeof(one)));
end;

procedure NetSetKeepAlive(S: TSocket);
var
  one: cint;
begin
  one := 1;
  setsockopt(S, SOL_SOCKET, SO_KEEPALIVE, @one, cuint(sizeof(one)));
end;

function NetListen(Port, Backlog: Integer; AnyAddress: Boolean): TSocket;
var
  addr: TSockAddrIn;
  one: cint;
  s: TSocket;
begin
  s := socket(AF_INET, SOCK_STREAM, 0);
  if s < 0 then
    raise ENetError.Create('net: socket failed (' + IntToStr(errno) + ')');
{$IFDEF DARWIN}
  one := 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, @one, cuint(sizeof(one)));
{$ENDIF}
  pas_fd_nonblock(s, 1);
  one := 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, @one, cuint(sizeof(one)));
  fillchar(addr, sizeof(addr), 0);
{$IFDEF DARWIN}
  addr.len := sizeof(TSockAddrIn);
{$ENDIF}
  addr.family := AF_INET;
  addr.port := HostToNet16(word(Port and $ffff));
  if AnyAddress then
    addr.addr := 0
  else
    addr.addr := $7f000001;   { 127.0.0.1 }
  if bind(s, @addr, sizeof(addr)) <> 0 then
  begin
    libc_close(s);
    raise ENetError.Create('net: bind failed (' + IntToStr(errno) + ')');
  end;
  if listen(s, Backlog) <> 0 then
  begin
    libc_close(s);
    raise ENetError.Create('net: listen failed (' + IntToStr(errno) + ')');
  end;
  Result := s;
end;

function NetBoundPort(S: TSocket): Integer;
var
  addr: TSockAddrIn;
  len: cint;
begin
  fillchar(addr, sizeof(addr), 0);
  len := sizeof(addr);
  if getsockname(S, @addr, @len) <> 0 then
    Result := 0
  else
    Result := HostToNet16(word(addr.port));
end;

{ ---- the async entry points ---- }

function NetAccept(Listener: TSocket): async TSocket;
var
  n: TSocket;
begin
  while true do
  begin
    n := accept(Listener, nil, nil);
    if n >= 0 then
    begin
{$IFDEF LINUX}
      { the listener's nonblocking flag is not inherited: set it }
      pas_fd_nonblock(n, 1);
{$ENDIF}
      exit(n);
    end;
    if errno = EINTR then
      continue;
    if (errno = EAGAIN) or (errno = EWOULDBLOCK) then
    begin
      NetWaitReady(Listener, true, false);
      continue;
    end;
    raise ENetError.Create('net: accept failed (' + IntToStr(errno) + ')');
  end;
end;

function NetConnect(const Host: string; Port: Integer): async TSocket;
var
  addr: TSockAddrIn;
  s, rc: TSocket;
  e, err: cint;
begin
  s := socket(AF_INET, SOCK_STREAM, 0);
  if s < 0 then
    raise ENetError.Create('net: socket failed (' + IntToStr(errno) + ')');
{$IFDEF DARWIN}
  e := 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, @e, cuint(sizeof(e)));
{$ENDIF}
  pas_fd_nonblock(s, 1);
  fillchar(addr, sizeof(addr), 0);
{$IFDEF DARWIN}
  addr.len := sizeof(TSockAddrIn);
{$ENDIF}
  addr.family := AF_INET;
  addr.port := HostToNet16(word(Port and $ffff));
  if inet_pton(AF_INET, pchar(Host), @addr.addr) <= 0 then
  begin
    libc_close(s);
    raise ENetError.Create('net: not an IPv4 address: ' + Host);
  end;
  rc := connect(s, @addr, sizeof(addr));
  if rc < 0 then
  begin
    if (errno <> EINPROGRESS) and (errno <> EAGAIN) then
    begin
      libc_close(s);
      raise ENetError.Create('net: connect failed (' + IntToStr(errno) + ')');
    end;
    NetWaitReady(s, false, true);
    err := 0;
    e := sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, @err, @e) <> 0) or (err <> 0) then
    begin
      libc_close(s);
      raise ENetError.Create('net: connect failed (' + IntToStr(err) + ')');
    end;
  end;
  Result := s;
end;

function NetReadSome(S: TSocket; Max: Integer): async TNetBytes;
var
  one: array[0..4095] of byte;
  out: TNetBytes;
  m: cint;
  n: ssize_t;
begin
  if Max > sizeof(one) then
    m := sizeof(one)
  else if Max > 0 then
    m := Max
  else
    m := 1;
  Result := nil;
  while true do
  begin
    n := recv(S, @one, m, 0);
    if n > 0 then
    begin
      SetLength(out, n);
      move(one, out[0], n);
      Result := out;
      exit;
    end;
    if n = 0 then
    begin
      SetLength(out, 0);
      Result := out;
      exit;
    end;
    if errno = EINTR then
      continue;
    if (errno <> EAGAIN) and (errno <> EWOULDBLOCK) then
      raise ENetError.Create('net: read failed (' + IntToStr(errno) + ')');
    NetWaitReady(S, true, false);
  end;
end;

function NetSendAll(S: TSocket; const Data: TNetBytes): async LongInt;
var
  off, len: longint;
  chunk: cint;
  n: ssize_t;
begin
  off := 0;
  len := Length(Data);
  while off < len do
  begin
    if len - off > 262144 then
      chunk := 262144
    else
      chunk := len - off;
    if chunk > 0 then
      n := send(S, @Data[off], chunk, MSG_NOSIGNAL)
    else
      n := 0;
    if n > 0 then
    begin
      inc(off, n);
      continue;
    end;
    if (n = 0) and (chunk = 0) then
      continue;
    if errno = EINTR then
      continue;
    if (errno <> EAGAIN) and (errno <> EWOULDBLOCK) then
      raise ENetError.Create('net: send failed (' + IntToStr(errno) + ')');
    NetWaitReady(S, false, true);
  end;
  Result := off;
end;

function NetReadInto(S: TSocket; buf: pointer; Max: Integer): async LongInt;
var
  n: ssize_t;
begin
  while true do
  begin
    n := recv(S, buf, Max, 0);
    if n > 0 then
      exit(n);
    if n = 0 then
      exit(0);
    if errno = EINTR then
      continue;
    if (errno <> EAGAIN) and (errno <> EWOULDBLOCK) then
      raise ENetError.Create('net: read failed (' + IntToStr(errno) + ')');
    NetWaitReady(S, true, false);
  end;
end;

procedure NetWaitReady(S: TSocket; WantRead, WantWrite: Boolean);
begin
  if WantWrite then
    pas_task_wait_fd(S, 1)
  else if WantRead then
    pas_task_wait_fd(S, 0);
end;

{$ENDIF}

end.
