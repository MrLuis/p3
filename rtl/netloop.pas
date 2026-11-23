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

  { One connection. Its methods are for the callbacks of its loop only. }
  TLoopConn = class
  private
    FSock: TSocket;
    FLoop: TObject;             { its TLoop }
    FIn, FOut: PAnsiChar;
    FInLen, FInCap: Integer;
    FOutLen, FOutCap, FOutSent: Integer;
    FPeerClosed, FClosing, FClosed: Boolean;
    FWriteOn, FReadOff: Boolean;
    FLastIO: Int64;
    FPrev, FNext: TLoopConn;    { its loop's connections }
    FDeadNext: TLoopConn;       { its loop's graveyard }
    procedure Grow(var Buf: PAnsiChar; var Cap: Integer; Need: Integer);
  public
    { the program's: anything to keep with the connection }
    Obj: TObject;
    Tag: Pointer;
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
    property Socket: TSocket read FSock;
    property PeerClosed: Boolean read FPeerClosed;
    property Closing: Boolean read FClosing;
  end;

  TLoopProc = reference to procedure(C: TLoopConn);

  TNetLoopServer = class
  private
    FThreads: Integer;
    FHosts: array of string;
    FPorts: array of Integer;
    FListen: array of array of TSocket;   { [loop][listener] }
    FLoops: array of TObject;
    FStop: Integer;                       { 1: Stop was called (read each turn) }
    FRunning: Boolean;
  public
    OnOpen, OnData, OnClose: TLoopProc;
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

{ ---- a loop ---- }

type
  TLoop = class
    Server: TNetLoopServer;
    Index: Integer;
    Fd: cint;                       { epoll / kqueue }
    Listeners: array of TSocket;
    Head: TLoopConn;                { its connections }
    Dead: TLoopConn;                { closed during this turn }
    Tid: TPthread;
    Count: Integer;
    procedure Watch(C: TLoopConn);
    procedure Interest(C: TLoopConn);
    procedure WatchListener(L: TSocket);
    procedure AcceptAll;
    procedure ReadSome(C: TLoopConn);
    procedure Flush(C: TLoopConn);
    procedure Finish(C: TLoopConn);
    procedure Bury(C: TLoopConn);
    procedure Sweep(Now: Int64);
    procedure Main;
  end;

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
  Result := FIn;
end;

function TLoopConn.InCount: Integer;
begin
  Result := FInLen;
end;

function TLoopConn.InText: string;
var
  t: string;
begin
  SetLength(t, FInLen);
  if FInLen > 0 then
    Move(FIn^, t[1], FInLen);
  Result := t;
end;

procedure TLoopConn.Consume(N: Integer);
begin
  if N <= 0 then
    exit;
  if N >= FInLen then
  begin
    FInLen := 0;
    FreeMem(FIn);          { idle: no buffer }
    FIn := nil;
    FInCap := 0;
    exit;
  end;
  Move((FIn + N)^, FIn^, FInLen - N);
  FInLen := FInLen - N;
end;

procedure TLoopConn.WriteBuf(P: Pointer; N: Integer);
begin
  if (N <= 0) or FClosed then
    exit;
  Grow(FOut, FOutCap, FOutLen + N);
  Move(PAnsiChar(P)^, (FOut + FOutLen)^, N);
  FOutLen := FOutLen + N;
end;

procedure TLoopConn.Write(const S: string);
begin
  if S <> '' then
    WriteBuf(@S[1], Length(S));
end;

procedure TLoopConn.Close;
begin
  FClosing := true;
end;

procedure TLoopConn.Abort;
begin
  FClosing := true;
  FOutLen := 0;
  FOutSent := 0;
end;

function TLoopConn.OutPending: Integer;
begin
  Result := FOutLen - FOutSent;
end;

function TLoopConn.LoopIndex: Integer;
begin
  Result := TLoop(FLoop).Index;
end;

{ ---- TLoop ---- }

procedure TLoop.WatchListener(L: TSocket);
{$IFDEF LINUX}
var
  ev: TEpollEvent;
begin
  FillChar(ev, sizeof(ev), 0);
  ev.events := EPOLLIN;
  ev.data := nil;               { nil: a listener }
  epoll_ctl(Fd, EPOLL_CTL_ADD, L, @ev);
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
  kevent(Fd, @k, 1, nil, 0, nil);
end;
{$ENDIF}

{ a new connection: reading on, writing registered but off }
procedure TLoop.Watch(C: TLoopConn);
{$IFDEF LINUX}
var
  ev: TEpollEvent;
begin
  FillChar(ev, sizeof(ev), 0);
  ev.events := EPOLLIN;
  ev.data := C;
  epoll_ctl(Fd, EPOLL_CTL_ADD, C.FSock, @ev);
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
  kevent(Fd, @k[0], 2, nil, 0, nil);
end;
{$ENDIF}

{ reading on unless back-pressured (or finished); writing on while output
  is pending - changed only when it changes }
procedure TLoop.Interest(C: TLoopConn);
var
  wantWrite, readOff: Boolean;
{$IFDEF LINUX}
  ev: TEpollEvent;
{$ELSE}
  k: array[0..1] of TKEvent;
  n: Integer;
{$ENDIF}
begin
  wantWrite := C.OutPending > 0;
  readOff := C.FPeerClosed or C.FClosing or (C.OutPending > Server.MaxOutput);
  if (wantWrite = C.FWriteOn) and (readOff = C.FReadOff) then
    exit;
{$IFDEF LINUX}
  FillChar(ev, sizeof(ev), 0);
  ev.data := C;
  if not readOff then ev.events := ev.events or EPOLLIN;
  if wantWrite then ev.events := ev.events or EPOLLOUT;
  epoll_ctl(Fd, EPOLL_CTL_MOD, C.FSock, @ev);
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
  kevent(Fd, @k[0], n, nil, 0, nil);
{$ENDIF}
  C.FWriteOn := wantWrite;
  C.FReadOff := readOff;
end;

procedure TLoop.AcceptAll;
var
  i: Integer;
  s: cint;
  c: TLoopConn;
begin
  if Server.FStop <> 0 then
    exit;
  for i := 0 to High(Listeners) do
    while true do
    begin
{$IFDEF LINUX}
      s := c_accept4(Listeners[i], nil, nil, SOCK_NONBLOCK or SOCK_CLOEXEC);
{$ELSE}
      s := c_accept(Listeners[i], nil, nil);
{$ENDIF}
      if s < 0 then
        break;                  { EAGAIN: no more now (another loop took it) }
{$IFDEF DARWIN}
      NetAdopt(s);              { non-blocking, no SIGPIPE }
{$ENDIF}
      NetSetNoDelay(s);
      c := TLoopConn.Create;
      c.FSock := s;
      c.FLoop := Self;
      c.FLastIO := GetTickCount64;
      c.FNext := Head;
      if Head <> nil then Head.FPrev := c;
      Head := c;
      inc(Count);
      Watch(c);
      if Assigned(Server.OnOpen) then
        Server.OnOpen(c);
      if c.FClosing then
        Finish(c);
    end;
end;

{ c is done: its callbacks are over, its record goes when this turn ends }
procedure TLoop.Bury(C: TLoopConn);
begin
  if C.FClosed then
    exit;
  C.FClosed := true;
  c_close(C.FSock);             { (drops its registrations) }
  if C.FPrev <> nil then C.FPrev.FNext := C.FNext else Head := C.FNext;
  if C.FNext <> nil then C.FNext.FPrev := C.FPrev;
  dec(Count);
  if Assigned(Server.OnClose) then
    Server.OnClose(C);
  C.FDeadNext := Dead;
  Dead := C;
end;

{ after a callback: send, then close it if it is finished }
procedure TLoop.Finish(C: TLoopConn);
begin
  if C.FClosed then
    exit;
  if C.OutPending > 0 then
    Flush(C);
  if C.FClosed then
    exit;
  if (C.FClosing or C.FPeerClosed) and (C.OutPending = 0) then
    Bury(C)
  else
    Interest(C);
end;

procedure TLoop.Flush(C: TLoopConn);
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

procedure TLoop.ReadSome(C: TLoopConn);
var
  n: Int64;
  got: Boolean;
begin
  got := false;
  while not C.FPeerClosed do
  begin
    C.Grow(C.FIn, C.FInCap, C.FInLen + 16384);
    n := c_recv(C.FSock, C.FIn + C.FInLen, C.FInCap - C.FInLen, 0);
    if n > 0 then
    begin
      C.FInLen := C.FInLen + n;
      C.FLastIO := GetTickCount64;
      got := true;
      if C.FInLen > Server.MaxInput then
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
  if got and Assigned(Server.OnData) and not C.FClosed then
    Server.OnData(C);
  Finish(C);
end;

procedure TLoop.Sweep(Now: Int64);
var
  c, nxt: TLoopConn;
begin
  if Server.IdleTimeoutMs <= 0 then
    exit;
  c := Head;
  while c <> nil do
  begin
    nxt := c.FNext;
    if Now - c.FLastIO > Server.IdleTimeoutMs then
    begin
      c.Abort;
      Bury(c);
    end;
    c := nxt;
  end;
end;

procedure TLoop.Main;
const
  MAXEV = 256;
var
  i, n: Integer;
  data: Pointer;
  c, nxt: TLoopConn;
  lastSweep, stopAt, now: Int64;
  stopping: Boolean;
{$IFDEF LINUX}
  evs: array[0..MAXEV - 1] of TEpollEvent;
  bits: Cardinal;
{$ELSE}
  evs: array[0..MAXEV - 1] of TKEvent;
  ts: TTimespec;
{$ENDIF}
begin
  for i := 0 to High(Listeners) do
    WatchListener(Listeners[i]);
  lastSweep := GetTickCount64;
  stopping := false;
  stopAt := 0;
  while true do
  begin
{$IFDEF LINUX}
    n := epoll_wait(Fd, @evs[0], MAXEV, 250);
{$ELSE}
    ts.sec := 0;
    ts.nsec := 250000000;
    n := kevent(Fd, nil, 0, @evs[0], MAXEV, @ts);
{$ENDIF}
    if n < 0 then
      n := 0;                   { EINTR }
    for i := 0 to n - 1 do
    begin
{$IFDEF LINUX}
      data := evs[i].data;
      bits := evs[i].events;
{$ELSE}
      data := evs[i].udata;
{$ENDIF}
      if data = nil then
      begin
        AcceptAll;
        continue;
      end;
      c := TLoopConn(data);
      if c.FClosed then
        continue;               { closed earlier this turn }
{$IFDEF LINUX}
      if (bits and EPOLLOUT) <> 0 then
        Finish(c);
      if not c.FClosed and ((bits and (EPOLLIN or EPOLLERR or EPOLLHUP)) <> 0) then
        if c.FReadOff then
          Finish(c)
        else
          ReadSome(c);
{$ELSE}
      if (evs[i].flags and EV_ERROR) <> 0 then
        continue;
      if evs[i].filter = EVFILT_WRITE then
        Finish(c)
      else if not c.FReadOff then
        ReadSome(c);
{$ENDIF}
    end;
    now := GetTickCount64;
    if now - lastSweep >= 1000 then
    begin
      Sweep(now);
      lastSweep := now;
    end;
    { Stop: no more reading; out with what is pending, then done }
    if not stopping and (Server.FStop <> 0) then
    begin
      stopping := true;
      stopAt := now + 2000;
      c := Head;
      while c <> nil do
      begin
        nxt := c.FNext;
        c.FClosing := true;
        Finish(c);
        c := nxt;
      end;
    end;
    { the graveyard: no event of a later turn names them }
    while Dead <> nil do
    begin
      c := Dead;
      Dead := c.FDeadNext;
      c.Free;
    end;
    if stopping and ((Head = nil) or (now >= stopAt)) then
      break;
  end;
  { whatever is left }
  while Head <> nil do
  begin
    Head.Abort;
    Bury(Head);
  end;
  while Dead <> nil do
  begin
    c := Dead;
    Dead := c.FDeadNext;
    c.Free;
  end;
  c_close(Fd);
end;

function LoopThread(Arg: Pointer): Pointer; cdecl;
begin
  TLoop(Arg).Main;
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
    n := c_sysconf(SC_NPROCESSORS_ONLN);
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
  l: TLoop;
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
    l := TLoop.Create;
    l.Server := Self;
    l.Index := i;
    l.Listeners := FListen[i];
{$IFDEF LINUX}
    l.Fd := epoll_create1(0);
{$ELSE}
    l.Fd := kqueue;
{$ENDIF}
    if l.Fd < 0 then
      raise ENetError.Create('netloop: no event queue');
    FLoops[i] := l;
  end;
  if FThreads > 1 then
    pas_start_threads;
  for i := 1 to FThreads - 1 do
    if pthread_create(TLoop(FLoops[i]).Tid, nil, @LoopThread, FLoops[i]) <> 0 then
      raise ENetError.Create('netloop: cannot start a loop thread');
  TLoop(FLoops[0]).Main;
  for i := 1 to FThreads - 1 do
    pthread_join(TLoop(FLoops[i]).Tid, nil);
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
