program httpbench;
{ The HTTP layer without sockets: routing, the request parser on
  fragmented and chunked requests and deep pipelines, answers with many
  headers, query parameters. ns per call (best of several runs). Compare two
  builds of the units: run.sh (the current ones against those of another
  commit).

    httpbench [SECONDS] }
{$H+}
uses sysutils, nethttp, netrouter;

function c_clock: Int64; cdecl; external 'c' name 'clock';

type
  TOp = procedure;
  TReq = class
    N: Integer;
  end;
  TApp = specialize TRouter<TReq>;

var
  Budget: Double = 0.3;
  Sink: Int64;
  App: TApp;
  Req: TReq;
  RC: TRouteContext;
  Ctx: THttpContext;
  Frag, Chunked, Pipeline, QueryReq: string;

function Now: Double;
begin
  Result := c_clock / 1000000;
end;

procedure Measure(const Name: string; Op: TOp; Reps: Integer);
var
  Start, T, Best: Double;
  Runs, I: Integer;
begin
  Best := 1e9;
  Runs := 0;
  Start := Now;
  repeat
    T := Now;
    for I := 1 to Reps do
      Op();
    T := (Now - T) / Reps;
    if T < Best then
      Best := T;
    Inc(Runs);
  until (Now - Start > Budget) and (Runs >= 3);
  WriteLn(Format('  %-34s %12.1f ns', [Name, Best * 1e9]));
end;

{ 300 literal routes under one segment, and a parameter route }
procedure RouteLast; begin App.Serve('GET', '/api/route-with-a-longish-name-299', Req, RC); end;
procedure RouteMiddle; begin App.Serve('GET', '/api/route-with-a-longish-name-150', Req, RC); end;
procedure RouteParam; begin App.Serve('GET', '/users/42', Req, RC); end;
procedure RouteMissing; begin App.Serve('GET', '/api/nothing-here', Req, RC); end;

{ the request, delivered in pieces of Piece bytes: every piece is a recv, and
  the parser starts from the beginning each time }
procedure Fragmented(const S: string; Piece: Integer);
var
  Have, Used: Integer;
  R: THttpParse;
begin
  Have := Piece;
  repeat
    if Have > Length(S) then
      Have := Length(S);
    R := HttpParseRequest(PAnsiChar(S), Have, Ctx, Used);
    if R <> hpIncomplete then
      break;
    Have := Have + Piece;
  until false;
  Inc(Sink, Used);
end;

procedure FragHead; begin Fragmented(Frag, 64); end;
procedure FragBody; begin Fragmented(Chunked, 512); end;

{ 200 pipelined requests in one buffer, each parsed from where the last ended }
procedure Pipelined;
var
  Off, Used: Integer;
begin
  Off := 0;
  while Off < Length(Pipeline) do
  begin
    if HttpParseRequest(PAnsiChar(Pipeline) + Off, Length(Pipeline) - Off, Ctx, Used) <> hpOk then
      break;
    Off := Off + Used;
  end;
  Inc(Sink, Off);
end;

procedure Answer;
var
  C: THttpContext;
  I: Integer;
begin
  C := THttpContext.Create;
  for I := 1 to 12 do
    C.SetHeader('X-Header-' + IntToStr(I), 'value number ' + IntToStr(I));
  for I := 1 to 60 do
    C.Write('0123456789012345678901234567890123456789012345678901234567890123');
  Inc(Sink, Length(C.Response));
  C.Free;
end;

procedure Query;
var
  Used: Integer;
begin
  HttpParseRequest(PAnsiChar(QueryReq), Length(QueryReq), Ctx, Used);
  Inc(Sink, Length(Ctx.QueryParam('q')) + Length(Ctx.QueryParam('page')));
  if Ctx.HasHeader('x-request-id') then
    Inc(Sink, Length(Ctx.Header('Accept')));
end;

var
  I: Integer;
begin
  if ParamCount > 0 then
    Budget := StrToFloat(ParamStr(1));
  App := TApp.Create;
  Req := TReq.Create;
  RC := TRouteContext.Create;
  for I := 0 to 299 do
    App.Get('/api/route-with-a-longish-name-' + IntToStr(I), procedure(C: TReq; X: TRouteContext) => begin Inc(C.N); end);
  App.Get('/users/{id}', procedure(C: TReq; X: TRouteContext) => begin Inc(C.N); end);
  Ctx := THttpContext.Create;
  Frag := 'GET /api/v1/users/12345?fields=name,email HTTP/1.1'#13#10;
  for I := 1 to 12 do
    Frag := Frag + 'X-Header-' + IntToStr(I) + ': some value for header number ' + IntToStr(I) + #13#10;
  Frag := Frag + #13#10;
  Chunked := 'POST /upload HTTP/1.1'#13#10'Transfer-Encoding: chunked'#13#10#13#10;
  for I := 1 to 400 do
    Chunked := Chunked + '20'#13#10'abcdefghijklmnopqrstuvwxyz012345'#13#10;
  Chunked := Chunked + '0'#13#10#13#10;
  Pipeline := '';
  for I := 1 to 200 do
    Pipeline := Pipeline + 'GET /hello/' + IntToStr(I) + ' HTTP/1.1'#13#10'Host: h'#13#10#13#10;
  QueryReq := 'GET /search?alpha=1&beta=two&gamma=3&delta=four&epsilon=5&zeta=six&eta=7&theta=eight&page=%32&q=hello+world HTTP/1.1'#13#10 +
              'Host: h'#13#10'Accept: */*'#13#10'X-Request-Id: abc'#13#10#13#10;
  WriteLn('router (301 routes)');
  Measure('literal route, last of 300', @RouteLast, 20000);
  Measure('literal route, middle', @RouteMiddle, 20000);
  Measure('parameter route', @RouteParam, 20000);
  Measure('no route', @RouteMissing, 20000);
  WriteLn('parser');
  Measure('head in 64-byte pieces', @FragHead, 2000);
  Measure('chunked body (400 chunks), 512 B', @FragBody, 50);
  Measure('200 pipelined requests', @Pipelined, 200);
  WriteLn('answers');
  Measure('12 headers + 60 writes', @Answer, 1000);
  Measure('2 QueryParam + 2 header lookups', @Query, 5000);
  if Sink = 42 then
    WriteLn;
end.
