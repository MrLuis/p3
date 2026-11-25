{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
{ netasync — awaitable BSD sockets for p3's async/await runtime.
  TCP over IPv4, on Linux and macOS (Windows: later). The entry points that
  can wait are async routines: called from a task they park it (the OS
  thread goes on with other tasks) until the socket is ready, through the
  poller of the task's own thread (kqueue on macOS, epoll on Linux; the
  shared io_uring ring with P3_NET_URING=1) and the runtime's task
  scheduler — so a cancelled token wakes a parked task with
  EOperationCancelled, and no thread ever blocks in a syscall. Readiness
  is delivered on the thread that runs the task, in batches, with no
  thread hop: the shape of a hand-written per-core event loop.

  Called from plain (non-async) code, the same functions block the calling
  thread instead of parking it.

  A descriptor may have several waits in flight at once, across tasks and
  threads (two accepts on one listener, a pool task and a driver task on
  one database fd). Every waiter gets its own registration, the readiness
  of the underlying socket wakes all of them, and the extra ones just see
  the transient EAGAIN the next syscall would produce and re-park. The
  usual pattern for one task stays: read until answered, write, repeat.

  Cancellation and wait records: a wait cancelled on the io_uring backend
  abandons its record on purpose — a completion may follow the task's rise,
  or never come — and leaks it, ~72 bytes per cancellation, counted by
  NetWaitsAbandoned (the poll backends retire their records cleanly, so the
  counter stays 0 there). Leak tests compare that counter, not the
  allocator.

  No fcntl/ioctl is used — p3-compiled threads cannot call variadic C
  functions — so sockets are created with SOCK_NONBLOCK, accepted sockets
  inherit the flag (Linux through accept4, macOS through the listener's
  state), and SIGPIPE is avoided with MSG_NOSIGNAL on Linux and SO_NOSIGPIPE
  on macOS. Needs --mode=objfpc. }
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

  { The IPv4 socket address, as NetMakePeer/NetPeerString/NetUdpRecvFrom
    pass it around by pointer. }
  PSockAddrIn = ^TSockAddrIn;
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

  { Flags for NetListenTo, applied before the bind. }
  TNetListenOpts = record
    ReuseAddr: boolean;  { SO_REUSEADDR }
    ReusePort: boolean;  { SO_REUSEPORT: several sockets may bind the same
                          (host, port) together and the kernel balances
                          the accepts among them (Linux, macOS; Windows
                          has none: ENetError) }
  end;

const
  InvalidSocket = -1;

  { Bits NetWaitFd/NetWaitFdUntil report: what fired at the wake. }
  NW_READ = 1;    { the fd is readable }
  NW_WRITE = 2;   { the fd is writable }
  NW_FAIL = 4;    { the fd is in an error state (POLLERR/POLLHUP, or a
                   failed poll completion on io_uring); a peer that
                   closed sending gracefully reports NW_READ and an
                   empty read, not the fail bit }
  NW_TIMEOUT = 8; { only NetWaitFdUntil's deadline expired first; unlike
                   a cancelled wait, the record completes and no leak
                   follows }

type
  ENetError = class(Exception);

{ ---- blocking helpers (no parking; safe anywhere) ---- }

{ A socket listening on port (0: any free port), on every address if
  AnyAddress; InvalidSocket if that fails; the socket is non-blocking. }
function NetListen(Port, Backlog: Integer; AnyAddress: Boolean): TSocket;
{ The port a listening socket is bound to (0 if unknown). }
function NetBoundPort(S: TSocket): Integer;
{ A socket listening on host (an IPv4 literal; '' = every address) and
  port (0: any free port), with the opts' SO_* flags set before the bind;
  raises ENetError on failure; the socket is non-blocking. }
