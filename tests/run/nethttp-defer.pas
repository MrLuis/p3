{ nethttp's deferred answers on netloop, finished from other callbacks of
  the loop: a timer (TNetLoop.Every) and a watched descriptor (WatchFd on a
  pipe). Checks: answers keep the order of pipelined requests behind a
  deferred one; a client that half-closes still gets its answer (Hold); a
  client that leaves while its answer is deferred frees its context when
  Complete comes (C.Data's destructor counts); Complete inside the handler
  answers at once; the server goes on.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/nethttp-defer.expected
  }
{$mode objfpc}{$H+}
program nethttp_defer;
uses sysutils, threading, netasync, netloop, netrouter, nethttp;

type
  TCount = specialize TAtomic<Integer>;
  { per connection: counts its frees }
  TConnData = class
    destructor Destroy; override;
  end;
  { the loop's: what waits for the timer and for the pipe }
  TPending = class
    Timer: array of THttpContext;
    Pipe: array of THttpContext;
    PipeIn, PipeOut: Integer;
  end;

var
  Freed: TCount;

destructor TConnData.Destroy;
begin
  Freed.Increment;
  inherited Destroy;
end;

function c_pipe(fds: Pointer): LongInt; cdecl; external 'c' name 'pipe';
function c_write(fd: LongInt; buf: Pointer; n: Int64): Int64; cdecl; external 'c' name 'write';
function c_read(fd: LongInt; buf: Pointer; n: Int64): Int64; cdecl; external 'c' name 'read';
function c_shutdown(fd, how: LongInt): LongInt; cdecl; external 'c' name 'shutdown';

function Pending(C: THttpContext): TPending;
begin
  Result := TPending(C.Conn.Loop.Data);
end;

{ the timer: every deferred /timer request is answered }
procedure Tick(L: TNetLoop);
var
  p: TPending;
  list: array of THttpContext;
  i: Integer;
begin
  p := TPending(L.Data);
  list := p.Timer;
  SetLength(p.Timer, 0);
  for i := 0 to High(list) do
    list[i].Complete;
end;

{ the pipe: one byte per /pipe request, answered in order }
procedure PipeReady(W: TLoopWatch; Readable, Writable: Boolean);
var
  p: TPending;
  b: array[0..63] of Byte;
  n, i: Int64;
  k: Integer;
  c: THttpContext;
begin
  p := TPending(W.Obj);
  n := c_read(W.Fd, @b[0], 64);
  for i := 1 to n do
  begin
    c := p.Pipe[0];
    for k := 1 to High(p.Pipe) do
      p.Pipe[k - 1] := p.Pipe[k];
    SetLength(p.Pipe, Length(p.Pipe) - 1);
    c.Complete;
  end;
end;

procedure LoopStart(L: TNetLoop);
var
  p: TPending;
  fds: array[0..1] of Integer;
  w: TLoopWatch;
begin
  p := TPending.Create;
  c_pipe(@fds[0]);
  p.PipeIn := fds[0];
  p.PipeOut := fds[1];
  L.Data := p;
  w := L.WatchFd(p.PipeIn, true, false, @PipeReady);
  w.Obj := p;
  L.Every(30, @Tick);
end;

const
  NL = #13#10;

function Exchange(Port: Integer; const Req: string; Half: Boolean): string;
var
  s: TSocket;
  buf: array[0..65535] of byte;
  n: LongInt;
  chunk: string;
begin
  s := Wait(NetConnect('127.0.0.1', Port));
  Wait(NetSendPtr(s, PChar(Req), Length(Req)));
  if Half then
    c_shutdown(s, 1);         { (SHUT_WR) }
  Result := '';
  while true do
  begin
    n := Wait(NetReadInto(s, @buf, sizeof(buf)));
    if n <= 0 then
      break;
    SetLength(chunk, n);
    Move(buf[0], chunk[1], n);
    Result := Result + chunk;
  end;
  NetClose(s);
end;

{ the bodies of the answers, in order }
function Bodies(const Raw: string): string;
var
  rest, head: string;
  e, p, cl: Integer;
begin
  Result := '';
  rest := Raw;
  while rest <> '' do
  begin
    e := Pos(NL + NL, rest);
    if e = 0 then
      exit(Result + ' <cut>');
    head := Copy(rest, 1, e - 1);
    p := Pos('Content-Length: ', head);
    cl := StrToIntDef(Copy(head, p + 16, Pos(NL, Copy(head, p, Length(head))) - 17), 0);
    if Result <> '' then
      Result := Result + ' | ';
    Result := Result + Copy(rest, e + 4, cl);
    rest := Copy(rest, e + 4 + cl, Length(rest));
  end;
end;

function RunLoop(Srv: TNetLoopServer): async;
begin
  Srv.Run;
end;

var
  r: THttpRouter;
  srv: TNetLoopServer;
  task: async;
  port, i: Integer;
  s: TSocket;
  req: string;
begin
  Freed := TCount.Create(0);
  r := THttpRouter.Create;
  r.Use(function(Next: THttpRouter.THandler): THttpRouter.THandler =>
    procedure(C: THttpContext; R: TRouteContext) => begin
      if C.Data = nil then C.Data := TConnData.Create;
      Next(C, R);
    end);
  r.Get('/now', procedure(C: THttpContext; R: TRouteContext) => C.Text(200, 'now'));
  r.Get('/timer/{n}', procedure(C: THttpContext; R: TRouteContext) => begin
    C.Text(200, 'timer ' + R.URLParam('n'));     { (filled now, sent later) }
    C.Defer;
    var p := Pending(C);
    SetLength(p.Timer, Length(p.Timer) + 1);
    p.Timer[High(p.Timer)] := C;
  end);
  r.Get('/pipe/{n}', procedure(C: THttpContext; R: TRouteContext) => begin
    C.Text(200, 'pipe ' + R.URLParam('n'));
    C.Defer;
    var p := Pending(C);
    SetLength(p.Pipe, Length(p.Pipe) + 1);
    p.Pipe[High(p.Pipe)] := C;
    var b: Byte := 1;
    c_write(p.PipeOut, @b, 1);
  end);
  r.Get('/sync', procedure(C: THttpContext; R: TRouteContext) => begin
    C.Defer;
    C.Text(200, 'sync');
    C.Complete;                                   { before the handler returns }
  end);

  srv := TNetLoopServer.Create(1);
  srv.Listen('127.0.0.1', 0);
  srv.OnLoopStart := @LoopStart;
  HttpServeLoop(srv, RouterHandler(r));
  task := Pool.Run(RunLoop(srv));
  port := srv.BoundPort;

  Writeln('timer: ', Bodies(Exchange(port, 'GET /timer/1 HTTP/1.1' + NL + 'Connection: close' + NL + NL, false)));
  Writeln('pipe: ', Bodies(Exchange(port, 'GET /pipe/1 HTTP/1.1' + NL + 'Connection: close' + NL + NL, false)));
  Writeln('sync: ', Bodies(Exchange(port, 'GET /sync HTTP/1.1' + NL + 'Connection: close' + NL + NL, false)));
  req := 'GET /timer/a HTTP/1.1' + NL + NL + 'GET /now HTTP/1.1' + NL + NL +
         'GET /pipe/b HTTP/1.1' + NL + NL + 'GET /sync HTTP/1.1' + NL + NL +
         'GET /timer/c HTTP/1.1' + NL + 'Connection: close' + NL + NL;
  Writeln('pipelined: ', Bodies(Exchange(port, req, false)));
  Writeln('half-closed: ', Bodies(Exchange(port, 'GET /timer/h HTTP/1.1' + NL + NL, true)));
  { clients that leave while their answers are deferred }
  for i := 1 to 20 do
  begin
    s := Wait(NetConnect('127.0.0.1', port));
    req := 'GET /timer/x HTTP/1.1' + NL + NL;
    Wait(NetSendPtr(s, PChar(req), Length(req)));
    NetClose(s);
  end;
  Sleep(300);
  Writeln('after 20 clients left: ', Bodies(Exchange(port, 'GET /now HTTP/1.1' + NL + 'Connection: close' + NL + NL, false)));
  srv.Stop;
  Wait(task);
  Writeln('contexts freed: ', Freed.Load, ' (of 26 connections)');
end.
