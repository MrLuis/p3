{ netrouter: chi-style routing over a context type of the program's own —
  static, parameter, expression, mixed and catch-all segments and their
  priority (with backtracking), 404 and 405, middleware order and
  short-circuit, groups, sub-routers (Route, Mount) and their patterns,
  Walk, configuration errors, the expression engine, and serving from
  several threads at once.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/netrouter-routes.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/netrouter-routes.expected
  }
{$mode objfpc}{$H+}
program netrouter_test;
uses sysutils, threading, netrouter;

type
  TReq = class
    Log: string;
    User: string;
  end;
  TApp = specialize TRouter<TReq>;

{ a handler that says which route ran, with its parameters }
function Says(Name: string): TApp.THandler;
begin
  Result := procedure(C: TReq; R: TRouteContext) => begin
    C.Log := C.Log + Name;
    for var i := 0 to R.ParamCount - 1 do
      C.Log := C.Log + ' ' + R.ParamName(i) + '=' + R.ParamValue(i);
  end;
end;

{ middleware that brackets the rest of the chain }
function Tag(Name: string): TApp.TMiddleware;
begin
  Result := function(Next: TApp.THandler): TApp.THandler =>
    procedure(C: TReq; R: TRouteContext) => begin
      C.Log := C.Log + Name + '(';
      Next(C, R);
      C.Log := C.Log + ')';
    end;
end;

{ middleware that stops the chain unless the request has a user }
function RequireUser(Next: TApp.THandler): TApp.THandler;
begin
  Result := procedure(C: TReq; R: TRouteContext) => begin
    if C.User = '' then
      C.Log := C.Log + 'denied'
    else
      Next(C, R);
  end;
end;

var
  app, api: TApp;
  req: TReq;
  rc: TRouteContext;

procedure Probe(const M, P: string; const User: string = '');
var
  res: TRouteResult;
begin
  req.Log := '';
  req.User := User;
  res := app.Serve(M, P, req, rc);
  case res of
    rrMatched: Writeln(M, ' ', P, ' -> ', req.Log, '  [', rc.RoutePattern, ']');
    rrNotFound: Writeln(M, ' ', P, ' -> 404 ', req.Log);
    rrMethodNotAllowed: Writeln(M, ' ', P, ' -> 405 allow: ', rc.AllowedMethods, ' ', req.Log);
  end;
end;

procedure Expect(const What: string; Ok: Boolean);
begin
  if Ok then Writeln(What, ': ok') else Writeln(What, ': FAIL');
end;

procedure ConfigErrors;
var
  r: TApp;
begin
  r := TApp.Create;
  r.Get('/x', Says('x'));
  try
    r.Use(Tag('late'));
    Writeln('use after routes: not raised');
  except
    on e: ERouterError do Writeln('use after routes: raised');
  end;
  try
    r.Get('nope', Says('x'));
  except
    on e: ERouterError do Writeln('no leading slash: raised');
  end;
  try
    r.Get('/a/*/b', Says('x'));
  except
    on e: ERouterError do Writeln('star not last: raised');
  end;
  r.Get('/u/{id}', Says('u'));
  try
    r.Get('/u/{name}/more', Says('x'));
  except
    on e: ERouterError do Writeln('conflicting names: raised');
  end;
  try
    r.Get('/r/{v:[0-9}', Says('x'));
  except
    on e: ERouterError do Writeln('bad expression: raised');
  end;
  try
    r.Get('/m/{a}{b}', Says('x'));
  except
    on e: ERouterError do Writeln('adjacent parameters: raised');
  end;
  r.Free;
end;

procedure Regexes;
  function M(const Expr, V: string): Boolean;
  var
    rx: TRegex;
  begin
    rx := TRegex.Create(Expr);
    Result := rx.Matches(PAnsiChar(V), Length(V));
    rx.Free;
  end;
begin
  Expect('rx digits', M('[0-9]+', '2025') and not M('[0-9]+', '20a5') and not M('[0-9]+', ''));
  Expect('rx \d{4}', M('\d{4}', '1999') and not M('\d{4}', '199') and not M('\d{4}', '19999'));
  Expect('rx {2,3}', M('a{2,3}', 'aa') and M('a{2,3}', 'aaa') and not M('a{2,3}', 'aaaa'));
  Expect('rx alternation', M('0[1-9]|1[0-2]', '07') and M('0[1-9]|1[0-2]', '12') and not M('0[1-9]|1[0-2]', '13'));
  Expect('rx group+', M('(ab)+c', 'ababc') and not M('(ab)+c', 'abac'));
  Expect('rx class', M('[a-z-]+', 'hello-world') and not M('[a-z-]+', 'Hello'));
  Expect('rx negated', M('[^.]+', 'abc') and not M('[^.]+', 'a.c'));
  Expect('rx anchors', M('^v\d+$', 'v12') and not M('^v\d+$', 'xv12'));
  Expect('rx optional', M('colou?r', 'color') and M('colou?r', 'colour'));
  Expect('rx dot star', M('a.*z', 'az') and M('a.*z', 'abcz') and not M('a.*z', 'abc'));
  Expect('rx backtrack', M('(a|ab)(c|bcd)', 'abcd'));