function NetListenTo(const host: string; Port, Backlog: Integer;
                     const opt: TNetListenOpts): TSocket;
{ Marks a descriptor created elsewhere (a listener bound by the program's
  own socket code, a database driver's fd) non-blocking, as the async
  entry points expect. }
procedure NetAdopt(S: TSocket);
procedure NetClose(S: TSocket);
procedure NetSetNoDelay(S: TSocket);
procedure NetSetKeepAlive(S: TSocket);
{ TCP keepalive timing for S, which must have SO_KEEPALIVE on: the first
  probe IdleSec after the last traffic, then every IntvlSec, with Count
  unanswered probes before the connection is considered dead. }
procedure NetSetKeepAliveTimer(S: TSocket; IdleSec, IntvlSec, Count: Integer);
{ Half-closes S's write side: everything already written is delivered,
  the peer sees the end of our send, and our own receiving continues
  until we close. }
procedure NetShutdownWrite(S: TSocket);
{ The wait records cancelled awaits have abandoned by design (~72 B each;
  a cancelled io_uring wait cannot free its record because a completion
  may still arrive). Leak tests compare this counter, not the allocator. }
function NetWaitsAbandoned: Int64;

{ ---- UDP (datagram sockets; poll-based waits like everything else) ---- }

{ A UDP socket bound to host (an IPv4 literal; '' = every address) and
  port (0 = any free port); the socket is non-blocking, raises ENetError
  on failure. }
function NetUdpBind(const host: string; Port: Integer): TSocket;
{ A UDP socket bound nowhere and connected to host:port: NetUdpSend and
  NetUdpRecv (or NetSendPtr/NetReadInto) talk to it. }
function NetUdpConnect(const Host: string; Port: Integer): TSocket;
{ connect()s an existing UDP socket: NetUdpSend/NetUdpRecv talk to the
  peer, and datagrams from anyone else are dropped by the kernel. }
procedure NetUdpPeerTo(S: TSocket; const Host: string; Port: Integer);
{ Fills peer (a TSockAddrIn) with host:port; raises on a non-IPv4 host. }
procedure NetMakePeer(const Host: string; Port: Integer; peer: pointer);
{ peer as 'ip:port'; '' if it is not an IPv4 address. }
function NetPeerString(peer: pointer): string;
{ One datagram to peer. Parks the task on full socket buffers. }
function NetUdpSendTo(S: TSocket; peer: pointer; buf: pointer;
                      len: LongInt): async LongInt;
{ One datagram to the socket's connected peer. }
function NetUdpSend(S: TSocket; buf: pointer; len: LongInt): async LongInt;
{ One datagram into buf, and its source address into src (a TSockAddrIn
  owned by the caller, or nil to drop it). Parks the task while there is
  no datagram. }
function NetUdpRecvFrom(S: TSocket; buf: pointer; Max: Integer;
                        src: pointer): async LongInt;
{ One datagram from the socket's connected peer. }
function NetUdpRecv(S: TSocket; buf: pointer; Max: Integer): async LongInt;

{ ---- resolution (the lookup blocks, so it runs on a pool thread) ---- }

{ host to its IPv4 dotted quad (Pool.Run does the blocking getaddrinfo);
  '' when it does not resolve. }
function NetResolve(const Host: string): async string;

{ ---- async entry points (park the calling task when they would block) ---- }

{ The next connection of the listener (already non-blocking); raises
  ENetError on an error. }
function NetAccept(Listener: TSocket): async TSocket;
{ A connection to host:port (an IPv4 literal, or a name resolved with
  getaddrinfo on a pool thread, issued AF_INET); raises ENetError on an
  error. }
function NetConnect(const Host: string; Port: Integer): async TSocket;
{ Up to Max bytes, whichever comes first; an empty array at the end of the
  connection; raises ENetError on an error. }
function NetReadSome(S: TSocket; Max: Integer): async TNetBytes;
{ Sends every byte or raises ENetError; returns the count sent. }
function NetSendAll(S: TSocket; const Data: TNetBytes): async LongInt;

{ One recv into buf (up to Max); the count, 0 at the end of the
  connection. Parks the task at most once (the caller retries). }
function NetReadInto(S: TSocket; buf: pointer; Max: Integer): async LongInt;
{ Sends every byte of buf^..buf^+len-1 (no copy: the buffer can come
  straight from an arena or the parser), or raises ENetError; the count
  sent. }
function NetSendPtr(S: TSocket; buf: pointer; len: LongInt): async LongInt;
{ The same operations as plain calls, for a task (or the main thread) that
  waits for them anyway: each parks the calling task itself, so `await`ing
  the async versions above only adds a child task per call (creation,
  switch, completion). Same results, same ENetError. }
function NetAcceptDirect(Listener: TSocket): TSocket;
function NetReadSomeDirect(S: TSocket; Max: Integer): TNetBytes;
function NetSendAllDirect(S: TSocket; const Data: TNetBytes): LongInt;
function NetReadIntoDirect(S: TSocket; buf: pointer; Max: Integer): LongInt;
function NetSendPtrDirect(S: TSocket; buf: pointer; len: LongInt): LongInt;
{ Parks the current task until S is readable (WantRead) or writable
  (WantWrite, winning if both); also works for foreign descriptors (like
  a database driver's fd). Returns the NW_* bits of what fired — NW_FAIL
  reports an error state (POLLERR/POLLHUP) so a driver can skip the
  speculative syscall and rebuild its connection at once. }
function NetWaitFd(S: TSocket; WantRead, WantWrite: Boolean): Integer;
{ Like NetWaitFd, bounded: the park returns by DeadlineMs at the latest,
  with NW_TIMEOUT set when nothing else fired first (0 waits as before).
  Idle-connection reaping, database-socket bounds and connect budgets are
  its users; a timed-out wait completes normally (no leak). }
function NetWaitFdUntil(S: TSocket; WantRead, WantWrite: Boolean;
                        DeadlineMs: LongInt): Integer;
{ Same wait, keeping only the fact that it fired at all. }
procedure NetWaitReady(S: TSocket; WantRead, WantWrite: Boolean);

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}
uses
  sysutils
  {$IFNDEF LINUX}{$IFNDEF DARWIN}{$IFNDEF MSWINDOWS}, netasync_needs_linux_darwin_or_windows{$ENDIF}{$ENDIF}{$ENDIF};

type
  cint = longint;
  cuint = cardinal;
  ssize_t = int64;
{$IFDEF MSWINDOWS}
  { Winsock: a SOCKET is pointer-sized (its handles fit TSocket), recv and
    send return an int, inet_ntop takes a size_t }
  csock = int64;
  cssize = cint;
  csize = int64;
{$ELSE}
  csock = cint;
  cssize = ssize_t;
  csize = cint;
{$ENDIF}

const
  AF_INET = 2;
  SOCK_STREAM = 1;
  SOCK_DGRAM = 2;
{$IFDEF LINUX}
  SOL_SOCKET = 1;
  SO_REUSEADDR = 2;
  SO_REUSEPORT = 15;
  SO_KEEPALIVE = 9;
  SO_ERROR = 4;
  MSG_NOSIGNAL = $4000;
  EINPROGRESS = 115;
  EAGAIN = 11;
  EWOULDBLOCK = 11;
  EINTR = 4;
{$ENDIF}
{$IFDEF MSWINDOWS}
  SOL_SOCKET = $ffff;
  SO_REUSEADDR = 4;
  SO_REUSEPORT = 0;        { none: see NetListenTo }
  SO_KEEPALIVE = 8;
  SO_ERROR = $1007;
  MSG_NOSIGNAL = 0;        { no SIGPIPE on Windows }
  EINPROGRESS = 10036;     { WSAEINPROGRESS }
  EAGAIN = 10035;          { WSAEWOULDBLOCK: also a connect in progress }
  EWOULDBLOCK = 10035;
  EINTR = 10004;           { WSAEINTR }
{$ENDIF}
{$IFDEF DARWIN}
  SOL_SOCKET = $ffff;
  SO_REUSEADDR = 4;
  SO_REUSEPORT = $200;
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
{$IFDEF LINUX}
  TCP_KEEPIDLE = 4;
  TCP_KEEPINTVL = 5;
  TCP_KEEPCNT = 6;
{$ENDIF}
{$IFDEF MSWINDOWS}
  TCP_KEEPIDLE = 3;        { (Windows 10 1709 and later) }
  TCP_KEEPINTVL = 17;
  TCP_KEEPCNT = 16;
{$ENDIF}
{$IFDEF DARWIN}
  TCP_KEEPALIVE = $10;     { first probe delay }
  TCP_KEEPINTVL = $101;
  TCP_KEEPCNT = $102;
{$ENDIF}

{$IFDEF MSWINDOWS}
function WSAGetLastError: cint; cdecl; external 'ws2_32' name 'WSAGetLastError';
function WSAStartup(version: word; data: pointer): cint; cdecl;
  external 'ws2_32' name 'WSAStartup';

{ the last socket error (Winsock keeps it per thread, not in errno) }
function errno: cint;
begin
  Result := WSAGetLastError;
end;
{$ELSE}
var
  errno: cint; external 'c' name 'errno';
{$ENDIF}

procedure libc_close(s: csock); cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32' name 'closesocket'{$ELSE}'c' name 'close'{$ENDIF};
function socket(af, t, p: cint): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'socket';
function bind(s: csock; addr: pointer; len: cint): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'bind';
function listen(s: csock; backlog: cint): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'listen';
function accept(s: csock; addr: pointer; len: pointer): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'accept';
function connect(s: csock; addr: pointer; len: cint): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'connect';
function recv(s: csock; buf: pointer; len: cint; flags: cint): cssize; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'recv';
function send(s: csock; buf: pointer; len: cint; flags: cint): cssize; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'send';
procedure setsockopt(s: csock; level, optname: cint; optval: pointer; optlen: cuint); cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'setsockopt';
function getsockopt(s: csock; level, optname: cint; optval: pointer; len: pointer): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'getsockopt';
function getsockname(s: csock; addr: pointer; len: pointer): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'getsockname';
function shutdown(s: csock; how: cint): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'shutdown';
function sendto(s: csock; buf: pointer; len: cint; flags: cint;
                dst: pointer; dstlen: cint): cssize; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'sendto';
function recvfrom(s: csock; buf: pointer; len: cint; flags: cint;
                  src: pointer; srclen: pointer): cssize; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'recvfrom';
function inet_ntop(af: cint; src: pointer; dst: pchar; size: csize): pchar;
  cdecl; external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'inet_ntop';

{$IFDEF DARWIN}{$DEFINE NET_CANONNAME_FIRST}{$ENDIF}
{$IFDEF MSWINDOWS}{$DEFINE NET_CANONNAME_FIRST}{$ENDIF}
type
  Paddrinfo = ^Taddrinfo;
  PPaddrinfo = ^Paddrinfo;
  { struct addrinfo: macOS/BSD and Windows put ai_canonname before
    ai_addr, glibc the other way round (Windows' ai_addrlen is a size_t:
    its low half, then the padding before the next pointer) }
  Taddrinfo = record
    flags, family: cint;
    socktype, protocol: cint;
    addrlen: cuint;
{$IFDEF NET_CANONNAME_FIRST}
    canonname: pchar;
    addr: pointer;
{$ELSE}
    addr: pointer;
    canonname: pchar;
{$ENDIF}
    next: Paddrinfo;
  end;

function getaddrinfo(node: pchar; service: pchar; hints: paddrinfo;
                     res: PPaddrinfo): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'getaddrinfo';
procedure freeaddrinfo(res: paddrinfo); cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'freeaddrinfo';
function inet_pton(af: cint; const src: pchar; dst: pointer): cint; cdecl;
  external {$IFDEF MSWINDOWS}'ws2_32'{$ELSE}'c'{$ENDIF} name 'inet_pton';
{$IFDEF LINUX}
function accept4(s: cint; addr: pointer; len: pointer; flags: cint): cint; cdecl;
  external 'c' name 'accept4';
{$ENDIF}

{ parks the current task on fd readiness, and the async read/send forms:
 * on Linux these are io_uring ops (RECV/SEND/POLL_ADD); on macOS a kqueue
 * wake followed by the syscall; outside tasks they just block. }
function pas_task_wait_fd(fd: cint; want_write: cint): cint; cdecl;
  external name '__pas_task_wait_fd';
function pas_task_wait_fd2(fd: cint; want_write: cint; deadline_ms: cint): cint;
  cdecl; external name '__pas_task_wait_fd2';
function pas_task_recv(fd: cint; buf: pointer; len: cint): Int64; cdecl;
  external name '__pas_task_recv';
function pas_task_send(fd: cint; buf: pointer; len: cint): Int64; cdecl;
  external name '__pas_task_send';
procedure pas_fd_nonblock(fd: cint; on: cint); cdecl;
  external name '__pas_fd_nonblock';
procedure pas_fd_poller_start; cdecl; external name '__pas_fd_poller_start';
procedure pas_fd_wake_peers(fd: cint; want_write: cint); cdecl;
  external name '__pas_fd_wake_peers';
function pas_net_waits_abandoned: int64; cdecl;
  external name '__pas_net_waits_abandoned';

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

function NetListenTo(const host: string; Port, Backlog: Integer;
                     const opt: TNetListenOpts): TSocket;
var
  addr: TSockAddrIn;
  one: cint;
  s: TSocket;
begin
{$IFDEF MSWINDOWS}
  if opt.ReusePort then
    raise ENetError.Create('net: ReusePort is not supported on Windows');
{$ENDIF}
  s := socket(AF_INET, SOCK_STREAM, 0);
  if s < 0 then
    raise ENetError.Create('net: socket failed (' + IntToStr(errno) + ')');
{$IFDEF DARWIN}
  one := 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, @one, cuint(sizeof(one)));
{$ENDIF}
  pas_fd_nonblock(s, 1);
  one := 1;
{$IFNDEF MSWINDOWS}
  if opt.ReuseAddr then
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, @one, cuint(sizeof(one)));
  if opt.ReusePort then
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, @one, cuint(sizeof(one)));
{$ENDIF}
  { (Windows: a listener rebinds a port in TIME_WAIT without SO_REUSEADDR,
    whose Windows meaning is another: letting a second socket take a port
    in use; and there is no SO_REUSEPORT to balance one port over several
    listeners) }
  fillchar(addr, sizeof(addr), 0);
{$IFDEF DARWIN}
  addr.len := sizeof(TSockAddrIn);
{$ENDIF}
  addr.family := AF_INET;
  addr.port := HostToNet16(word(Port and $ffff));
  if (host = '') or (host = '0.0.0.0') then
    addr.addr := 0
  else if inet_pton(AF_INET, pchar(host), @addr.addr) <= 0 then
  begin
    libc_close(s);
    raise ENetError.Create('net: not an IPv4 address: ' + host);
  end;
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

