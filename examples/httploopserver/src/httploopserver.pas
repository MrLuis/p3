program httploopserver;
(* An HTTP/1.1 server on the loop engine: p3's netloop unit, event loops
   over non-blocking sockets (epoll on Linux, kqueue on macOS), one per
   thread, with nethttp's HTTP/1.1 on top and netrouter's routing (unit
   site).

     httploopserver [--port N] [--root DIR] [--any] [--threads N] [--requests N]

   HttpServeLoop installs the loop's callbacks: every whole request a
   connection has received (keep-alive, pipelining) is parsed, routed and
   answered; netloop sends the answers as the socket takes them. No task,
   no stack per connection: an idle connection costs its object (and its
   THttpContext). The price: a handler
   runs on its loop's thread and must not wait, or every connection of
   that loop waits with it. Compare httpasyncserver: the same site on
   netasync tasks, where a handler may. *)
{$H+}
uses sysutils, netloop, nethttp, site;

var
  Port: longint = 8080;
  AnyAddress: boolean = false;
  Threads: longint = 0;     { 0: one loop per CPU }
  MaxRequests: longint = 0; { 0: no limit }
  Srv: TNetLoopServer;

procedure Usage;
begin
  Writeln('usage: httploopserver [--port N] [--root DIR] [--any] [--threads N] [--requests N]');
  halt(1);
end;

procedure ParseArguments;
var
  i: longint;
  a: string;

  function Value: string;
  begin
    if i = ParamCount then
      Usage;
    i := i + 1;
    Value := ParamStr(i);
  end;

begin
  i := 1;
  while i <= ParamCount do
  begin
    a := ParamStr(i);
    if a = '--port' then
      Port := StrToIntDef(Value, -1)
    else if a = '--root' then
      SiteRoot := Value
    else if a = '--any' then
      AnyAddress := true
    else if a = '--threads' then
      Threads := StrToIntDef(Value, -1)
    else if a = '--requests' then
      MaxRequests := StrToIntDef(Value, -1)
    else
      Usage;
    i := i + 1;
  end;
  if (Port < 0) or (Port > 65535) or (MaxRequests < 0) or (Threads < 0) then
    Usage;
end;

{ after each answer (on the loop's thread) }
procedure Answered(C: THttpContext);
begin
  if (SiteServed.Increment = MaxRequests) and (MaxRequests > 0) then
    Srv.Stop;               { the loops send what is pending, then Run returns }
end;

var
  host: string;
begin
  ParseArguments;
  SiteEngine := 'httploopserver (netloop event loops)';
  NewSite;
  Srv := TNetLoopServer.Create(Threads);
  Srv.IdleTimeoutMs := 75000;
  if AnyAddress then host := '' else host := '127.0.0.1';
  Srv.Listen(host, Port);
  { nethttp: parse every request received, route it, send the answer }
  HttpServeLoop(Srv, RouterHandler(SiteRouter), @Answered);
  Writeln('httploopserver on http://127.0.0.1:', Srv.BoundPort, '/ (', Srv.Threads, ' loops)');
  Flush(Output);
  Srv.Run;
  Writeln('stopped after ', MaxRequests, ' request(s)');
  Srv.Free;
end.
