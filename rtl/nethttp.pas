(* nethttp - HTTP/1.1 for p3's servers: the layer between the network
   engines (netasync's tasks, netloop's event loops) and the routing
   (netrouter). It parses requests, gives handlers one context to read the
   request from and build the answer in, and serves a router or a handler
   on either engine. Needs --mode=objfpc.

     r := THttpRouter.Create;                     { netrouter, over THttpContext }
     r.Get('/hello/{name}', procedure(C: THttpContext; R: TRouteContext) =>
       C.Text(200, 'Hello, ' + R.URLParam('name')));
     r.Get('/static/*', FileServer('public'));

     srv := TNetLoopServer.Create;                { the loop engine }
     srv.Listen('127.0.0.1', 8080);
     HttpServeLoop(srv, RouterHandler(r));
     srv.Run;
   or
     Wait(HttpServeAsync(NetListenTo('127.0.0.1', 8080, 1024, opt), RouterHandler(r)));

   Requests. HttpParseRequest reads one request from the start of a buffer
   without copying it: the method, target, header names and values and the
   body are spans of the buffer (a chunked body is decoded into a string of
   its own). It takes HTTP/1.0 and 1.1, keep-alive (1.1's default,
   Connection: close / keep-alive), pipelining (the bytes after a request
   are the next one's), Content-Length and chunked bodies. It refuses what
   would let two servers disagree on where a request ends: both
   Content-Length and Transfer-Encoding, two different Content-Lengths, a
   Transfer-Encoding other than chunked, folded header lines. Limits
   (HttpLimits): the head (431), the body (413), the number of headers (431).

   The context. One per connection, reused for each request (with its
   TRouteContext, Route): its request accessors read the buffer, so they
   are valid while the handler runs (copy what is kept). Strings are made
   when asked: Path (percent-decoded), RawPath, Query, QueryParam, Header,
   Body. The answer: Status, ContentType, SetHeader / AddHeader, Write
   (appends to the body), and the shortcuts Send, Text, Html, Json,
   Redirect. HEAD answers carry no body (its length stays); 1xx, 204 and
   304 have none. A handler that answers nothing sends 200 with an empty
   body; Close makes the connection close after the answer.

   Serving. HttpServeLoop installs OnOpen / OnData / OnClose on a
   TNetLoopServer: every whole request received is answered on the loop's
   thread (a handler must not wait there). HttpServeAsync accepts on a
   listener and serves each connection as a task on the thread pool (a
   handler may wait: Wait(...) parks its connection only); idle connections
   close after HttpLimits.IdleTimeoutMs. Both answer a request that cannot
   be parsed with its error and close; After (optional) runs after each
   answer (counting, logging).

   Routers. THttpRouter is netrouter's TRouter over THttpContext; serving
   one answers 404 / 405 (Allow:) itself unless its NotFound /
   MethodNotAllowed handlers do. FileServer(root) is a handler for a
   catch-all route (/static/* -> root/...), ServeFile answers one file:
   content type by extension, a directory's index.html, 403 for '..'. *)
unit nethttp;

{$H+}

interface

{$IFNDEF P3_OBJFPC}
uses nethttp_needs_mode_objfpc;
{$ELSE}

uses sysutils, netasync, netloop, netrouter, threading;

type
  THttpLimits = record
    MaxHeadBytes: Integer;    { request line and headers (431) }
    MaxHeaders: Integer;      { (431) }
    MaxBodyBytes: Integer;    { (413) }
    IdleTimeoutMs: Integer;   { HttpServeAsync: a quiet keep-alive connection closes }
  end;

  THttpParse = (hpIncomplete, hpOk, hpBad, hpHeadTooLarge, hpBodyTooLarge, hpBadVersion);

  THttpSpan = record
    Off, Len: Integer;        { into the request's buffer }
  end;

  { A request and its answer. Its request part reads the buffer the request
    was parsed from: valid while the handler runs. }
  THttpContext = class
  private
    FBuf: PAnsiChar;
    FMethod, FTarget, FVersion, FBodySpan: THttpSpan;
    FNames, FValues: array of THttpSpan;
    FNHeaders: Integer;
    FChunkedBody: string;
    FChunked: Boolean;
    FKeepAlive: Boolean;
    FPath: string;
    FPathMade: Boolean;
    { the answer }
    FStatus: Integer;
    FContentType: string;
    FHeaders: string;          { extra header lines, each ending in CRLF }
    FBody: string;
    FAnswered: Boolean;
    FClose: Boolean;
    function Span(const S: THttpSpan): string;
    procedure ResetAnswer;
    procedure SetStatus(Code: Integer);
    procedure SetContentType(const Value: string);
  public
    Route: TRouteContext;      { netrouter's, reused }
    Data: TObject;             { the program's, per connection }
    constructor Create;
    destructor Destroy; override;
    { ---- the request ---- }
    function Method: string;
    { the path, percent-decoded, without the query }
    function Path: string;
    function RawPath: string;
    function Query: string;
    { a query parameter, decoded ('' if absent) }
    function QueryParam(const Name: string): string;
    function Target: string;
    function Version: string;
    { a header's value ('' if absent), its name case-insensitive }
    function Header(const Name: string): string;
    function HasHeader(const Name: string): Boolean;
    function HeaderCount: Integer;
    function HeaderName(I: Integer): string;
    function HeaderValue(I: Integer): string;
    function Body: string;
    function KeepAlive: Boolean;
    { ---- the answer ---- }
    procedure SetHeader(const Name, Value: string);   { replaces }
    procedure AddHeader(const Name, Value: string);
    procedure Write(const S: string);                 { appends to the body }
    procedure Send(Code: Integer; const CType, Content: string);
    procedure Text(Code: Integer; const Content: string);
    procedure Html(Code: Integer; const Content: string);
    procedure Json(Code: Integer; const Content: string);
    procedure Redirect(Code: Integer; const Location: string);
    { close the connection after this answer }
    procedure Close;
    { the whole answer: status line, headers, body (none for HEAD) }
    function Response: string;
    property Status: Integer read FStatus write SetStatus;
    property ContentType: string read FContentType write SetContentType;
    property Answered: Boolean read FAnswered;
    property ResponseBody: string read FBody;
    property Closing: Boolean read FClose;
  end;

  THttpHandler = reference to procedure(C: THttpContext);
  THttpRouter = specialize TRouter<THttpContext>;

var
  HttpLimits: THttpLimits;

{ One request from P[0..Len): hpOk (Used: its bytes; C holds it), or
  hpIncomplete (more bytes needed), or an error (HttpErrorStatus). }
function HttpParseRequest(P: PAnsiChar; Len: Integer; C: THttpContext;
                          out Used: Integer): THttpParse;
function HttpErrorStatus(R: THttpParse): Integer;
function HttpStatusText(Code: Integer): string;
{ %XX decoded (and '+' as a space when Plus) }
function HttpUrlDecode(const S: string; Plus: Boolean = false): string;
function HttpHtmlEscape(const S: string): string;
function HttpContentType(const FileName: string): string;

{ a router as a handler: 404 / 405 (with Allow:) when the router's own
  handlers did not answer }
function RouterHandler(R: THttpRouter): THttpHandler;

{ Serve on a netloop server (installs its OnOpen / OnData / OnClose; then
  Run it). After runs after each answer, on the loop's thread. A router:
  HttpServeLoop(srv, RouterHandler(r)). }
procedure HttpServeLoop(Server: TNetLoopServer; const Handler: THttpHandler;
                        const After: THttpHandler = nil);

{ Serve the connections of Listener, each as a task on the pool; does not
  return (run it with Wait, or spawn it). After runs after each answer has
  been sent, in the connection's task. }
function HttpServeAsync(Listener: TSocket; Handler: THttpHandler;
                        After: THttpHandler = nil): async;
{ (a router: HttpServeAsync(listener, RouterHandler(r))) }

{ the file Name: content type by extension; a directory's index.html; 404 }
procedure ServeFile(C: THttpContext; const Name: string);
{ a handler for a catch-all route: serves Root + '/' + the route's '*'
  (or the request path when the route has none); '..' is a 403 }
function FileServer(Root: string): THttpRouter.THandler;
{ is a path free of '..' segments (and NULs)? }
function HttpSafePath(const P: string): Boolean;

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}

const
  CRLF = #13#10;

{ ---- small helpers ---- }

function LowerAscii(c: AnsiChar): AnsiChar; inline;
begin
  if (c >= 'A') and (c <= 'Z') then
    Result := AnsiChar(Ord(c) + 32)
  else
    Result := c;
end;

{ P[0..Len) equals S, case-insensitive (S lowercase) }
function SameNoCase(P: PAnsiChar; Len: Integer; const S: string): Boolean;
var
  i: Integer;
begin
  if Len <> Length(S) then
    exit(false);
  for i := 0 to Len - 1 do
    if LowerAscii((P + i)^) <> S[i + 1] then
      exit(false);
  Result := true;
end;

function HexDigit(c: AnsiChar): Integer;
begin
  case c of
    '0'..'9': Result := Ord(c) - Ord('0');
    'a'..'f': Result := Ord(c) - Ord('a') + 10;
    'A'..'F': Result := Ord(c) - Ord('A') + 10;
  else
    Result := -1;
  end;
end;

function HttpUrlDecode(const S: string; Plus: Boolean): string;
var
  i, hi, lo, n: Integer;
  r: string;
begin
  if (Pos('%', S) = 0) and (not Plus or (Pos('+', S) = 0)) then
    exit(S);
  SetLength(r, Length(S));
  n := 0;
  i := 1;
  while i <= Length(S) do
  begin
    if (S[i] = '%') and (i + 2 <= Length(S)) then
    begin
      hi := HexDigit(S[i + 1]);
      lo := HexDigit(S[i + 2]);
      if (hi >= 0) and (lo >= 0) then
      begin
        inc(n);
        r[n] := AnsiChar(hi * 16 + lo);
        i := i + 3;
        continue;
      end;
    end;
    inc(n);
    if Plus and (S[i] = '+') then
      r[n] := ' '
    else
      r[n] := S[i];
    inc(i);
  end;
  SetLength(r, n);
  Result := r;
end;

function HttpHtmlEscape(const S: string): string;
var
  i: Integer;
  r: string;
begin
  r := '';
  for i := 1 to Length(S) do
    case S[i] of
      '&': r := r + '&amp;';
      '<': r := r + '&lt;';
      '>': r := r + '&gt;';
      '"': r := r + '&quot;';
      '''': r := r + '&#39;';
    else
      r := r + S[i];
    end;
  Result := r;
end;

function HttpStatusText(Code: Integer): string;
begin
  case Code of
    100: Result := 'Continue';
    101: Result := 'Switching Protocols';
    200: Result := 'OK';
    201: Result := 'Created';
    202: Result := 'Accepted';
    204: Result := 'No Content';
    206: Result := 'Partial Content';
    301: Result := 'Moved Permanently';
    302: Result := 'Found';
    303: Result := 'See Other';
    304: Result := 'Not Modified';
    307: Result := 'Temporary Redirect';
    308: Result := 'Permanent Redirect';
    400: Result := 'Bad Request';
    401: Result := 'Unauthorized';
    403: Result := 'Forbidden';
    404: Result := 'Not Found';
    405: Result := 'Method Not Allowed';
    408: Result := 'Request Timeout';
    409: Result := 'Conflict';
    410: Result := 'Gone';
    411: Result := 'Length Required';
    413: Result := 'Content Too Large';
    414: Result := 'URI Too Long';
    415: Result := 'Unsupported Media Type';
    422: Result := 'Unprocessable Content';
    429: Result := 'Too Many Requests';
    431: Result := 'Request Header Fields Too Large';
    500: Result := 'Internal Server Error';
    501: Result := 'Not Implemented';
    502: Result := 'Bad Gateway';
    503: Result := 'Service Unavailable';
    504: Result := 'Gateway Timeout';
    505: Result := 'HTTP Version Not Supported';
  else
    Result := 'Status ' + IntToStr(Code);
  end;
end;

function HttpErrorStatus(R: THttpParse): Integer;
begin
  case R of
    hpHeadTooLarge: Result := 431;
    hpBodyTooLarge: Result := 413;
    hpBadVersion: Result := 505;
  else
    Result := 400;
  end;
end;

function HttpContentType(const FileName: string): string;
var
  ext: string;
begin
  ext := LowerCase(ExtractFileExt(FileName));
  if (ext = '.html') or (ext = '.htm') then Result := 'text/html; charset=utf-8'
  else if ext = '.css' then Result := 'text/css; charset=utf-8'
  else if (ext = '.js') or (ext = '.mjs') then Result := 'text/javascript; charset=utf-8'
  else if ext = '.json' then Result := 'application/json'
  else if (ext = '.txt') or (ext = '.md') or (ext = '.pas') or (ext = '.csv') then
    Result := 'text/plain; charset=utf-8'
  else if ext = '.xml' then Result := 'application/xml'
  else if ext = '.svg' then Result := 'image/svg+xml'
  else if ext = '.png' then Result := 'image/png'
  else if (ext = '.jpg') or (ext = '.jpeg') then Result := 'image/jpeg'
  else if ext = '.gif' then Result := 'image/gif'
  else if ext = '.webp' then Result := 'image/webp'
  else if ext = '.ico' then Result := 'image/x-icon'
  else if ext = '.wasm' then Result := 'application/wasm'
  else if ext = '.pdf' then Result := 'application/pdf'
  else if ext = '.woff2' then Result := 'font/woff2'
  else Result := 'application/octet-stream';
end;

function HttpSafePath(const P: string): Boolean;
var
  i, start: Integer;
begin
  Result := false;
  if Pos(#0, P) > 0 then
    exit;
  start := 1;
  for i := 1 to Length(P) + 1 do
    if (i > Length(P)) or (P[i] = '/') or (P[i] = '\') then
    begin
      if Copy(P, start, i - start) = '..' then
        exit;
      start := i + 1;
    end;
  Result := true;
end;

{ ---- the parser ---- }

{ is the token Tok (lowercase) among the comma-separated values of P[0..Len)? }
function HasToken(P: PAnsiChar; Len: Integer; const Tok: string): Boolean;
var
  i, s, e: Integer;
begin
  i := 0;
  while i < Len do
  begin
    while (i < Len) and ((P + i)^ in [' ', #9, ',']) do
      inc(i);
    s := i;
    while (i < Len) and ((P + i)^ <> ',') do
      inc(i);
    e := i;
    while (e > s) and ((P + e - 1)^ in [' ', #9]) do
      dec(e);
    if SameNoCase(P + s, e - s, Tok) then
      exit(true);
  end;
  Result := false;
end;

{ a chunked body from P[0..Len): hpOk with Used and the decoded Body }
function ParseChunked(P: PAnsiChar; Len: Integer; out Body: string;
                      out Used: Integer): THttpParse;
var
  i, n, d, size, total: Integer;
  b: string;
begin
  Used := 0;
  Body := '';
  b := '';
  total := 0;
  i := 0;
  while true do
  begin
    { size in hex [; extensions] CRLF }
    size := 0;
    n := 0;
    while i < Len do
    begin
      d := HexDigit((P + i)^);
      if d < 0 then
        break;
      if size > HttpLimits.MaxBodyBytes then
        exit(hpBodyTooLarge);
      size := size * 16 + d;
      inc(n);
      inc(i);
    end;
    if i >= Len then
      exit(hpIncomplete);
    if n = 0 then
      exit(hpBad);
    while (i < Len) and ((P + i)^ <> #13) do   { (extensions: ignored) }
    begin
      if (P + i)^ = #10 then
        exit(hpBad);
      inc(i);
    end;
    if i + 1 >= Len then
      exit(hpIncomplete);
    if (P + i + 1)^ <> #10 then
      exit(hpBad);
    i := i + 2;
    if size = 0 then
      break;
    total := total + size;
    if total > HttpLimits.MaxBodyBytes then
      exit(hpBodyTooLarge);
    if i + size + 2 > Len then
      exit(hpIncomplete);
    n := Length(b);
    SetLength(b, n + size);
    Move((P + i)^, b[n + 1], size);
    i := i + size;
    if ((P + i)^ <> #13) or ((P + i + 1)^ <> #10) then
      exit(hpBad);
    i := i + 2;
  end;
  { trailers (ignored) up to an empty line }
  while true do
  begin
    if i + 1 >= Len then
      exit(hpIncomplete);
    if ((P + i)^ = #13) and ((P + i + 1)^ = #10) then
    begin
      i := i + 2;
      break;
    end;
    while (i + 1 < Len) and not (((P + i)^ = #13) and ((P + i + 1)^ = #10)) do
      inc(i);
    if i + 1 >= Len then
      exit(hpIncomplete);
    i := i + 2;
  end;
  Body := b;
  Used := i;
  Result := hpOk;
end;

function HttpParseRequest(P: PAnsiChar; Len: Integer; C: THttpContext;
                          out Used: Integer): THttpParse;
var
  headEnd, i, lineEnd, ls, colon, vs, ve, sp1, sp2: Integer;
  cl, v: Int64;
  hasCL, chunked, hasTE, http11: Boolean;
  connClose, connKeep: Boolean;
  bodyUsed: Integer;
  chunkBody: string;
begin
  Used := 0;
  { the head: up to the empty line }
  headEnd := -1;
  i := 0;
  while i + 3 < Len do
  begin
    if ((P + i)^ = #13) and ((P + i + 1)^ = #10) and ((P + i + 2)^ = #13) and
       ((P + i + 3)^ = #10) then
    begin
      headEnd := i;
      break;
    end;
    if i >= HttpLimits.MaxHeadBytes then
      exit(hpHeadTooLarge);
    inc(i);
  end;
  if headEnd < 0 then
  begin
    if Len > HttpLimits.MaxHeadBytes then
      exit(hpHeadTooLarge);
    exit(hpIncomplete);
  end;
  C.FBuf := P;
  C.FNHeaders := 0;
  C.FChunked := false;
  C.FChunkedBody := '';
  C.FPathMade := false;
  C.FPath := '';
  { the request line: METHOD SP target SP HTTP/1.x }
  lineEnd := 0;
  while not (((P + lineEnd)^ = #13) and ((P + lineEnd + 1)^ = #10)) do
    inc(lineEnd);
  sp1 := 0;
  while (sp1 < lineEnd) and ((P + sp1)^ <> ' ') do
  begin
    if not ((P + sp1)^ in ['A'..'Z', 'a'..'z', '0'..'9', '!', '#', '$', '%', '&', '''',
                           '*', '+', '-', '.', '^', '_', '`', '|', '~']) then
      exit(hpBad);
    inc(sp1);
  end;
  if (sp1 = 0) or (sp1 >= lineEnd) then
    exit(hpBad);
  sp2 := sp1 + 1;
  while (sp2 < lineEnd) and ((P + sp2)^ <> ' ') do
  begin
    if Ord((P + sp2)^) <= 32 then
      exit(hpBad);
    inc(sp2);
  end;
  if (sp2 = sp1 + 1) or (sp2 >= lineEnd) then
    exit(hpBad);
  C.FMethod.Off := 0;
  C.FMethod.Len := sp1;
  C.FTarget.Off := sp1 + 1;
  C.FTarget.Len := sp2 - sp1 - 1;
  C.FVersion.Off := sp2 + 1;
  C.FVersion.Len := lineEnd - sp2 - 1;
  if not (((P + C.FTarget.Off)^ = '/') or
          ((C.FTarget.Len = 1) and ((P + C.FTarget.Off)^ = '*'))) then
    exit(hpBad);
  if (C.FVersion.Len <> 8) or not SameNoCase(P + C.FVersion.Off, 5, 'http/') or
     ((P + C.FVersion.Off + 6)^ <> '.') then
    exit(hpBad);
  if ((P + C.FVersion.Off + 5)^ <> '1') or not ((P + C.FVersion.Off + 7)^ in ['0', '1']) then
    exit(hpBadVersion);
  http11 := (P + C.FVersion.Off + 7)^ = '1';
  { the headers }
  hasCL := false;
  hasTE := false;
  chunked := false;
  connClose := false;
  connKeep := false;
  cl := 0;
  ls := lineEnd + 2;
  while ls < headEnd + 2 do
  begin
    i := ls;
    while not (((P + i)^ = #13) and ((P + i + 1)^ = #10)) do
      inc(i);
    lineEnd := i;
    if (P + ls)^ in [' ', #9] then
      exit(hpBad);              { a folded line }
    colon := ls;
    while (colon < lineEnd) and ((P + colon)^ <> ':') do
    begin
      if (P + colon)^ in [' ', #9] then
        exit(hpBad);            { a space in a name }
      inc(colon);
    end;
    if (colon = ls) or (colon >= lineEnd) then
      exit(hpBad);
    vs := colon + 1;
    while (vs < lineEnd) and ((P + vs)^ in [' ', #9]) do
      inc(vs);
    ve := lineEnd;
    while (ve > vs) and ((P + ve - 1)^ in [' ', #9]) do
      dec(ve);
    if C.FNHeaders >= HttpLimits.MaxHeaders then
      exit(hpHeadTooLarge);
    if C.FNHeaders >= Length(C.FNames) then
    begin
      SetLength(C.FNames, C.FNHeaders + 16);
      SetLength(C.FValues, C.FNHeaders + 16);
    end;
    C.FNames[C.FNHeaders].Off := ls;
    C.FNames[C.FNHeaders].Len := colon - ls;
    C.FValues[C.FNHeaders].Off := vs;
    C.FValues[C.FNHeaders].Len := ve - vs;
    inc(C.FNHeaders);
    if SameNoCase(P + ls, colon - ls, 'content-length') then
    begin
      if ve = vs then
        exit(hpBad);
      v := 0;
      for i := vs to ve - 1 do
      begin
        if not ((P + i)^ in ['0'..'9']) then
          exit(hpBad);
        v := v * 10 + (Ord((P + i)^) - Ord('0'));
        if v > HttpLimits.MaxBodyBytes then
          exit(hpBodyTooLarge);
      end;
      if hasCL and (v <> cl) then
        exit(hpBad);            { two different lengths }
      hasCL := true;
      cl := v;
    end
    else if SameNoCase(P + ls, colon - ls, 'transfer-encoding') then
    begin
      hasTE := true;
      if not SameNoCase(P + vs, ve - vs, 'chunked') then
        exit(hpBad);            { (only chunked, alone) }
      chunked := true;
    end
    else if SameNoCase(P + ls, colon - ls, 'connection') then
    begin
      if HasToken(P + vs, ve - vs, 'close') then connClose := true;
      if HasToken(P + vs, ve - vs, 'keep-alive') then connKeep := true;
    end;
    ls := lineEnd + 2;
  end;
  if hasCL and hasTE then
    exit(hpBad);                { which one ends the request? refuse }
  if http11 then
    C.FKeepAlive := not connClose
  else
    C.FKeepAlive := connKeep and not connClose;
  { the body }
  C.FBodySpan.Off := headEnd + 4;
  if chunked then
  begin
    Result := ParseChunked(P + headEnd + 4, Len - headEnd - 4, chunkBody, bodyUsed);
    if Result <> hpOk then
      exit;
    C.FChunked := true;
    C.FChunkedBody := chunkBody;
    C.FBodySpan.Len := 0;
    Used := headEnd + 4 + bodyUsed;
    C.ResetAnswer;   { a new request: a new answer }
    exit(hpOk);
  end;
  if Len - headEnd - 4 < cl then
    exit(hpIncomplete);
  C.FBodySpan.Len := Integer(cl);
  Used := headEnd + 4 + Integer(cl);
  C.ResetAnswer;     { a new request: a new answer }
  Result := hpOk;
end;

{ ---- THttpContext ---- }

constructor THttpContext.Create;
begin
  inherited Create;
  Route := TRouteContext.Create;
  ResetAnswer;
end;

destructor THttpContext.Destroy;
begin
  Route.Free;
  inherited Destroy;
end;

procedure THttpContext.ResetAnswer;
begin
  FStatus := 200;
  FContentType := '';
  FHeaders := '';
  FBody := '';
  FAnswered := false;
  FClose := false;
end;

function THttpContext.Span(const S: THttpSpan): string;
var
  r: string;
begin
  SetLength(r, S.Len);
  if S.Len > 0 then
    Move((FBuf + S.Off)^, r[1], S.Len);
  Result := r;
end;

function THttpContext.Method: string;
var
  p: PAnsiChar;
begin
  { the usual ones without an allocation }
  p := FBuf;
  case FMethod.Len of
    3: if (p^ = 'G') and ((p + 1)^ = 'E') and ((p + 2)^ = 'T') then exit('GET')
       else if (p^ = 'P') and ((p + 1)^ = 'U') and ((p + 2)^ = 'T') then exit('PUT');
    4: if (p^ = 'P') and ((p + 1)^ = 'O') and ((p + 2)^ = 'S') and ((p + 3)^ = 'T') then exit('POST')
       else if (p^ = 'H') and ((p + 1)^ = 'E') and ((p + 2)^ = 'A') and ((p + 3)^ = 'D') then exit('HEAD');
  end;
  Result := Span(FMethod);
end;

function THttpContext.RawPath: string;
var
  s: THttpSpan;
  i: Integer;
begin
  s := FTarget;
  for i := 0 to FTarget.Len - 1 do
    if (FBuf + FTarget.Off + i)^ = '?' then
    begin
      s.Len := i;
      break;
    end;
  Result := Span(s);
end;

function THttpContext.Path: string;
begin
  if not FPathMade then
  begin
    FPath := HttpUrlDecode(RawPath);
    FPathMade := true;
  end;
  Result := FPath;
end;

function THttpContext.Query: string;
var
  i: Integer;
  s: THttpSpan;
begin
  Result := '';
  for i := 0 to FTarget.Len - 1 do
    if (FBuf + FTarget.Off + i)^ = '?' then
    begin
      s.Off := FTarget.Off + i + 1;
      s.Len := FTarget.Len - i - 1;
      exit(Span(s));
    end;
end;

function THttpContext.QueryParam(const Name: string): string;
var
  q, pair, k: string;
  i, e: Integer;
begin
  Result := '';
  q := Query;
  while q <> '' do
  begin
    i := Pos('&', q);
    if i = 0 then
    begin
      pair := q;
      q := '';
    end
    else
    begin
      pair := Copy(q, 1, i - 1);
      q := Copy(q, i + 1, Length(q));
    end;
    e := Pos('=', pair);
    if e = 0 then
      k := pair
    else
      k := Copy(pair, 1, e - 1);
    if HttpUrlDecode(k, true) = Name then
    begin
      if e = 0 then
        exit('');
      exit(HttpUrlDecode(Copy(pair, e + 1, Length(pair)), true));
    end;
  end;
end;

function THttpContext.Target: string;
begin
  Result := Span(FTarget);
end;

function THttpContext.Version: string;
begin
  Result := Span(FVersion);
end;

function THttpContext.HasHeader(const Name: string): Boolean;
var
  i: Integer;
  low: string;
begin
  low := LowerCase(Name);
  for i := 0 to FNHeaders - 1 do
    if SameNoCase(FBuf + FNames[i].Off, FNames[i].Len, low) then
      exit(true);
  Result := false;
end;

function THttpContext.Header(const Name: string): string;
var
  i: Integer;
  low: string;
begin
  low := LowerCase(Name);
  for i := 0 to FNHeaders - 1 do
    if SameNoCase(FBuf + FNames[i].Off, FNames[i].Len, low) then
      exit(Span(FValues[i]));
  Result := '';
end;

function THttpContext.HeaderCount: Integer;
begin
  Result := FNHeaders;
end;

function THttpContext.HeaderName(I: Integer): string;
begin
  Result := Span(FNames[I]);
end;

function THttpContext.HeaderValue(I: Integer): string;
begin
  Result := Span(FValues[I]);
end;

function THttpContext.Body: string;
begin
  if FChunked then
    Result := FChunkedBody
  else
    Result := Span(FBodySpan);
end;

function THttpContext.KeepAlive: Boolean;
begin
  Result := FKeepAlive and not FClose;
end;

procedure THttpContext.SetStatus(Code: Integer);
begin
  FStatus := Code;
  FAnswered := true;
end;

procedure THttpContext.SetContentType(const Value: string);
begin
  FContentType := Value;
  FAnswered := true;
end;

procedure THttpContext.SetHeader(const Name, Value: string);
var
  low, line: string;
  p, e: Integer;
begin
  { drop a line of the same name, then add }
  low := LowerCase(Name) + ':';
  p := Pos(CRLF + low, CRLF + LowerCase(FHeaders));
  if p > 0 then
  begin
    e := p;
    while (e <= Length(FHeaders)) and (FHeaders[e] <> #13) do
      inc(e);
    Delete(FHeaders, p, e - p + 2);
  end;
  line := Name + ': ' + Value + CRLF;
  FHeaders := FHeaders + line;
  FAnswered := true;
end;

procedure THttpContext.AddHeader(const Name, Value: string);
begin
  FHeaders := FHeaders + Name + ': ' + Value + CRLF;
  FAnswered := true;
end;

procedure THttpContext.Write(const S: string);
begin
  FBody := FBody + S;
  FAnswered := true;
end;

procedure THttpContext.Send(Code: Integer; const CType, Content: string);
begin
  FStatus := Code;
  FContentType := CType;
  FBody := Content;
  FAnswered := true;
end;

procedure THttpContext.Text(Code: Integer; const Content: string);
begin
  Send(Code, 'text/plain; charset=utf-8', Content);
end;

procedure THttpContext.Html(Code: Integer; const Content: string);
begin
  Send(Code, 'text/html; charset=utf-8', Content);
end;

procedure THttpContext.Json(Code: Integer; const Content: string);
begin
  Send(Code, 'application/json', Content);
end;

procedure THttpContext.Redirect(Code: Integer; const Location: string);
begin
  SetHeader('Location', Location);
  Send(Code, 'text/html; charset=utf-8',
       '<a href="' + HttpHtmlEscape(Location) + '">' + HttpStatusText(Code) + '</a>.' + #10);
end;

procedure THttpContext.Close;
begin
  FClose := true;
end;

function THttpContext.Response: string;
var
  r, conn: string;
  noBody: Boolean;
begin
  if KeepAlive then
    conn := 'keep-alive'
  else
    conn := 'close';
  noBody := (FStatus < 200) or (FStatus = 204) or (FStatus = 304);
  r := 'HTTP/1.1 ' + IntToStr(FStatus) + ' ' + HttpStatusText(FStatus) + CRLF;
  if (FContentType <> '') and not noBody then
    r := r + 'Content-Type: ' + FContentType + CRLF;
  if not noBody then
    r := r + 'Content-Length: ' + IntToStr(Length(FBody)) + CRLF;
  r := r + 'Connection: ' + conn + CRLF + FHeaders + CRLF;
  if not noBody and (Method <> 'HEAD') then
    r := r + FBody;
  Result := r;
end;

{ ---- routers ---- }

function RouterHandler(R: THttpRouter): THttpHandler;
begin
  Result := procedure(C: THttpContext) => begin
    case R.Serve(C.Method, C.Path, C, C.Route) of
      rrNotFound:
        if not C.Answered then
          C.Text(404, '404 page not found' + #10);
      rrMethodNotAllowed:
        if not C.Answered then
        begin
          C.SetHeader('Allow', C.Route.AllowedMethods);
          C.Text(405, '405 method not allowed' + #10);
        end;
    end;
  end;
end;

{ ---- files ---- }

function c_fopen(name, mode: PChar): Pointer; cdecl; external 'c' name 'fopen';
function c_fread(buf: Pointer; size, count: Int64; f: Pointer): Int64; cdecl; external 'c' name 'fread';
function c_fclose(f: Pointer): LongInt; cdecl; external 'c' name 'fclose';

function ReadWholeFile(const Name: string; out Content: string): Boolean;
var
  f: Pointer;
  buf: array[0..65535] of AnsiChar;
  n, used: Int64;
  r: string;
begin
  Result := false;
  Content := '';
  f := c_fopen(PChar(Name), 'rb');
  if f = nil then
    exit;
  r := '';
  used := 0;
  repeat
    n := c_fread(@buf[0], 1, sizeof(buf), f);
    if n > 0 then
    begin
      SetLength(r, used + n);
      Move(buf[0], r[used + 1], n);
      used := used + n;
    end;
  until n < sizeof(buf);
  c_fclose(f);
  Content := r;
  Result := true;
end;

procedure ServeFile(C: THttpContext; const Name: string);
var
  fn, content: string;
begin
  fn := Name;
  if DirectoryExists(fn) then
  begin
    if (fn = '') or (fn[Length(fn)] <> '/') then
      fn := fn + '/';
    fn := fn + 'index.html';
  end;
  if not FileExists(fn) then
  begin
    C.Text(404, '404 page not found' + #10);
    exit;
  end;
  if not ReadWholeFile(fn, content) then
  begin
    C.Text(500, 'the file could not be read' + #10);
    exit;
  end;
  C.Send(200, HttpContentType(fn), content);
end;

function FileServer(Root: string): THttpRouter.THandler;
var
  base: string;
begin
  base := Root;
  if (base <> '') and (base[Length(base)] = '/') then
    SetLength(base, Length(base) - 1);
  Result := procedure(C: THttpContext; RC: TRouteContext) => begin
    var rest: string;
    if not RC.HasParam('*', rest) then
      rest := C.Path
    else
      rest := '/' + HttpUrlDecode(rest);
    if not HttpSafePath(rest) then
    begin
      C.Text(403, '403 forbidden' + #10);
      exit;
    end;
    ServeFile(C, base + rest);
  end;
end;

{ ---- serving on netloop ---- }

procedure HttpServeLoop(Server: TNetLoopServer; const Handler: THttpHandler;
                        const After: THttpHandler = nil);
var
  h, a: THttpHandler;
begin
  h := Handler;
  a := After;
  Server.OnOpen := procedure(C: TLoopConn) => C.Obj := THttpContext.Create;
  Server.OnClose := procedure(C: TLoopConn) => C.Obj.Free;
  Server.OnData := procedure(C: TLoopConn) => begin
    var ctx := THttpContext(C.Obj);
    var off := 0;
    var used: Integer;
    while not C.Closing do
    begin
      var res := HttpParseRequest(C.InPtr + off, C.InCount - off, ctx, used);
      if res = hpIncomplete then
        break;
      if res <> hpOk then
      begin
        ctx.ResetAnswer;
        ctx.FKeepAlive := false;
        ctx.FMethod.Len := 0;
        ctx.Text(HttpErrorStatus(res), HttpStatusText(HttpErrorStatus(res)) + #10);
        ctx.Close;
        C.Write(ctx.Response);
        C.Close;
        if Assigned(a) then a(ctx);
        off := C.InCount;
        break;
      end;
      ctx.ResetAnswer;
      h(ctx);
      C.Write(ctx.Response);
      if not ctx.KeepAlive then
        C.Close;
      off := off + used;
      if Assigned(a) then a(ctx);
    end;
    C.Consume(off);
  end;
end;

{ ---- serving on netasync ---- }

function HttpServeConn(Sock: TSocket; Handler: THttpHandler; After: THttpHandler): async;
var
  ctx: THttpContext;
  buf: PAnsiChar;
  cap, len, used, n, bits: Integer;
  res: THttpParse;
  resp: string;
begin
  NetSetNoDelay(Sock);
  ctx := THttpContext.Create;
  cap := 16384;
  GetMem(buf, cap);
  len := 0;
  try
    while true do
    begin
      if len > 0 then
        res := HttpParseRequest(buf, len, ctx, used)
      else
        res := hpIncomplete;
      if res = hpIncomplete then
      begin
        if len = cap then
        begin
          cap := cap * 2;
          ReallocMem(buf, cap);
        end;
        if (len = 0) and (HttpLimits.IdleTimeoutMs > 0) then
        begin
          bits := NetWaitFdUntil(Sock, true, false, HttpLimits.IdleTimeoutMs);
          if (bits and NW_TIMEOUT) <> 0 then
            exit;             { quiet for too long }
        end;
        n := await NetReadInto(Sock, buf + len, cap - len);
        if n <= 0 then
          exit;               { the client is gone }
        len := len + n;
        continue;
      end;
      if res <> hpOk then
      begin
        ctx.ResetAnswer;
        ctx.FKeepAlive := false;
        ctx.FMethod.Len := 0;
        ctx.Text(HttpErrorStatus(res), HttpStatusText(HttpErrorStatus(res)) + #10);
        ctx.Close;
        resp := ctx.Response;
        await NetSendPtr(Sock, PChar(resp), Length(resp));
        if Assigned(After) then After(ctx);
        exit;
      end;
      ctx.ResetAnswer;
      Handler(ctx);
      resp := ctx.Response;
      await NetSendPtr(Sock, PChar(resp), Length(resp));
      if Assigned(After) then After(ctx);
      if not ctx.KeepAlive then
        exit;
      { the next request: what followed this one }
      len := len - used;
      if len > 0 then
        Move((buf + used)^, buf^, len);
    end;
  finally
    FreeMem(buf);
    ctx.Free;
    NetClose(Sock);
  end;
end;

function HttpLaunch(Sock: TSocket; Handler: THttpHandler; After: THttpHandler): async;
begin
  { on the pool: a task started any other way stays on this thread }
  await Pool.Run(HttpServeConn(Sock, Handler, After));
end;

procedure HttpDropped(E: Exception);
begin
  { a connection that broke: nothing to answer }
end;

function HttpServeAsync(Listener: TSocket; Handler: THttpHandler; After: THttpHandler): async;
begin
  while true do
    detached(HttpDropped) HttpLaunch(await NetAccept(Listener), Handler, After);
end;

initialization
  HttpLimits.MaxHeadBytes := 16384;
  HttpLimits.MaxHeaders := 100;
  HttpLimits.MaxBodyBytes := 8 * 1024 * 1024;
  HttpLimits.IdleTimeoutMs := 75000;

{$ENDIF}

end.