function NetListen(Port, Backlog: Integer; AnyAddress: Boolean): TSocket;
var
  opt: TNetListenOpts;
begin
  opt.ReuseAddr := true;    { the old form always set SO_REUSEADDR }
  opt.ReusePort := false;
  if AnyAddress then
    Result := NetListenTo('', Port, Backlog, opt)
  else
    Result := NetListenTo('127.0.0.1', Port, Backlog, opt);
end;

procedure NetAdopt(S: TSocket);
{$IFDEF DARWIN}
var
  one: cint;
{$ENDIF}
begin
  pas_fd_nonblock(S, 1);
{$IFDEF DARWIN}
  one := 1;
  setsockopt(S, SOL_SOCKET, SO_NOSIGPIPE, @one, cuint(sizeof(one)));
{$ENDIF}
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

function NetAcceptDirect(Listener: TSocket): TSocket;
var
  n: TSocket;
begin
  while true do
  begin
    n := accept(Listener, nil, nil);
    if n >= 0 then
    begin
      { other waits parked on the listener (on other threads) may never
        see this readiness: the accept consumed it }
      pas_fd_wake_peers(Listener, 0);
      { the listener's nonblocking flag is not inherited on every kernel,
        and macOS does not hand accepted sockets its SO_NOSIGPIPE }
      NetAdopt(n);
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