end;

{ the router is read-only while serving: tasks on several threads at once }
function Hammer(n: Integer): async Integer;
var
  c: TReq;
  ctx: TRouteContext;
  i: Integer;
begin
  c := TReq.Create;
  ctx := TRouteContext.Create;
  Result := 0;
  for i := 1 to n do
  begin
    c.Log := '';
    if (app.Serve('GET', '/posts/' + IntToStr(i), c, ctx) = rrMatched) and
       (ctx.URLParam('id') = IntToStr(i)) then
      inc(Result);
  end;
  ctx.Free;
  c.Free;
end;

var
  t: array[1..4] of async Integer;
  total, i: Integer;
begin
  app := TApp.Create;
  app.Use(Tag('log'));
  app.Get('/', Says('home'));
  app.Get('/posts', Says('list'));
  app.Post('/posts', Says('create'));
  app.Get('/posts/new', Says('new-form'));
  app.Get('/posts/{id:[0-9]+}', Says('show'));
  app.Get('/posts/{slug}', Says('by-slug'));
  app.Delete('/posts/{id:[0-9]+}', Says('delete'));
  app.Get('/files/{name}.{ext}', Says('file'));
  app.Get('/archive/{year:\d{4}}/{month:0[1-9]|1[0-2]}', Says('archive'));
  app.Get('/static/*', Says('static'));
  app.Get('/a/b/d', Says('abd'));
  app.Get('/a/{x}/c', Says('axc'));
  app.Handle('/any', Says('any'));
  app.Method('PURGE', '/cache', Says('purge'));
  app.Using(@RequireUser).Get('/me', Says('me'));
  app.Group(procedure(g: TApp) => begin
    g.Use(Tag('admin'));
    g.Get('/admin/stats', Says('stats'));
  end);
  app.Route('/users/{uid}', procedure(u: TApp) => begin
    u.Use(Tag('users'));
    u.Get('/', Says('user'));
    u.Get('/posts/{pid}', Says('user-post'));
  end);
  api := TApp.Create;
  api.Get('/', Says('api-root'));
  api.Get('/v1/ping', Says('pong'));
  api.Get('/v1/items/{id}', Says('item'));
  app.Mount('/api', api);
  app.NotFound(procedure(C: TReq; R: TRouteContext) => C.Log := C.Log + 'not-found');
  rc := TRouteContext.Create;
  req := TReq.Create;

  Writeln('-- static, parameters, priority');
  Probe('GET', '/');
  Probe('GET', '/posts');
  Probe('POST', '/posts');
  Probe('GET', '/posts/new');
  Probe('GET', '/posts/42');
  Probe('GET', '/posts/hello-world');
  Probe('DELETE', '/posts/42');
  Probe('GET', '/files/report.tar.gz');
  Probe('GET', '/archive/2024/07');
  Probe('GET', '/archive/2024/13');
  Probe('GET', '/static/css/site.css');
  Probe('GET', '/static/');
  Writeln('-- backtracking, any method, custom method');
  Probe('GET', '/a/b/c');
  Probe('GET', '/a/b/d');
  Probe('PATCH', '/any');
  Probe('PURGE', '/cache');
  Writeln('-- 404 / 405');
  Probe('GET', '/nowhere');
  Probe('GET', '/posts/');
  Probe('PUT', '/posts');
  Probe('DELETE', '/posts/hello-world');
  Writeln('-- middleware, groups');
  Probe('GET', '/me');
  Probe('GET', '/me', 'ann');
  Probe('GET', '/admin/stats');
  Writeln('-- sub-routers');
  Probe('GET', '/users/7');
  Probe('GET', '/users/7/');
  Probe('GET', '/users/7/posts/99');
  Probe('GET', '/api');
  Probe('GET', '/api/');
  Probe('GET', '/api/v1/ping');
  Probe('GET', '/api/v1/items/x9');
  Probe('GET', '/api/v2/nothing');
  Writeln('-- without a context of the caller');
  req.Log := '';
  Writeln('own context: ', app.Serve('GET', '/posts/5', req) = rrMatched, ' ', req.Log);

  Writeln('-- walk');
  app.Walk(procedure(const M, P: string) => Writeln(M, ' ', P));

  Writeln('-- configuration errors');
  ConfigErrors;
  Writeln('-- expressions');
  Regexes;

  Writeln('-- threads');
  for i := 1 to 4 do
    t[i] := Pool.Run(Hammer(2000));
  total := 0;
  for i := 1 to 4 do
    total := total + Wait(t[i]);
  Writeln('served from 4 tasks: ', total);

  req.Free;
  rc.Free;
  app.Free;
  api.Free;
end.
