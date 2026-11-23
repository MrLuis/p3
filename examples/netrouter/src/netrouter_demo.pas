(* netrouter demo: a small HTTP/1.1 server (keep-alive, GET/POST without
   bodies) on netasync, its routing done by netrouter.

   The router knows nothing about HTTP: this program reads the request
   line, gives the router the method and the path, and a TConn of its own;
   the handlers write the response through TConn. Each connection is a
   task on the pool (Pool.Run); the handlers run inside it, so one that
   waits (Wait, Sleep) parks only its connection.

     bin/netrouter_demo [port]        (default 8080)
     curl localhost:8080/
     curl localhost:8080/hello/ann
     curl localhost:8080/posts/42
     curl localhost:8080/posts/abc          -> the slug route
     curl localhost:8080/api/v1/uptime
     curl localhost:8080/api/v1/routes
     curl -X DELETE localhost:8080/posts/42 -> 405, Allow: GET
     curl localhost:8080/admin -H 'X-Token: secret'
*)
program netrouter_demo;

{$H+}

uses sysutils, threading, netasync, netrouter;

type
  { what a handler gets: the request's few fields, and the response }
  TConn = class
    Sock: TSocket;
    Headers: string;        { the raw header lines }
    Status: Integer;
    Extra: string;          { extra response header lines }
    Body: string;
    function Header(const Name: string): string;
    procedure Reply(Code: Integer; const Text: string);
  end;
  TApp = specialize TRouter<TConn>;

var
  App: TApp;
  Started: Int64;

{ every route, one per line (TRouter.Walk) }
function RouteList: string;
var
  list: string;
begin
  list := '';
  App.Walk(procedure(M, P) => list := list + M + ' ' + P + #10);
  Result := TrimRight(list);
end;

function TConn.Header(const Name: string): string;
var
  p, e: Integer;
  low: string;
begin
  low := LowerCase(Headers);
  p := Pos(#10 + LowerCase(Name) + ':', low);
  if p = 0 then
    exit('');
  p := p + Length(Name) + 2;
  e := p;
  while (e <= Length(Headers)) and (Headers[e] <> #13) and (Headers[e] <> #10) do
    inc(e);
  Result := Trim(Copy(Headers, p, e - p));
end;

procedure TConn.Reply(Code: Integer; const Text: string);
begin
  Status := Code;
  Body := Text + #10;
end;

{ ---- middleware ---- }

{ one log line per request, after it ran }
function Logger(Next: TApp.THandler): TApp.THandler;
begin
  Result := procedure(C: TConn; R: TRouteContext) => begin
    var t0 := GetTickCount64;
    Next(C, R);
    Writeln(R.Method, ' ', R.Path, ' -> ', C.Status, ' [', R.RoutePattern, '] ',
            GetTickCount64 - t0, ' ms');
  end;
end;

{ the admin routes want X-Token: secret }
function RequireToken(Next: TApp.THandler): TApp.THandler;
begin
  Result := procedure(C: TConn; R: TRouteContext) => begin
    if C.Header('X-Token') <> 'secret' then
      C.Reply(401, 'unauthorized')
    else
      Next(C, R);
  end;
end;

{ ---- handlers ---- }

function Slow(ms: Integer): async Integer;
begin
  Sleep(ms);          { parks this connection's task only }
  Result := ms;
end;

procedure Routes;
begin
  App := TApp.Create;
  App.Use(@Logger);
  App.Get('/', procedure(C: TConn; R: TRouteContext) => C.Reply(200, 'netrouter demo'));
  App.Get('/hello/{name}', procedure(C: TConn; R: TRouteContext) =>
    C.Reply(200, 'hello, ' + R.URLParam('name')));
  App.Get('/posts/{id:[0-9]+}', procedure(C: TConn; R: TRouteContext) =>
    C.Reply(200, 'post #' + R.URLParam('id')));
  App.Get('/posts/{slug}', procedure(C: TConn; R: TRouteContext) =>
    C.Reply(200, 'post "' + R.URLParam('slug') + '"'));
  App.Get('/slow/{ms:[0-9]{1,4}}', procedure(C: TConn; R: TRouteContext) =>
    C.Reply(200, 'waited ' + IntToStr(Wait(Slow(StrToInt(R.URLParam('ms'))))) + ' ms'));
  App.Route('/api/v1', procedure(api: TApp) => begin
    api.Get('/uptime', procedure(C: TConn; R: TRouteContext) =>
      C.Reply(200, IntToStr((GetTickCount64 - Started) div 1000) + ' s'));
    api.Get('/routes', procedure(C: TConn; R: TRouteContext) => C.Reply(200, RouteList));
  end);
  App.Using(@RequireToken).Get('/admin', procedure(C: TConn; R: TRouteContext) =>
    C.Reply(200, 'welcome, admin'));
  App.NotFound(procedure(C: TConn; R: TRouteContext) => C.Reply(404, 'no route for ' + R.Path));
  App.MethodNotAllowed(procedure(C: TConn; R: TRouteContext) => begin
    C.Extra := 'Allow: ' + R.AllowedMethods + #13#10;
    C.Reply(405, 'method not allowed');
  end);
end;

{ ---- the HTTP part: just enough to feed the router ---- }

function StatusText(Code: Integer): string;
begin
  case Code of
    200: Result := 'OK';
    401: Result := 'Unauthorized';
    404: Result := 'Not Found';
    405: Result := 'Method Not Allowed';
  else
    Result := 'Error';
  end;
end;

function Serve(sock: TSocket): async;
var
  c: TConn;
  rc: TRouteContext;
  buf: array[0..8191] of byte;
  pending, line, method, target, path, resp: string;
  n, e, sp1, sp2, q: Integer;
begin
  c := TConn.Create;
  c.Sock := sock;
  rc := TRouteContext.Create;   { one per connection, reused per request }
  pending := '';
  try
    while true do
    begin
      { a request: up to the blank line (no bodies in this demo) }
      e := Pos(#13#10#13#10, pending);
      while e = 0 do
      begin
        n := await NetReadInto(sock, @buf, sizeof(buf));
        if n <= 0 then
          exit;
        SetLength(line, n);
        move(buf, line[1], n);
        pending := pending + line;
        e := Pos(#13#10#13#10, pending);
      end;
      line := Copy(pending, 1, Pos(#13#10, pending) - 1);
      c.Headers := Copy(pending, Length(line) + 1, e - Length(line) + 1);
      Delete(pending, 1, e + 3);
      sp1 := Pos(' ', line);
      sp2 := Pos(' ', Copy(line, sp1 + 1, Length(line))) + sp1;
      if (sp1 = 0) or (sp2 = sp1) then
        exit;
      method := Copy(line, 1, sp1 - 1);
      target := Copy(line, sp1 + 1, sp2 - sp1 - 1);
      q := Pos('?', target);
      if q > 0 then
        path := Copy(target, 1, q - 1)
      else
        path := target;
      c.Status := 200;
      c.Extra := '';
      c.Body := '';
      { the router does the rest; without a handler for the outcome the
        caller would answer here }
      App.Serve(method, path, c, rc);
      resp := 'HTTP/1.1 ' + IntToStr(c.Status) + ' ' + StatusText(c.Status) + #13#10 +
              'Content-Type: text/plain' + #13#10 + c.Extra +
              'Content-Length: ' + IntToStr(Length(c.Body)) + #13#10#13#10 + c.Body;
      await NetSendPtr(sock, PChar(resp), Length(resp));
    end;
  finally
    rc.Free;
    c.Free;
    NetClose(sock);
  end;
end;

{ each connection on the pool: a task started any other way stays on the
  acceptor's thread }
function Launch(sock: TSocket): async;
begin
  await Pool.Run(Serve(sock));
end;

procedure Dropped(E: Exception);
begin
end;

function Acceptor(listener: TSocket): async;
begin
  while true do
    detached(Dropped) Launch(await NetAccept(listener));
end;

var
  port: Integer;
  opt: TNetListenOpts;
begin
  port := StrToIntDef(ParamStr(1), 8080);
  Started := GetTickCount64;
  Routes;
  opt.ReuseAddr := true;
  opt.ReusePort := false;
  var lst := NetListenTo('127.0.0.1', port, 128, opt);
  Writeln('netrouter demo on http://127.0.0.1:', NetBoundPort(lst));
  Wait(Acceptor(lst));
end.