{ Finishes the handshake of a socket with its connect() already issued
  (the literal path connects here, the resolved name on the pool thread);
  parks the task while the kernel connects, then checks the final state. }
function NetConnectFinish(s: TSocket): TSocket;
var
  bits, err, e: cint;
begin
  bits := NetWaitFd(s, false, true);
  if (bits and NW_FAIL) <> 0 then
  begin
    libc_close(s);
    raise ENetError.Create('net: connect failed (the fd died at the wake)');
  end;
  err := 0;
  e := sizeof(err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, @err, @e) <> 0) or (err <> 0) then
  begin
    libc_close(s);
    raise ENetError.Create('net: connect failed (' + IntToStr(err) + ')');
  end;
  Result := s;
end;

{ The blocking resolver, as an async task; Pool.Run runs its body on a
  pool thread. Issues the nonblocking connect of the first AF_INET
  result: the fd, -1 when the name does not resolve. The getaddrinfo
  call itself blocks that pool thread while it runs (resolv.conf bounds
  it); the connect is only issued here. }
function NetConnectWorker(const host: string; port: Integer): async Integer;
var
  hint: Taddrinfo;
  res, p: Paddrinfo;
  nodeC, portC: ansistring;
  s: TSocket;
  rc: cint;
begin
  Result := -1;
  fillchar(hint, sizeof(hint), 0);
  hint.family := AF_INET;      { the unit's TCP-over-IPv4 scope }
  hint.socktype := SOCK_STREAM;
  nodeC := host;
  portC := IntToStr(port);
  if getaddrinfo(pchar(nodeC), pchar(portC), @hint, @res) <> 0 then
    exit(-1);
  p := res;
  while p <> nil do
  begin
    s := socket(p^.family, p^.socktype, p^.protocol);
    if s >= 0 then
    begin
      NetAdopt(s);
      rc := connect(s, p^.addr, cint(p^.addrlen));
      if (rc = 0) or (errno = EINPROGRESS) or (errno = EAGAIN) then
      begin
        freeaddrinfo(res);
        exit(s);
      end;
      libc_close(s);
    end;
    p := p^.next;
  end;
  freeaddrinfo(res);
end;

function NetConnect(const Host: string; Port: Integer): async TSocket;
var
  addr: TSockAddrIn;
  lit: cardinal;
  s, rc: TSocket;
begin
  if inet_pton(AF_INET, pchar(Host), @lit) > 0 then
  begin
    s := socket(AF_INET, SOCK_STREAM, 0);
    if s < 0 then
      raise ENetError.Create('net: socket failed (' + IntToStr(errno) + ')');
    NetAdopt(s);
    fillchar(addr, sizeof(addr), 0);
{$IFDEF DARWIN}
    addr.len := sizeof(TSockAddrIn);
{$ENDIF}
    addr.family := AF_INET;
    addr.port := HostToNet16(word(Port and $ffff));
    addr.addr := lit;
    rc := connect(s, @addr, sizeof(addr));
    if (rc < 0) and (errno <> EINPROGRESS) and (errno <> EAGAIN) then
    begin
      libc_close(s);
      raise ENetError.Create('net: connect failed (' + IntToStr(errno) + ')');
    end;
    exit(NetConnectFinish(s));
  end;
  { a name: the blocking getaddrinfo runs on a pool thread; the connect is
    issued there, the handshake finishes on the caller's thread }
  var t := Pool.Run(NetConnectWorker(Host, Port));
  s := Wait(t);  if s < 0 then
  begin
    if s = -1 then
      raise ENetError.Create('net: unknown name: ' + Host);
    raise ENetError.Create('net: connect failed (' + IntToStr(-s) + ')');
  end;
  Result := NetConnectFinish(s);
end;

function NetReadSomeDirect(S: TSocket; Max: Integer): TNetBytes;
var
  one: array[0..4095] of byte;
  out: TNetBytes;
  m: cint;
  n: Int64;
begin
  if Max > 65536 then
    m := 65536
  else if Max > 0 then
    m := Max
  else
    m := 1;
  if m > sizeof(one) then
  begin
    { a big read goes straight into the result (then cut to what came) }
    SetLength(out, m);
    n := pas_task_recv(S, @out[0], m);
    if n < 0 then
      raise ENetError.Create('net: read failed (' + IntToStr(-n) + ')');
    if n < m then
      SetLength(out, n);
    Result := out;
    exit;
  end;
  n := pas_task_recv(S, @one, m);
  if n < 0 then
    raise ENetError.Create('net: read failed (' + IntToStr(-n) + ')');
  SetLength(out, n);
  if n > 0 then
    move(one, out[0], n);
  Result := out;
end;

function NetWaitFd(S: TSocket; WantRead, WantWrite: Boolean): Integer;
begin
  if WantWrite then
    Result := pas_task_wait_fd(S, 1)
  else if WantRead then
    Result := pas_task_wait_fd(S, 0)
  else
    Result := 0;
end;

function NetWaitFdUntil(S: TSocket; WantRead, WantWrite: Boolean;
                        DeadlineMs: LongInt): Integer;
begin
  if DeadlineMs <= 0 then
  begin
    Result := NetWaitFd(S, WantRead, WantWrite);
    exit;
  end;
  if WantWrite then
    Result := pas_task_wait_fd2(S, 1, cint(DeadlineMs))
  else if WantRead then
    Result := pas_task_wait_fd2(S, 0, cint(DeadlineMs))
  else
    Result := 0;
end;

procedure NetWaitReady(S: TSocket; WantRead, WantWrite: Boolean);
begin
  if WantWrite then
  begin
    pas_task_wait_fd(S, 1);
    exit;
  end;
  if WantRead then
    pas_task_wait_fd(S, 0);
end;

function NetReadIntoDirect(S: TSocket; buf: pointer; Max: Integer): LongInt;
var
  n: Int64;
begin
  n := pas_task_recv(S, buf, Max);
  if n < 0 then
    raise ENetError.Create('net: read failed (' + IntToStr(-n) + ')');
  Result := n;
end;

function NetSendPtrDirect(S: TSocket; buf: pointer; len: LongInt): LongInt;
var
  off, len2: longint;
  chunk: cint;
  n: Int64;
  p: pchar;
begin
  p := buf;
  off := 0;
  len2 := len;
  while off < len2 do
  begin
    if len2 - off > 262144 then
      chunk := 262144
    else
      chunk := len2 - off;
    n := pas_task_send(S, p + off, chunk);
    if n < 0 then
      raise ENetError.Create('net: send failed (' + IntToStr(-n) + ')');
    inc(off, n);
  end;
  Result := off;
end;

function NetSendAllDirect(S: TSocket; const Data: TNetBytes): LongInt;
begin
  if Length(Data) = 0 then
    exit(0);
  Result := NetSendPtrDirect(S, @Data[0], Length(Data));
end;

function NetAccept(Listener: TSocket): async TSocket;
begin
  Result := NetAcceptDirect(Listener);
end;

function NetReadSome(S: TSocket; Max: Integer): async TNetBytes;
begin
  Result := NetReadSomeDirect(S, Max);
end;

function NetReadInto(S: TSocket; buf: pointer; Max: Integer): async LongInt;
begin
  Result := NetReadIntoDirect(S, buf, Max);
end;

function NetSendPtr(S: TSocket; buf: pointer; len: LongInt): async LongInt;
begin
  Result := NetSendPtrDirect(S, buf, len);
end;

function NetSendAll(S: TSocket; const Data: TNetBytes): async LongInt;
begin
  Result := NetSendAllDirect(S, Data);
end;

procedure NetSetKeepAliveTimer(S: TSocket; IdleSec, IntvlSec, Count: Integer);
var
  v: cint;
begin
{$IFDEF DARWIN}
  v := cint(IdleSec);
  setsockopt(S, IPPROTO_TCP, TCP_KEEPALIVE, @v, cuint(sizeof(v)));
{$ELSE}
  v := cint(IdleSec);
  setsockopt(S, IPPROTO_TCP, TCP_KEEPIDLE, @v, cuint(sizeof(v)));
{$ENDIF}
  v := cint(IntvlSec);
  setsockopt(S, IPPROTO_TCP, TCP_KEEPINTVL, @v, cuint(sizeof(v)));
  v := cint(Count);
  setsockopt(S, IPPROTO_TCP, TCP_KEEPCNT, @v, cuint(sizeof(v)));
end;

procedure NetShutdownWrite(S: TSocket);
begin
  shutdown(S, 1);   { SHUT_WR }
end;

function NetWaitsAbandoned: Int64;
begin
  Result := pas_net_waits_abandoned;
end;

{ ---- resolution ---- }

{ The dotted-quad form of NetResolve's worker, as an async task (Pool.Run
  runs the body on a pool thread; getaddrinfo blocks that thread only). }
function NetResolveWork(const host: string): async string;
var
  hint: Taddrinfo;
  res: Paddrinfo;
  nodeC: ansistring;
  dst: array[0..15] of ansiChar;
begin
  Result := '';
  fillchar(hint, sizeof(hint), 0);
  hint.family := AF_INET;
  hint.socktype := SOCK_STREAM;
  nodeC := host;
  if getaddrinfo(pchar(nodeC), nil, @hint, @res) <> 0 then
    exit('');
  if res <> nil then
  begin
    { the IPv4 address sits after the family(+len) and port fields }
    if inet_ntop(AF_INET, pchar(res^.addr) + 4, pchar(@dst[0]), sizeof(dst)) <> nil then
      Result := StrPas(@dst[0]);
    freeaddrinfo(res);
  end;
end;

function NetResolve(const Host: string): async string;
begin
  var t := Pool.Run(NetResolveWork(Host));
  Result := Wait(t);
end;

{ ---- UDP ---- }

procedure NetMakePeer(const Host: string; Port: Integer; peer: pointer);
var
  a: PSockAddrIn;
begin
  a := peer;
  fillchar(a^, sizeof(TSockAddrIn), 0);
{$IFDEF DARWIN}
  a^.len := sizeof(TSockAddrIn);
{$ENDIF}
  a^.family := AF_INET;
  a^.port := HostToNet16(word(Port and $ffff));
  if (Host = '') or (Host = '0.0.0.0') then
    a^.addr := 0
  else if inet_pton(AF_INET, pchar(Host), @a^.addr) <= 0 then
    raise ENetError.Create('net: not an IPv4 address: ' + Host);
end;

function NetPeerString(peer: pointer): string;
var
  a: PSockAddrIn;
  dst: array[0..15] of ansiChar;
begin
  Result := '';
  a := peer;
  if a^.port = 0 then
    exit('');
  if inet_ntop(AF_INET, @a^.addr, pchar(@dst[0]), sizeof(dst)) <> nil then
    Result := StrPas(@dst[0]) + ':' + IntToStr(HostToNet16(a^.port));
end;

function NetUdpBind(const Host: string; Port: Integer): TSocket;
var
  addr: TSockAddrIn;
  s: TSocket;
begin
  Result := -1;
  s := socket(AF_INET, SOCK_DGRAM, 0);
  if s < 0 then
    raise ENetError.Create('net: socket failed (' + IntToStr(errno) + ')');
  NetAdopt(s);
  NetMakePeer(Host, Port, @addr);
  if bind(s, @addr, sizeof(addr)) <> 0 then
  begin
    libc_close(s);
    raise ENetError.Create('net: bind failed (' + IntToStr(errno) + ')');
  end;
  Result := s;
end;

procedure NetUdpPeerTo(S: TSocket; const Host: string; Port: Integer);
var
  addr: TSockAddrIn;
begin
  NetMakePeer(Host, Port, @addr);
  if connect(S, @addr, sizeof(addr)) <> 0 then
    raise ENetError.Create('net: udp peer failed (' + IntToStr(errno) + ')');
end;

function NetUdpConnect(const Host: string; Port: Integer): TSocket;
var
  s: TSocket;
begin
  s := socket(AF_INET, SOCK_DGRAM, 0);
  if s < 0 then
    raise ENetError.Create('net: socket failed (' + IntToStr(errno) + ')');
  NetAdopt(s);
  NetUdpPeerTo(s, Host, Port);
  Result := s;
end;

function NetUdpSendTo(S: TSocket; peer: pointer; buf: pointer;
                      len: LongInt): async LongInt;
var
  n: Int64;
  serrno: cint;
begin
  repeat
    n := sendto(S, buf, cint(len), MSG_NOSIGNAL, peer, cint(sizeof(TSockAddrIn)));
    serrno := errno;
    if n >= 0 then
      exit(n);
    if serrno = EINTR then
      continue;
    if (serrno = EAGAIN) or (serrno = EWOULDBLOCK) then
    begin
      NetWaitFd(S, false, true);
      continue;
    end;
    raise ENetError.Create('net: sendto failed (' + IntToStr(serrno) + ')');
  until false;
end;

function NetUdpSend(S: TSocket; buf: pointer; len: LongInt): async LongInt;
begin
  { connected datagrams: send() parks and retries like the streams do }
  Result := await NetSendPtr(S, buf, len);
end;

function NetUdpRecvFrom(S: TSocket; buf: pointer; Max: Integer;
                        src: pointer): async LongInt;
var
  n: Int64;
  serrno: cint;
  fromlen: cuint;
begin
  repeat
    if src <> nil then
    begin
      fromlen := cuint(sizeof(TSockAddrIn));
      n := recvfrom(S, buf, cint(Max), 0, src, @fromlen);
    end
    else
      n := recvfrom(S, buf, cint(Max), 0, nil, nil);
    serrno := errno;
    if n >= 0 then
      exit(n);
    if serrno = EINTR then
      continue;
    if (serrno = EAGAIN) or (serrno = EWOULDBLOCK) then
    begin
      NetWaitFd(S, true, false);
      continue;
    end;
    raise ENetError.Create('net: recvfrom failed (' + IntToStr(serrno) + ')');
  until false;
end;

function NetUdpRecv(S: TSocket; buf: pointer; Max: Integer): async LongInt;
begin
  Result := await NetReadInto(S, buf, Max);
end;

{$IFDEF MSWINDOWS}
var
  wsaData: array[0..511] of byte;   { (a WSADATA, 408 bytes on Win64) }

initialization
  WSAStartup($0202, @wsaData);
{$ENDIF}

{$ENDIF}

end.
