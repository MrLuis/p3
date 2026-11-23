program httpasyncserver;
(* An HTTP/1.1 server on the async engine: netasync tasks, one per
  connection, on the thread pool, with nethttp's HTTP/1.1 on top and
  netrouter's routing (unit site).

    httpasyncserver [--port N] [--root DIR] [--any] [--requests N]

  HttpServeAsync accepts; a connection's task reads requests (keep-alive,
  pipelining), routes each one, and sends the answer. A handler that waits (/sleep/{ms} here, a
  database query in a real server) parks its connection's task only; the
  pool threads go on with the other connections. Compare httploopserver:
  the same site on netloop's event loops, where no handler may wait. *)
{$H+}
uses sysutils, netasync, netrouter, nethttp, site;

var
  Port: longint = 8080;
  AnyAddress: boolean = false;
  MaxRequests: longint = 0; { 0: no limit }

procedure Usage;
begin
  Writeln('usage: httpasyncserver [--port N] [--root DIR] [--any] [--requests N]');
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
    else if a = '--requests' then
      MaxRequests := StrToIntDef(Value, -1)
    else
      Usage;
    i := i + 1;
  end;
  if (Port < 0) or (Port > 65535) or (MaxRequests < 0) then
    Usage;
end;

{ ---- what only this engine can do: a handler that waits ---- }

function Pause(ms: longint): async longint;
begin
  Sleep(ms);            { parks the task; the thread serves others }
  Result := ms;
end;

procedure AddAsyncRoutes(s: TSite);
begin
  s.Get('/sleep/{ms:[0-9]{1,4}}', procedure(C: THttpContext; R: TRouteContext) =>
    C.Text(200, 'slept ' + IntToStr(Wait(Pause(StrToInt(R.URLParam('ms'))))) + ' ms' + #10));
end;

{ after each answer (in the connection's task) }
procedure Answered(C: THttpContext);
begin
  if (SiteServed.Increment = MaxRequests) and (MaxRequests > 0) then
  begin
    Writeln('stopped after ', MaxRequests, ' request(s)');
    Flush(Output);
    halt(0);
  end;
end;

var
  opt: TNetListenOpts;
  host: string;
  listener: TSocket;
begin
  ParseArguments;
  SiteEngine := 'httpasyncserver (netasync tasks)';
  AddAsyncRoutes(NewSite);
  opt.ReuseAddr := true;
  opt.ReusePort := false;
  if AnyAddress then host := '' else host := '127.0.0.1';
  listener := NetListenTo(host, Port, 128, opt);
  Writeln('httpasyncserver on http://127.0.0.1:', NetBoundPort(listener), '/');
  Flush(Output);
  { nethttp: a task per connection on the pool; parses, routes, answers }
  Wait(HttpServeAsync(listener, RouterHandler(SiteRouter), @Answered));
end.
