unit site;
(* The site both example servers serve: a netrouter router over nethttp's
  request context (THttpRouter, THttpContext), its routes and middleware.
  The servers only hand it to an engine (nethttp's HttpServeLoop or
  HttpServeAsync), which parses the requests and sends the answers.

    GET  /                  the root's index.html
    GET  /*                 a file under the root (a directory: its index.html)
    GET  /status            a page the server makes
    GET  /hello/{name}      a URL parameter
    GET  /posts/{id:[0-9]+} a parameter with an expression (else 404)
    GET  /api/echo/{word}   a sub-router whose middleware makes answers JSON
    GET  /api/routes        every route (TRouter.Walk), as JSON
  HEAD works wherever GET does; another method on a known path is a 405
  with an Allow header; an unknown path a 404 page. Each request is logged
  as "GET /hello/ann 200" by the Logger middleware. *)
{$H+}
interface

uses sysutils, threading, netrouter, nethttp;

type
  TSite = THttpRouter;
  TCounter = specialize TAtomic<longint>;

var
  SiteRoot: string = 'public';
  SiteEngine: string = '';      { shown by /status }
  SiteServed: TCounter;         { requests answered (the servers count) }
  SiteStarted: int64;
  SiteRouter: TSite;            { the router NewSite made }

{ The router with the site's routes (a server may add its own before it
  serves). }
function NewSite: TSite;
{ an HTML error page as the answer }
procedure Fail(C: THttpContext; Code: longint; const Detail: string);

implementation

procedure Fail(C: THttpContext; Code: longint; const Detail: string);
var
  title: string;
begin
  title := IntToStr(Code) + ' ' + HttpStatusText(Code);
  C.Html(Code, '<!doctype html><html><head><title>' + title + '</title></head><body><h1>' +
         title + '</h1><p>' + HttpHtmlEscape(Detail) + '</p></body></html>' + #10);
end;

{ ---- middleware ---- }

{ one log line per request, once it has been answered }
function Logger(Next: TSite.THandler): TSite.THandler;
begin
  Result := procedure(C: THttpContext; R: TRouteContext) => begin
    Next(C, R);
    Writeln(C.Method, ' ', C.Target, ' ', C.Status);
    Flush(Output);
  end;
end;

{ the /api sub-router answers JSON }
function JsonAnswers(Next: TSite.THandler): TSite.THandler;
begin
  Result := procedure(C: THttpContext; R: TRouteContext) => begin
    Next(C, R);
    if C.Status = 200 then
      C.ContentType := 'application/json';
  end;
end;

{ ---- handlers ---- }

function JsonString(const s: string): string;
var
  i: longint;
begin
  Result := '"';
  for i := 1 to Length(s) do
    case s[i] of
      '"': Result := Result + '\"';
      '\': Result := Result + '\\';
      #0..#31: Result := Result + '\u00' + IntToHex(Ord(s[i]), 2);
    else
      Result := Result + s[i];
    end;
  Result := Result + '"';
end;

{ the file at Path under the root (a directory: its index.html) }
procedure ServeUnderRoot(C: THttpContext; const Path: string);
var
  name: string;
begin
  if not HttpSafePath(Path) then
  begin
    Fail(C, 403, 'The path leaves the served directory.');
    exit;
  end;
  name := SiteRoot + Path;
  if not FileExists(name) and not FileExists(name + '/index.html') then
  begin
    Fail(C, 404, 'There is no ' + C.Path + ' here.');
    exit;
  end;
  ServeFile(C, name);   { nethttp: content type, a directory's index.html }
end;

procedure Home(C: THttpContext; R: TRouteContext);
begin
  ServeUnderRoot(C, '/');
end;

procedure AnyFile(C: THttpContext; R: TRouteContext);
begin
  ServeUnderRoot(C, '/' + R.URLParam('*'));
end;

procedure Status(C: THttpContext; R: TRouteContext);
begin
  C.Html(200,
    '<!doctype html><html><head><title>Status</title></head><body>' +
    '<h1>' + HttpHtmlEscape(SiteEngine) + '</h1><ul>' +
    '<li>requests served: ' + IntToStr(SiteServed.Load) + '</li>' +
    '<li>running for: ' + IntToStr((GetTickCount64 - SiteStarted) div 1000) + ' s</li>' +
    '<li>root: ' + HttpHtmlEscape(SiteRoot) + '</li>' +
    '</ul><p><a href="/">home</a></p></body></html>' + #10);
end;

function RouteList: string;
var
  list: string;
  site: TSite;
begin
  list := '';
  site := SiteRouter;
  site.Walk(procedure(M, P) => begin
    if list <> '' then
      list := list + ',';
    list := list + '{"method":' + JsonString(M) + ',"pattern":' + JsonString(P) + '}';
  end);
  Result := '[' + list + ']';
end;

function NewSite: TSite;
var
  s: TSite;
begin
  s := TSite.Create;
  s.Use(@Logger);
  s.Get('/', @Home);
  s.Head('/', @Home);
  s.Get('/*', @AnyFile);
  s.Head('/*', @AnyFile);
  s.Get('/status', @Status);
  s.Get('/hello/{name}', procedure(C: THttpContext; R: TRouteContext) =>
    C.Text(200, 'Hello, ' + R.URLParam('name') + '!' + #10));
  s.Get('/posts/{id:[0-9]+}', procedure(C: THttpContext; R: TRouteContext) =>
    C.Text(200, 'post #' + R.URLParam('id') + #10));
  s.Route('/api', procedure(api: TSite) => begin
    api.Use(@JsonAnswers);
    api.Get('/echo/{word}', procedure(C: THttpContext; R: TRouteContext) =>
      C.Send(200, 'text/plain', '{"word":' + JsonString(R.URLParam('word')) + '}'));
    api.Get('/routes', procedure(C: THttpContext; R: TRouteContext) =>
      C.Send(200, 'text/plain', RouteList));
  end);
  s.NotFound(procedure(C: THttpContext; R: TRouteContext) =>
    Fail(C, 404, 'There is no ' + C.Path + ' here.'));
  s.MethodNotAllowed(procedure(C: THttpContext; R: TRouteContext) => begin
    C.SetHeader('Allow', R.AllowedMethods);
    Fail(C, 405, 'This path takes ' + R.AllowedMethods + '.');
  end);
  SiteRouter := s;
  Result := s;
end;

initialization
  SiteServed := TCounter.Create(0);
  SiteStarted := GetTickCount64;
end.
