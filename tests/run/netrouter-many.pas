{ netrouter with many literal routes under one segment (found by a table):
  every route is reached, near misses are not, parameters and the catch-all
  still work beside them, several levels deep.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/netrouter-many.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/netrouter-many.expected
}
program netroutermany;
{$H+}
uses sysutils, netrouter;

type
  TReq = class
    Log: string;
  end;
  TApp = specialize TRouter<TReq>;

function Says(Name: string): TApp.THandler;
begin
  Result := procedure(C: TReq; R: TRouteContext) => begin
    C.Log := Name;
  end;
end;

var
  app: TApp;
  req: TReq;
  rc: TRouteContext;
  i, wrong: Integer;

function Probe(const M, P: string): string;
begin
  req.Log := '';
  case app.Serve(M, P, req, rc) of
    rrMatched: Result := req.Log;
    rrNotFound: Result := '404';
    rrMethodNotAllowed: Result := '405 ' + rc.AllowedMethods;
  end;
end;

begin
  app := TApp.Create;
  req := TReq.Create;
  rc := TRouteContext.Create;
  for i := 0 to 199 do
  begin
    app.Get('/api/item' + IntToStr(i), Says('item' + IntToStr(i)));
    app.Post('/api/item' + IntToStr(i), Says('post' + IntToStr(i)));
    app.Get('/api/group' + IntToStr(i) + '/sub/leaf', Says('leaf' + IntToStr(i)));
  end;
  app.Get('/api/{name}', Says('param'));
  app.Get('/api/files/*', Says('files'));
  app.Get('/', Says('root'));
  wrong := 0;
  for i := 0 to 199 do
  begin
    if Probe('GET', '/api/item' + IntToStr(i)) <> 'item' + IntToStr(i) then Inc(wrong);
    if Probe('POST', '/api/item' + IntToStr(i)) <> 'post' + IntToStr(i) then Inc(wrong);
    if Probe('GET', '/api/group' + IntToStr(i) + '/sub/leaf') <> 'leaf' + IntToStr(i) then Inc(wrong);
  end;
  Writeln('wrong=', wrong);
  Writeln(Probe('GET', '/'));
  Writeln(Probe('GET', '/api/item200'));      { not a route: the parameter }
  Writeln(Probe('GET', '/api/item'));
  Writeln(Probe('GET', '/api/item1x'));
  Writeln(Probe('PUT', '/api/item7'));
  Writeln(Probe('GET', '/api/group7/sub'));
  Writeln(Probe('GET', '/api/group7/sub/leaf/more'));
  Writeln(Probe('GET', '/api/files/a/b/c'));
  Writeln(Probe('GET', '/nothing'));
  Writeln(Probe('GET', '/api/'));
end.
