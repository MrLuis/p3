{ nethttp serving one router on both engines (netloop's event loops and
  netasync's tasks), checked from raw sockets: keep-alive and pipelining,
  Content-Length and chunked POST bodies, the query, FileServer (and its
  403 for '..'), the default 404 and 405 (Allow), HEAD, a request that
  cannot be parsed (400, then the connection closes), and After.
  RUN: rm -rf %t.dir && mkdir -p %t.dir
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: cd %t.dir && %t | diff - %S/nethttp-serve.expected
  }
{$mode objfpc}{$H+}
program nethttp_serve;
uses sysutils, threading, netasync, netloop, netrouter, nethttp;

type
  TCount = specialize TAtomic<Integer>;

var
  Router: THttpRouter;
  Answers: TCount;

const
  NL = #13#10;

{ ---- a raw client: send, read until the server closes ---- }

function Exchange(Port: Integer; const Req: string): string;
var
  s: TSocket;
  buf: array[0..65535] of byte;
  n: LongInt;
  chunk: string;
begin
  s := Wait(NetConnect('127.0.0.1', Port));
  Wait(NetSendPtr(s, PChar(Req), Length(Req)));
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

{ the status lines and bodies of a run of answers, on one line }
function Summary(const Raw: string): string;
var
  rest, head, body: string;
  e, cl, p: Integer;
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
    cl := 0;
    if p > 0 then
      cl := StrToIntDef(Copy(head, p + 16, Pos(NL, Copy(head, p, Length(head))) - 17), 0);
    body := Copy(rest, e + 4, cl);
    if Result <> '' then
      Result := Result + ' | ';
    Result := Result + Copy(head, 10, 3);
    p := Pos('Allow: ', head);
    if p > 0 then
      Result := Result + ' [' + Copy(head, p, Pos(NL, Copy(head, p, Length(head)) + NL) - 1) + ']';
    if Pos('Connection: close', head) > 0 then
      Result := Result + ' (close)';
    if body <> '' then
      Result := Result + ' ' + TrimRight(body);
    rest := Copy(rest, e + 4 + cl, Length(rest));
  end;
end;

procedure Check(const Engine: string; Port: Integer);
  procedure Show(const What, Req: string);
  begin
    Writeln(Engine, ' ', What, ': ', Summary(Exchange(Port, Req)));
  end;
begin
  Show('hello', 'GET /hello/ann HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('keep-alive x3', 'GET /hello/a HTTP/1.1' + NL + NL + 'GET /hello/b HTTP/1.1' + NL + NL +
       'GET /hello/c HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('query', 'GET /q?name=b%20c&x=1 HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('POST length', 'POST /echo HTTP/1.1' + NL + 'Content-Length: 5' + NL +
       'Connection: close' + NL + NL + 'hello');
  Show('POST chunked', 'POST /echo HTTP/1.1' + NL + 'Transfer-Encoding: chunked' + NL +
       'Connection: close' + NL + NL + '3' + NL + 'abc' + NL + '2' + NL + 'de' + NL + '0' + NL + NL);
  Show('file', 'GET /files/note.txt HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('file index', 'GET /files/ HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('file missing', 'GET /files/none.txt HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('file ..', 'GET /files/%2e%2e/secret HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('file twice-encoded', 'GET /files/no%2574e.txt HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('HEAD', 'HEAD /hello/h HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('404', 'GET /nowhere HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('405', 'DELETE /hello/x HTTP/1.1' + NL + 'Connection: close' + NL + NL);
  Show('400 then closed', 'GET /hello/ok HTTP/1.1' + NL + NL + 'BROKEN' + NL + NL +
       'GET /hello/never HTTP/1.1' + NL + NL);
end;

procedure MakeFiles;
var
  f: text;
begin
  CreateDir('site');
  Assign(f, 'site/note.txt'); Rewrite(f); Writeln(f, 'a note'); Close(f);
  Assign(f, 'site/index.html'); Rewrite(f); Writeln(f, '<p>index</p>'); Close(f);
  Assign(f, 'secret'); Rewrite(f); Writeln(f, 'do not serve'); Close(f);
end;

function RunLoop(Srv: TNetLoopServer): async;
begin
  Srv.Run;
end;

var
  srv: TNetLoopServer;
  loopTask: async;
  opt: TNetListenOpts;
  lst: TSocket;
begin
  MakeFiles;
  Answers := TCount.Create(0);
  Router := THttpRouter.Create;
  Router.Get('/hello/{name}', procedure(C: THttpContext; R: TRouteContext) =>
    C.Text(200, 'hello ' + R.URLParam('name')));
  Router.Get('/q', procedure(C: THttpContext; R: TRouteContext) =>
    C.Text(200, 'name=' + C.QueryParam('name') + ' x=' + C.QueryParam('x')));
  Router.Post('/echo', procedure(C: THttpContext; R: TRouteContext) =>
    C.Text(200, 'got ' + C.Body));
  Router.Get('/files/*', FileServer('site'));
  Router.Head('/hello/{name}', procedure(C: THttpContext; R: TRouteContext) =>
    C.Text(200, 'hello ' + R.URLParam('name')));

  { the loop engine }
  srv := TNetLoopServer.Create(2);
  srv.Listen('127.0.0.1', 0);
  HttpServeLoop(srv, RouterHandler(Router), procedure(C: THttpContext) => Answers.Increment);
  loopTask := Pool.Run(RunLoop(srv));
  Check('loop ', srv.BoundPort);
  srv.Stop;
  Wait(loopTask);
  srv.Free;

  { the async engine }
  opt.ReuseAddr := true;
  opt.ReusePort := false;
  lst := NetListenTo('127.0.0.1', 0, 128, opt);
  detached HttpServeAsync(lst, RouterHandler(Router), procedure(C: THttpContext) => Answers.Increment);
  Check('async', NetBoundPort(lst));

  Writeln('answers (After): ', Answers.Load);
end.
