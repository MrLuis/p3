(* netloop - the loop engine: TCP servers on per-thread event loops over
   non-blocking sockets, the design of a hand-written C10K server, as a
   library. The sibling of netasync, which runs one task per connection:
   here a connection is a small object, and the program reacts to its
   events with callbacks. Needs --mode=objfpc.

     srv := TNetLoopServer.Create(4);            { 4 loops; 0: one per CPU }
     srv.Listen('127.0.0.1', 8080);
     srv.OnData := procedure(C: TLoopConn) => begin
       { C.InPtr/C.InCount: the bytes received and not consumed yet }
       ... C.Write(answer); C.Consume(used); ...
     end;
     srv.Run;                                    { until Stop }

   Engine. Run starts one event loop per thread (the caller's thread is
   the first): epoll on Linux, kqueue on macOS. Every loop accepts on the
   listeners itself (Linux: each loop has listeners of its own, bound with
   SO_REUSEPORT, and the kernel spreads the connections; macOS, which does
   not balance SO_REUSEPORT: the loops share one listener per port), and a
   connection stays on the loop that accepted it for its whole life:
   everything about it happens on that one thread. Sockets are registered
   once; the loop reads until EAGAIN, calls OnData, then sends what the
   socket takes, and asks for writability only while output is pending.

   Callbacks (closures; each runs on the connection's loop thread):
     OnOpen(C)   a new connection (C.Obj and C.Tag are the program's)
     OnData(C)   new bytes arrived (or the peer closed its side:
                 C.PeerClosed); consume what makes a whole message, leave
                 the rest for the next call
     OnClose(C)  the connection is gone (free what C.Obj holds here)
   A callback must not block: the loop's other connections wait for it.
   Write, Close and Abort are for the callbacks of the connection's own
   loop only.

   Buffers. Input and output grow as needed and are freed when empty, so
   an idle connection costs its object and nothing else. Back-pressure:
   while a connection's unsent output exceeds MaxOutput, the loop stops
   reading from it. A connection whose unconsumed input exceeds MaxInput
   is aborted. Close sends what is pending, then closes; Abort closes now.
   After the peer closed its side (PeerClosed), the connection closes once
   its output is sent.

   Timeouts. With IdleTimeoutMs > 0 a connection without a byte in or out
   for that long is closed (checked each second).

   Answering later. A connection may be written to (Write, Close) from any
   callback of its loop, not only its own OnData: from a timer, or from the
   watch of another descriptor (a database socket) whose answer completes a
   request; the loop sends it when the callback returns. While C.Hold is
   true, the peer closing its side does not close the connection: an answer
   still being worked on can be sent (set it back to false when done).

   Loops. Each loop is a TNetLoop, given to OnLoopStart (on its thread,
   before it serves: open what it needs, a database pool, into L.Data) and
   OnLoopStop (after its connections are closed). C.Loop is a connection's.
   On its own thread a program can also have a loop
     WatchFd(fd, read, write, proc)  call proc when fd is readable/writable
                                     (a descriptor the program owns: Unwatch
                                     it before closing it; SetInterest
                                     changes what is waited for)
     Every(ms, proc)                 call proc every ms milliseconds

   Stop (any thread, also a callback) makes every loop stop accepting and
   reading, send the output still pending (2 s at most), close its
   connections and return; Run returns when all have. *)
unit netloop;

{$H+}

interface

{$IFNDEF P3_OBJFPC}
uses netloop_needs_mode_objfpc;
{$ELSE}

uses sysutils, netasync;

type
  TNetLoopServer = class;
  TNetLoop = class;

  { what an event names: a connection or a watched descriptor }
  TLoopItem = class
  private
    FKind: Integer;             { 1 connection, 2 watch }
  end;

  { One connection. Its methods are for the callbacks of its loop only. }
  TLoopConn = class(TLoopItem)
  private
    FSock: TSocket;
    FLoop: TNetLoop;
    FIn, FOut: PAnsiChar;
    FInLen, FInCap: Integer;
    FInOff: Integer;            { the first byte not consumed yet }
    FOutLen, FOutCap, FOutSent: Integer;
    FPeerClosed, FClosing, FClosed: Boolean;
    FWriteOn, FReadOff: Boolean;
    FLastIO: Int64;
    FPrev, FNext: TLoopConn;    { its loop's connections }
    FDeadNext: TLoopConn;       { its loop's graveyard }
    FDirty: Boolean;            { written outside its own callbacks }
    FDirtyNext: TLoopConn;
    procedure Grow(var Buf: PAnsiChar; var Cap: Integer; Need: Integer);
    procedure Touched;
  public
    { the program's: anything to keep with the connection }
    Obj: TObject;
    Tag: Pointer;
    { true: the peer closing its side does not close the connection (an
      answer is still being worked on) }
    Hold: Boolean;
    destructor Destroy; override;
    { the bytes received and not consumed yet }
    function InPtr: PAnsiChar;
    function InCount: Integer;
    function InText: string;
    { drops the first N of them }
    procedure Consume(N: Integer);
    { queues bytes to send (the loop sends them after the callback) }
    procedure Write(const S: string);
    procedure WriteBuf(P: Pointer; N: Integer);
    { closes once the queued output is sent / at once }
    procedure Close;
    procedure Abort;
    function OutPending: Integer;
    function LoopIndex: Integer;
    property Loop: TNetLoop read FLoop;
    property Socket: TSocket read FSock;
    property PeerClosed: Boolean read FPeerClosed;
    property Closing: Boolean read FClosing;
  end;

  TLoopProc = reference to procedure(C: TLoopConn);
  TNetLoopProc = reference to procedure(L: TNetLoop);

  TLoopWatch = class;
  TLoopFdProc = reference to procedure(W: TLoopWatch; Readable, Writable: Boolean);

  { A descriptor the program owns, watched by a loop (WatchFd). }
  TLoopWatch = class(TLoopItem)
  private
    FFd: Integer;
    FLoop: TNetLoop;
    FRead, FWrite: Boolean;
    FDone: Boolean;
    FDeadNext: TLoopWatch;
    FProc: TLoopFdProc;
  public
    Obj: TObject;
    Tag: Pointer;
    { what is waited for }
    procedure SetInterest(Read, Write: Boolean);
    property Fd: Integer read FFd;
    property Loop: TNetLoop read FLoop;
  end;

  TLoopTimer = record
    Every: Int64;
    Next: Int64;
    Proc: TNetLoopProc;
  end;

  { One event loop (one thread). Its methods are for its own thread. }
  TNetLoop = class
  private
    FServer: TNetLoopServer;
    FIndex: Integer;
    FFd: PtrInt;                    { epoll / kqueue / wepoll's HANDLE }
    FListeners: array of TSocket;
    FHead: TLoopConn;               { its connections }
    FDead: TLoopConn;               { closed during this turn }
    FDeadWatches: TLoopWatch;
    FDirty: TLoopConn;              { written outside their callbacks }
    FCurrent: TLoopConn;            { whose callback runs }
    FTimers: array of TLoopTimer;
    FTid: PtrUInt;
    FCount: Integer;
    procedure Watch(C: TLoopConn);
    procedure Interest(C: TLoopConn);
    procedure WatchListener(L: TSocket);
    procedure AcceptAll;
    procedure ReadSome(C: TLoopConn);
    procedure Flush(C: TLoopConn);
    procedure Finish(C: TLoopConn);
    procedure Bury(C: TLoopConn);
    procedure Sweep(Now: Int64);
    procedure FinishDirty;
    procedure FireWatch(W: TLoopWatch; Readable, Writable: Boolean);
    procedure RunTimers(Now: Int64);
    function WaitMs(Now: Int64): Integer;
    procedure Main;
  public
    { the program's: what this loop's callbacks share (OnLoopStart) }
    Data: TObject;
    { calls Proc when Fd is readable / writable, on this loop }
    function WatchFd(Fd: Integer; Read, Write: Boolean; const Proc: TLoopFdProc): TLoopWatch;
    { stops watching (before the program closes the descriptor); W is
      freed when this turn ends }
    procedure Unwatch(W: TLoopWatch);
    { calls Proc every Ms milliseconds (from the next turn on) }
    procedure Every(Ms: Integer; const Proc: TNetLoopProc);
    property Index: Integer read FIndex;
    property Server: TNetLoopServer read FServer;
    property ConnCount: Integer read FCount;
  end;

  TNetLoopServer = class
  private
    FThreads: Integer;
    FHosts: array of string;
    FPorts: array of Integer;
    FListen: array of array of TSocket;   { [loop][listener] }
    FLoops: array of TNetLoop;
    FStop: Integer;                       { 1: Stop was called (read each turn) }
    FRunning: Boolean;
  public
    OnOpen, OnData, OnClose: TLoopProc;
    { on each loop's thread: before it serves / after it has stopped }
    OnLoopStart, OnLoopStop: TNetLoopProc;
    { ms without a byte in or out before a connection is closed; 0: never }
    IdleTimeoutMs: Integer;
    { unsent output above which a connection is not read (back-pressure) }
    MaxOutput: Integer;
    { unconsumed input above which a connection is aborted }
    MaxInput: Integer;
    { Threads: the number of loops (0: one per CPU) }
    constructor Create(Threads: Integer = 0);
    destructor Destroy; override;
    { a listener on Host ('' = every address) and Port (0: any free one);
      before Run; raises ENetError }
    procedure Listen(const Host: string; Port: Integer);
    { the port of the I-th Listen (the one chosen for port 0) }
    function BoundPort(I: Integer = 0): Integer;
    { runs the loops until Stop; the caller's thread is the first loop }
    procedure Run;
    procedure Stop;
    property Threads: Integer read FThreads;
  end;

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}

{ ---- the C library ---- }

type
  cint = longint;
  TPthread = PtrUInt;
  TThreadStart = function(Arg: Pointer): Pointer; cdecl;

{ Linux and Windows wait with epoll (Windows: wepoll, epoll over the AFD
  driver's polling, in p3's runtime), macOS with kqueue }
{$IFDEF LINUX}{$DEFINE NETLOOP_EPOLL}{$ENDIF}
{$IFDEF MSWINDOWS}{$DEFINE NETLOOP_EPOLL}{$ENDIF}

{$IFDEF MSWINDOWS}
const
  EINTR = 10004;               { WSAEINTR }
  EAGAIN = 10035;              { WSAEWOULDBLOCK }
  MSG_NOSIGNAL = 0;

function WSAGetLastError: cint; cdecl; external 'ws2_32' name 'WSAGetLastError';

{ the last socket error (Winsock keeps it per thread, not in errno) }
function errno: cint;
begin
  Result := WSAGetLastError;
end;

{ Winsock: a SOCKET is pointer-sized, recv and send return an int }
function c_accept(s: Int64; addr, len: Pointer): cint; cdecl; external 'ws2_32' name 'accept';
function c_recv_ws(s: Int64; buf: Pointer; len: cint; flags: cint): cint; cdecl; external 'ws2_32' name 'recv';
function c_send_ws(s: Int64; buf: Pointer; len: cint; flags: cint): cint; cdecl; external 'ws2_32' name 'send';
function c_closesocket(s: Int64): cint; cdecl; external 'ws2_32' name 'closesocket';
function GetActiveProcessorCount(group: Word): Cardinal; cdecl; external 'kernel32' name 'GetActiveProcessorCount';

function c_recv(s: cint; buf: Pointer; len: Int64; flags: cint): Int64;
begin
  Result := c_recv_ws(s, buf, cint(len), flags);
end;

function c_send(s: cint; buf: Pointer; len: Int64; flags: cint): Int64;
begin
  Result := c_send_ws(s, buf, cint(len), flags);
end;

function c_close(fd: cint): cint;
begin
  Result := c_closesocket(fd);
end;

function pthread_create(var tid: TPthread; attr: Pointer; start: TThreadStart; arg: Pointer): cint;
  cdecl; external 'pthread' name 'pthread_create';
function pthread_join(tid: TPthread; ret: Pointer): cint; cdecl; external 'pthread' name 'pthread_join';
{$ELSE}
const
  EINTR = 4;
{$IFDEF DARWIN}
  EAGAIN = 35;
  MSG_NOSIGNAL = 0;            { (NetAdopt set SO_NOSIGPIPE) }
{$ELSE}
  EAGAIN = 11;
  MSG_NOSIGNAL = $4000;
  SOCK_NONBLOCK = $800;
  SOCK_CLOEXEC = $80000;
{$ENDIF}

var
  errno: cint; external 'c' name 'errno';

function c_accept(s: cint; addr, len: Pointer): cint; cdecl; external 'c' name 'accept';
{$IFDEF LINUX}
function c_accept4(s: cint; addr, len: Pointer; flags: cint): cint; cdecl; external 'c' name 'accept4';
{$ENDIF}
function c_recv(s: cint; buf: Pointer; len: Int64; flags: cint): Int64; cdecl; external 'c' name 'recv';
function c_send(s: cint; buf: Pointer; len: Int64; flags: cint): Int64; cdecl; external 'c' name 'send';
function c_close(fd: cint): cint; cdecl; external 'c' name 'close';
function c_sysconf(name: cint): Int64; cdecl; external 'c' name 'sysconf';
function pthread_create(var tid: TPthread; attr: Pointer; start: TThreadStart; arg: Pointer): cint;
  cdecl; external 'c' name 'pthread_create';
function pthread_join(tid: TPthread; ret: Pointer): cint; cdecl; external 'c' name 'pthread_join';
{$ENDIF}
{ the runtime: counted values become thread-safe before a thread exists }
procedure pas_start_threads; cdecl; external name '__pas_start_threads';

{$IFDEF LINUX}
const
  EPOLLIN = $1;
  EPOLLOUT = $4;
  EPOLLERR = $8;
  EPOLLHUP = $10;
  EPOLL_CTL_ADD = 1;
  EPOLL_CTL_DEL = 2;
  EPOLL_CTL_MOD = 3;
  SC_NPROCESSORS_ONLN = 84;
type
  { struct epoll_event: packed on x86_64 (12 bytes), aligned elsewhere }
  {$IFDEF CPUX86_64}
  {$packrecords 1}
  TEpollEvent = record
    events: Cardinal;
    data: Pointer;
  end;
  {$packrecords default}
  {$ELSE}
  TEpollEvent = record
    events: Cardinal;
    pad: Cardinal;
    data: Pointer;
  end;
  {$ENDIF}
  PEpollEvent = ^TEpollEvent;
function epoll_create1(flags: cint): cint; cdecl; external 'c';
function epoll_ctl(epfd, op, fd: cint; ev: PEpollEvent): cint; cdecl; external 'c';
function epoll_wait(epfd: cint; evs: PEpollEvent; max, timeout: cint): cint; cdecl; external 'c';
{$ENDIF}

{$IFDEF MSWINDOWS}
const
  EPOLLIN = $1;
  EPOLLOUT = $4;
  EPOLLERR = $8;
  EPOLLHUP = $10;
  EPOLL_CTL_ADD = 1;           { (wepoll's numbers: MOD and DEL are swapped }
  EPOLL_CTL_MOD = 2;           {  against Linux's) }
  EPOLL_CTL_DEL = 3;
type
  TEpollEvent = record         { struct epoll_event: not packed in wepoll }
    events: Cardinal;
    pad: Cardinal;
    data: Pointer;
  end;
  PEpollEvent = ^TEpollEvent;
{ wepoll (in p3's runtime): the port is a HANDLE, the descriptor a SOCKET }
function epoll_create1(flags: cint): PtrInt; cdecl; external name 'epoll_create1';
function epoll_close(ep: PtrInt): cint; cdecl; external name 'epoll_close';
function epoll_ctl(ep: PtrInt; op: cint; s: Int64; ev: PEpollEvent): cint; cdecl;
  external name 'epoll_ctl';
function epoll_wait(ep: PtrInt; evs: PEpollEvent; max, timeout: cint): cint; cdecl;
  external name 'epoll_wait';
{$ENDIF}

{$IFDEF DARWIN}
const
  EVFILT_READ = -1;
  EVFILT_WRITE = -2;
  EV_ADD = 1;
  EV_ENABLE = 4;
  EV_DISABLE = 8;
  EV_DELETE = 2;
  EV_ERROR = $4000;
  EV_EOF = $8000;
  SC_NPROCESSORS_ONLN = 58;
type
  TKEvent = record
    ident: PtrUInt;
    filter: SmallInt;
    flags: Word;
    fflags: Cardinal;
    data: PtrInt;
    udata: Pointer;
  end;
  PKEvent = ^TKEvent;
  TTimespec = record
    sec, nsec: Int64;
  end;
function kqueue: cint; cdecl; external 'c';
function kevent(kq: cint; changes: PKEvent; nchanges: cint; events: PKEvent;
                nevents: cint; timeout: Pointer): cint; cdecl; external 'c';
{$ENDIF}

{ ---- TLoopConn ---- }

destructor TLoopConn.Destroy;
begin
  if FIn <> nil then FreeMem(FIn);
  if FOut <> nil then FreeMem(FOut);
  inherited Destroy;
end;

procedure TLoopConn.Grow(var Buf: PAnsiChar; var Cap: Integer; Need: Integer);
var
  n: Integer;
begin
  if Need <= Cap then
    exit;
  n := Cap;
  if n < 4096 then n := 4096;
  while n < Need do
    n := n * 2;
  ReallocMem(Buf, n);
  Cap := n;
end;

function TLoopConn.InPtr: PAnsiChar;
begin
  Result := FIn + FInOff;
end;

function TLoopConn.InCount: Integer;
begin
  Result := FInLen - FInOff;
end;

function TLoopConn.InText: string;
var
  t: string;
begin
  SetLength(t, FInLen - FInOff);
  if FInLen > FInOff then
    Move((FIn + FInOff)^, t[1], FInLen - FInOff);
  Result := t;
end;

{ Consume only moves a cursor: the bytes left are moved down once, when more
  arrive (ReadSome), so taking messages one at a time costs nothing per
  message. }
procedure TLoopConn.Consume(N: Integer);
begin
  if N <= 0 then
    exit;
  if N >= FInLen - FInOff then
  begin
    FInLen := 0;
    FInOff := 0;
    FreeMem(FIn);          { idle: no buffer }
    FIn := nil;
    FInCap := 0;
    exit;
  end;
  FInOff := FInOff + N;
end;

{ changed outside its own callback: the loop finishes it after this one }
procedure TLoopConn.Touched;
begin
  if FDirty or FClosed or (FLoop.FCurrent = Self) then
    exit;
  FDirty := true;
  FDirtyNext := FLoop.FDirty;
  FLoop.FDirty := Self;
end;

procedure TLoopConn.WriteBuf(P: Pointer; N: Integer);
begin
  if (N <= 0) or FClosed then
    exit;
  Grow(FOut, FOutCap, FOutLen + N);
  Move(PAnsiChar(P)^, (FOut + FOutLen)^, N);
  FOutLen := FOutLen + N;
  Touched;
end;

procedure TLoopConn.Write(const S: string);
begin
  if S <> '' then
    WriteBuf(@S[1], Length(S));
end;

procedure TLoopConn.Close;
begin
  FClosing := true;
  Touched;
end;

procedure TLoopConn.Abort;
begin
  FClosing := true;
  FOutLen := 0;
  FOutSent := 0;
  Touched;
end;

function TLoopConn.OutPending: Integer;
begin
  Result := FOutLen - FOutSent;
end;

function TLoopConn.LoopIndex: Integer;
begin
  Result := FLoop.FIndex;
end;

{ ---- TNetLoop ---- }

procedure TNetLoop.WatchListener(L: TSocket);
{$IFDEF NETLOOP_EPOLL}
var
  ev: TEpollEvent;
begin
  FillChar(ev, sizeof(ev), 0);
  ev.events := EPOLLIN;
  ev.data := nil;               { nil: a listener }
  epoll_ctl(FFd, EPOLL_CTL_ADD, L, @ev);
end;
{$ELSE}
var
  k: TKEvent;
begin
  FillChar(k, sizeof(k), 0);
  k.ident := L;
  k.filter := EVFILT_READ;
  k.flags := EV_ADD;
  k.udata := nil;
  kevent(FFd, @k, 1, nil, 0, nil);
end;
{$ENDIF}

{ a new connection: reading on, writing registered but off }
procedure TNetLoop.Watch(C: TLoopConn);
{$IFDEF NETLOOP_EPOLL}
var
  ev: TEpollEvent;
begin
  FillChar(ev, sizeof(ev), 0);
  ev.events := EPOLLIN;
  ev.data := C;
  epoll_ctl(FFd, EPOLL_CTL_ADD, C.FSock, @ev);
end;
{$ELSE}
var
  k: array[0..1] of TKEvent;
begin
  FillChar(k, sizeof(k), 0);
  k[0].ident := C.FSock;
  k[0].filter := EVFILT_READ;
  k[0].flags := EV_ADD;
  k[0].udata := C;
  k[1].ident := C.FSock;
  k[1].filter := EVFILT_WRITE;
  k[1].flags := EV_ADD or EV_DISABLE;
  k[1].udata := C;
  kevent(FFd, @k[0], 2, nil, 0, nil);
end;
{$ENDIF}

{ reading on unless back-pressured (or finished); writing on while output
  is pending - changed only when it changes }
procedure TNetLoop.Interest(C: TLoopConn);
var
  wantWrite, readOff: Boolean;
{$IFDEF NETLOOP_EPOLL}
  ev: TEpollEvent;
{$ELSE}
  k: array[0..1] of TKEvent;
  n: Integer;
{$ENDIF}
begin
  wantWrite := C.OutPending > 0;
  readOff := C.FPeerClosed or C.FClosing or (C.OutPending > FServer.MaxOutput);
  if (wantWrite = C.FWriteOn) and (readOff = C.FReadOff) then
    exit;
{$IFDEF NETLOOP_EPOLL}
  FillChar(ev, sizeof(ev), 0);
  ev.data := C;
  if not readOff then ev.events := ev.events or EPOLLIN;
  if wantWrite then ev.events := ev.events or EPOLLOUT;
  epoll_ctl(FFd, EPOLL_CTL_MOD, C.FSock, @ev);
{$ELSE}
  FillChar(k, sizeof(k), 0);
  n := 0;
  if wantWrite <> C.FWriteOn then
  begin
    k[n].ident := C.FSock;
    k[n].filter := EVFILT_WRITE;
    if wantWrite then k[n].flags := EV_ENABLE else k[n].flags := EV_DISABLE;
    k[n].udata := C;
    inc(n);
  end;
  if readOff <> C.FReadOff then
  begin
    k[n].ident := C.FSock;
    k[n].filter := EVFILT_READ;
    if readOff then k[n].flags := EV_DISABLE else k[n].flags := EV_ENABLE;
    k[n].udata := C;
    inc(n);
  end;
  kevent(FFd, @k[0], n, nil, 0, nil);
{$ENDIF}
  C.FWriteOn := wantWrite;
  C.FReadOff := readOff;
end;

procedure TNetLoop.AcceptAll;
var
  i: Integer;
  s: cint;
  c: TLoopConn;
begin
  if FServer.FStop <> 0 then
    exit;
  for i := 0 to High(FListeners) do
    while true do
    begin
{$IFDEF LINUX}
      s := c_accept4(FListeners[i], nil, nil, SOCK_NONBLOCK or SOCK_CLOEXEC);
{$ELSE}
      s := c_accept(FListeners[i], nil, nil);
{$ENDIF}
      if s < 0 then
        break;                  { EAGAIN: no more now (another loop took it) }
{$IFNDEF LINUX}
      NetAdopt(s);              { non-blocking (and on macOS no SIGPIPE) }
{$ENDIF}
      NetSetNoDelay(s);
      c := TLoopConn.Create;
      c.FKind := 1;
      c.FSock := s;
      c.FLoop := Self;
      c.FLastIO := GetTickCount64;
      c.FNext := FHead;
      if FHead <> nil then FHead.FPrev := c;
      FHead := c;
      inc(FCount);
      Watch(c);
      if Assigned(FServer.OnOpen) then
      begin
        FCurrent := c;
        FServer.OnOpen(c);
        FCurrent := nil;
      end;
      if c.FClosing then
        Finish(c);
    end;
end;

{ c is done: its callbacks are over, its record goes when this turn ends }
procedure TNetLoop.Bury(C: TLoopConn);
begin
  if C.FClosed then
    exit;
  C.FClosed := true;
  c_close(C.FSock);             { (drops its registrations) }
  if C.FPrev <> nil then C.FPrev.FNext := C.FNext else FHead := C.FNext;
  if C.FNext <> nil then C.FNext.FPrev := C.FPrev;
  dec(FCount);
  if Assigned(FServer.OnClose) then
  begin
    FCurrent := C;
    FServer.OnClose(C);
    FCurrent := nil;
  end;
  C.FDeadNext := FDead;
  FDead := C;
end;

{ after a callback: send, then close it if it is finished }
procedure TNetLoop.Finish(C: TLoopConn);
begin
  if C.FClosed then
    exit;
  if C.OutPending > 0 then
    Flush(C);
  if C.FClosed then
    exit;
  if (C.FClosing or (C.FPeerClosed and not C.Hold)) and (C.OutPending = 0) then
    Bury(C)
  else
    Interest(C);
end;

procedure TNetLoop.Flush(C: TLoopConn);
var
  n: Int64;
begin
  while C.FOutSent < C.FOutLen do
  begin
    n := c_send(C.FSock, C.FOut + C.FOutSent, C.FOutLen - C.FOutSent, MSG_NOSIGNAL);
    if n > 0 then
    begin
      C.FOutSent := C.FOutSent + n;
      C.FLastIO := GetTickCount64;
    end
    else if (n < 0) and (errno = EINTR) then
      continue
    else if (n < 0) and (errno = EAGAIN) then
      exit                      { full: writability will call again }
    else
    begin
      C.FOutLen := 0;           { the peer is gone }
      C.FOutSent := 0;
      Bury(C);
      exit;
    end;
  end;
  { all out: no buffer }
  C.FOutLen := 0;
  C.FOutSent := 0;
  if C.FOut <> nil then
  begin
    FreeMem(C.FOut);
    C.FOut := nil;
    C.FOutCap := 0;
  end;
end;

procedure TNetLoop.ReadSome(C: TLoopConn);
var
  n: Int64;
  got: Boolean;
begin
  got := false;
  if C.FInOff > 0 then
  begin
    C.FInLen := C.FInLen - C.FInOff;
    Move((C.FIn + C.FInOff)^, C.FIn^, C.FInLen);
    C.FInOff := 0;
  end;
  while not C.FPeerClosed do
  begin
    C.Grow(C.FIn, C.FInCap, C.FInLen + 16384);
    n := c_recv(C.FSock, C.FIn + C.FInLen, C.FInCap - C.FInLen, 0);
    if n > 0 then
    begin
      C.FInLen := C.FInLen + n;
      C.FLastIO := GetTickCount64;
      got := true;
      if C.FInLen > FServer.MaxInput then
      begin
        C.Abort;
        Finish(C);
        exit;
      end;
    end
    else if (n < 0) and (errno = EINTR) then
      continue
    else if (n < 0) and (errno = EAGAIN) then
      break
    else
    begin
      C.FPeerClosed := true;    { (or broken: the same for the program) }
      got := true;
    end;
  end;
  if (C.FInLen = 0) and (C.FIn <> nil) then
  begin
    FreeMem(C.FIn);
    C.FIn := nil;
    C.FInCap := 0;
  end;
  if got and Assigned(FServer.OnData) and not C.FClosed then
  begin
    FCurrent := C;
    FServer.OnData(C);
    FCurrent := nil;
  end;
  Finish(C);
end;

procedure TNetLoop.Sweep(Now: Int64);
var
  c, nxt: TLoopConn;
begin
  if FServer.IdleTimeoutMs <= 0 then
    exit;
  c := FHead;
  while c <> nil do
  begin
    nxt := c.FNext;
    if Now - c.FLastIO > FServer.IdleTimeoutMs then
    begin
      c.Abort;
      Bury(c);
    end;
    c := nxt;
  end;
end;

procedure TNetLoop.FinishDirty;
var
  c: TLoopConn;
begin
  while FDirty <> nil do
  begin
    c := FDirty;
    FDirty := c.FDirtyNext;
    c.FDirty := false;
    c.FDirtyNext := nil;
    Finish(c);
  end;
end;

{ ---- watched descriptors ---- }

procedure SetWatch(L: TNetLoop; W: TLoopWatch; Op: Integer);
{$IFDEF NETLOOP_EPOLL}
var
  ev: TEpollEvent;
begin
  FillChar(ev, sizeof(ev), 0);
  ev.data := W;
  if W.FRead then ev.events := ev.events or EPOLLIN;
  if W.FWrite then ev.events := ev.events or EPOLLOUT;
  epoll_ctl(L.FFd, Op, W.FFd, @ev);
end;
{$ELSE}
var
  k: array[0..1] of TKEvent;
begin
  FillChar(k, sizeof(k), 0);
  k[0].ident := W.FFd;
  k[0].filter := EVFILT_READ;
  k[0].udata := W;
  k[1].ident := W.FFd;
  k[1].filter := EVFILT_WRITE;
  k[1].udata := W;
  if Op = 2 then
  begin
    k[0].flags := EV_DELETE;
    k[1].flags := EV_DELETE;
  end
  else
  begin
    if W.FRead then k[0].flags := EV_ADD or EV_ENABLE else k[0].flags := EV_ADD or EV_DISABLE;
    if W.FWrite then k[1].flags := EV_ADD or EV_ENABLE else k[1].flags := EV_ADD or EV_DISABLE;
  end;
  kevent(L.FFd, @k[0], 2, nil, 0, nil);
end;
{$ENDIF}

function TNetLoop.WatchFd(Fd: Integer; Read, Write: Boolean; const Proc: TLoopFdProc): TLoopWatch;
var
  w: TLoopWatch;
begin
  w := TLoopWatch.Create;
  w.FKind := 2;
  w.FFd := Fd;
  w.FLoop := Self;
  w.FRead := Read;
  w.FWrite := Write;
  w.FProc := Proc;
  SetWatch(Self, w, 1);          { (EPOLL_CTL_ADD) }
  Result := w;
end;

procedure TLoopWatch.SetInterest(Read, Write: Boolean);
begin
  if FDone or ((Read = FRead) and (Write = FWrite)) then
    exit;
  FRead := Read;
  FWrite := Write;
  SetWatch(FLoop, Self, 3);      { (EPOLL_CTL_MOD; kqueue: EV_ADD again) }
end;

procedure TNetLoop.Unwatch(W: TLoopWatch);
begin
  if (W = nil) or W.FDone then
    exit;
  W.FDone := true;
  SetWatch(Self, W, 2);          { (EPOLL_CTL_DEL / EV_DELETE) }
  W.FDeadNext := FDeadWatches;
  FDeadWatches := W;
end;

procedure TNetLoop.FireWatch(W: TLoopWatch; Readable, Writable: Boolean);
begin
  if W.FDone then
    exit;                        { unwatched earlier this turn }
  W.FProc(W, Readable and W.FRead, Writable and W.FWrite);
  FinishDirty;
end;

{ ---- timers ---- }

procedure TNetLoop.Every(Ms: Integer; const Proc: TNetLoopProc);
var
  n: Integer;
begin
  if Ms < 1 then Ms := 1;
  n := Length(FTimers);
  SetLength(FTimers, n + 1);
  FTimers[n].Every := Ms;
  FTimers[n].Next := GetTickCount64 + Ms;
  FTimers[n].Proc := Proc;
end;

procedure TNetLoop.RunTimers(Now: Int64);
var
  i: Integer;
  p: TNetLoopProc;
begin
  for i := 0 to High(FTimers) do
    if Now >= FTimers[i].Next then
    begin
      FTimers[i].Next := Now + FTimers[i].Every;
      p := FTimers[i].Proc;
      p(Self);
      FinishDirty;
    end;
end;

{ how long the next wait may be: 250 ms, or less for a timer due sooner }
function TNetLoop.WaitMs(Now: Int64): Integer;
var
  i: Integer;
  d: Int64;
begin
  Result := 250;
  for i := 0 to High(FTimers) do
  begin
    d := FTimers[i].Next - Now;
    if d < 0 then d := 0;
    if d < Result then Result := Integer(d);
  end;
end;

procedure TNetLoop.Main;
const
  MAXEV = 256;
var
  i, n: Integer;
  data: Pointer;
  c, nxt: TLoopConn;
  w: TLoopWatch;
  lastSweep, stopAt, now: Int64;
  stopping: Boolean;
{$IFDEF NETLOOP_EPOLL}
  evs: array[0..MAXEV - 1] of TEpollEvent;
  bits: Cardinal;
{$ELSE}
  evs: array[0..MAXEV - 1] of TKEvent;
  ts: TTimespec;
{$ENDIF}
begin
  if Assigned(FServer.OnLoopStart) then
    FServer.OnLoopStart(Self);
  for i := 0 to High(FListeners) do
    WatchListener(FListeners[i]);
  lastSweep := GetTickCount64;
  stopping := false;
  stopAt := 0;
  while true do
  begin
{$IFDEF NETLOOP_EPOLL}
    n := epoll_wait(FFd, @evs[0], MAXEV, WaitMs(GetTickCount64));
{$ELSE}
    ts.sec := 0;
    ts.nsec := Int64(WaitMs(GetTickCount64)) * 1000000;
    n := kevent(FFd, nil, 0, @evs[0], MAXEV, @ts);
{$ENDIF}
    if n < 0 then
      n := 0;                   { EINTR }
    for i := 0 to n - 1 do
    begin
{$IFDEF NETLOOP_EPOLL}
      data := evs[i].data;
      bits := evs[i].events;
{$ELSE}
      data := evs[i].udata;
{$ENDIF}
      if data = nil then
      begin
        AcceptAll;
        FinishDirty;
        continue;
      end;
      if TLoopItem(data).FKind = 2 then
      begin
{$IFDEF NETLOOP_EPOLL}
        FireWatch(TLoopWatch(data), (bits and (EPOLLIN or EPOLLERR or EPOLLHUP)) <> 0,
                  (bits and (EPOLLOUT or EPOLLERR or EPOLLHUP)) <> 0);
{$ELSE}
        if (evs[i].flags and EV_ERROR) = 0 then
          FireWatch(TLoopWatch(data), evs[i].filter = EVFILT_READ, evs[i].filter = EVFILT_WRITE);
{$ENDIF}
        continue;
      end;
      c := TLoopConn(data);
      if c.FClosed then
        continue;               { closed earlier this turn }
{$IFDEF NETLOOP_EPOLL}
      if (bits and EPOLLOUT) <> 0 then
        Finish(c);
      if not c.FClosed and ((bits and (EPOLLIN or EPOLLERR or EPOLLHUP)) <> 0) then
        if not c.FReadOff then
          ReadSome(c)
        else if (bits and (EPOLLERR or EPOLLHUP)) <> 0 then
        begin
          c.Abort;              { broken both ways (and reported until closed) }
          Bury(c);
        end
        else
          Finish(c);
      FinishDirty;
{$ELSE}
      if (evs[i].flags and EV_ERROR) <> 0 then
        continue;
      if evs[i].filter = EVFILT_WRITE then
        Finish(c)
      else if not c.FReadOff then
        ReadSome(c);
      FinishDirty;
{$ENDIF}
    end;
    now := GetTickCount64;
    RunTimers(now);
    if now - lastSweep >= 1000 then
    begin
      Sweep(now);
      lastSweep := now;
    end;
    { Stop: no more reading; out with what is pending, then done }
    if not stopping and (FServer.FStop <> 0) then
    begin
      stopping := true;
      stopAt := now + 2000;
      c := FHead;
      while c <> nil do
      begin
        nxt := c.FNext;
        c.FClosing := true;
        Finish(c);
        c := nxt;
      end;
    end;
    FinishDirty;
    { the graveyard: no event of a later turn names them }
    while FDead <> nil do
    begin
      c := FDead;
      FDead := c.FDeadNext;
      c.Free;
    end;
    while FDeadWatches <> nil do
    begin
      w := FDeadWatches;
      FDeadWatches := w.FDeadNext;
      w.Free;
    end;
    if stopping and ((FHead = nil) or (now >= stopAt)) then
      break;
  end;
  { whatever is left }
  while FHead <> nil do
  begin
    FHead.Abort;
    Bury(FHead);
  end;
  while FDead <> nil do
  begin
    c := FDead;
    FDead := c.FDeadNext;
    c.Free;
  end;
  if Assigned(FServer.OnLoopStop) then
    FServer.OnLoopStop(Self);
  while FDeadWatches <> nil do
  begin
    w := FDeadWatches;
    FDeadWatches := w.FDeadNext;
    w.Free;
  end;
{$IFDEF MSWINDOWS}
  epoll_close(FFd);
{$ELSE}
  c_close(FFd);
{$ENDIF}
end;

function LoopThread(Arg: Pointer): Pointer; cdecl;
begin
  TNetLoop(Arg).Main;
  Result := nil;
end;

{ ---- TNetLoopServer ---- }

constructor TNetLoopServer.Create(Threads: Integer);
var
  n: Int64;
begin
  inherited Create;
  if Threads <= 0 then
  begin
{$IFDEF MSWINDOWS}
    n := GetActiveProcessorCount($FFFF);   { ALL_PROCESSOR_GROUPS }
{$ELSE}
    n := c_sysconf(SC_NPROCESSORS_ONLN);
{$ENDIF}
    if n < 1 then n := 1;
    Threads := Integer(n);
  end;
  FThreads := Threads;
  SetLength(FListen, FThreads);
  IdleTimeoutMs := 0;
  MaxOutput := 4 * 1024 * 1024;
  MaxInput := 4 * 1024 * 1024;
end;

destructor TNetLoopServer.Destroy;
var
  i, k: Integer;
begin
  for i := 0 to High(FListen) do
    for k := 0 to High(FListen[i]) do
{$IFDEF LINUX}
      NetClose(FListen[i][k]);
{$ELSE}
      if i = 0 then NetClose(FListen[i][k]);   { (shared) }
{$ENDIF}
  inherited Destroy;
end;

procedure TNetLoopServer.Listen(const Host: string; Port: Integer);
var
  opt: TNetListenOpts;
  i, k: Integer;
  first: TSocket;
begin
  if FRunning then
    raise ENetError.Create('netloop: Listen before Run');
  k := Length(FHosts);
  SetLength(FHosts, k + 1);
  SetLength(FPorts, k + 1);
  FHosts[k] := Host;
  opt.ReuseAddr := true;
{$IFDEF LINUX}
  { a listener per loop on one port: the kernel spreads the connections }
  opt.ReusePort := true;
  first := NetListenTo(Host, Port, 4096, opt);
  FPorts[k] := NetBoundPort(first);
  for i := 0 to FThreads - 1 do
  begin
    SetLength(FListen[i], k + 1);
    if i = 0 then
      FListen[i][k] := first
    else
      FListen[i][k] := NetListenTo(Host, FPorts[k], 4096, opt);
  end;
{$ELSE}
  { one listener shared by the loops (SO_REUSEPORT does not balance here) }
  opt.ReusePort := false;
  first := NetListenTo(Host, Port, 4096, opt);
  FPorts[k] := NetBoundPort(first);
  for i := 0 to FThreads - 1 do
  begin
    SetLength(FListen[i], k + 1);
    FListen[i][k] := first;
  end;
{$ENDIF}
end;

function TNetLoopServer.BoundPort(I: Integer): Integer;
begin
  Result := FPorts[I];
end;

procedure TNetLoopServer.Run;
var
  i: Integer;
  l: TNetLoop;
begin
  if FRunning then
    raise ENetError.Create('netloop: Run while running');
  if Length(FHosts) = 0 then
    raise ENetError.Create('netloop: Run without Listen');
  FRunning := true;
  FStop := 0;
  SetLength(FLoops, FThreads);
  for i := 0 to FThreads - 1 do
  begin
    l := TNetLoop.Create;
    l.FServer := Self;
    l.FIndex := i;
    l.FListeners := FListen[i];
{$IFDEF NETLOOP_EPOLL}
    l.FFd := epoll_create1(0);
{$ELSE}
    l.FFd := kqueue;
{$ENDIF}
{$IFDEF MSWINDOWS}
    if l.FFd = 0 then           { (a NULL HANDLE) }
{$ELSE}
    if l.FFd < 0 then
{$ENDIF}
      raise ENetError.Create('netloop: no event queue');
    FLoops[i] := l;
  end;
  if FThreads > 1 then
    pas_start_threads;
  for i := 1 to FThreads - 1 do
    if pthread_create(FLoops[i].FTid, nil, @LoopThread, FLoops[i]) <> 0 then
      raise ENetError.Create('netloop: cannot start a loop thread');
  FLoops[0].Main;
  for i := 1 to FThreads - 1 do
    pthread_join(FLoops[i].FTid, nil);
  for i := 0 to FThreads - 1 do
    FLoops[i].Free;
  SetLength(FLoops, 0);
  FRunning := false;
end;

procedure TNetLoopServer.Stop;
begin
  { (one aligned word: a loop sees it at its next turn, at most 250 ms on) }
  FStop := 1;
end;

{$ENDIF}

end.
