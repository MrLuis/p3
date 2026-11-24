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
   listener and serves each connection as a task, on the thread pool or on
   the accepting thread (a handler may wait: Wait(...) parks its connection
   only); a connection closes after HttpLimits.IdleTimeoutMs without a
   request, or RequestTimeoutMs on one request or one answer. Both answer a
   request that cannot be parsed with its error (HttpOnBadRequest, if set,
   writes it: C.Status holds the code) and close; After (optional) runs
   after each answer (counting, logging).

   Answering later (loop engine). A handler that starts work which ends in
   another callback of its loop (a query on a database socket the loop
   watches, a timer) calls C.Defer and returns; the request is then done
   being read (copy what is needed first: the request accessors are not
   valid after Defer), and the connection's next requests wait. Whoever
   finishes fills the answer in and calls C.Complete, on the same loop
   thread: the answer goes out and the connection goes on. If the client
   left meanwhile, Complete frees the context instead (do not touch it
   after Complete). C.Conn is the context's connection (nil on the async
   engine), C.Conn.Loop its loop.

   Memory. Between requests an idle keep-alive connection keeps no buffer
   and a small context (Shrink); on the loop engine none at all, unless the
   program keeps per-connection C.Data on it.

   Routers. THttpRouter is netrouter's TRouter over THttpContext; serving
   one answers 404 / 405 (Allow:) itself unless its NotFound /
   MethodNotAllowed handlers do. C.Data is the program's, per connection,
   and is freed with the context. FileServer(root) is a handler for a
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
    RequestTimeoutMs: Integer;{ HttpServeAsync: to receive one request / send one answer }
  end;

  THttpParse = (hpIncomplete, hpOk, hpBad, hpHeadTooLarge, hpBodyTooLarge, hpBadVersion);

  THttpSpan = record
    Off, Len: Integer;        { into the request's buffer }
  end;

  THttpContext = class;
  THttpHandler = reference to procedure(C: THttpContext);

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
    FIsHead: Boolean;
    FPath: string;
    FPathMade: Boolean;
    { the answer }
    FStatus: Integer;
    FContentType: string;
    { extra header lines, each ending in CRLF (a buffer grown by doubling,
      so that adding a header does not copy the others) }
    FHdr: PAnsiChar;
    FHdrLen, FHdrCap: Integer;
    { the body: the string Send was given, or (once Write was called) the
      buffer FBodyBuf, grown by doubling }
    FBody: string;
    FBodyBuf: PAnsiChar;
    FBodyLen, FBodyCap: Integer;
    FBuffered: Boolean;
    FHeadLength: Int64;        { a HEAD answer's Content-Length when its body is not made (-1: its own) }
    FAnswered: Boolean;
    FClose: Boolean;
    FRoute: TRouteContext;
    { the loop engine }
    FConn: TLoopConn;
    FHandler, FAfter: THttpHandler;
    FDeferred, FOrphan, FInPump: Boolean;
    function Span(const S: THttpSpan): string;
    procedure ResetAnswer;
    procedure SetStatus(Code: Integer);
    procedure SetContentType(const Value: string);
    function GetRoute: TRouteContext;
    function GetResponseBody: string;
    function BodyLength: Integer;
  public
    Data: TObject;             { the program's, per connection (freed with it) }
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
    { (loop engine) the answer comes later: Complete sends it }
    procedure Defer;
    procedure Complete;
    { the whole answer: status line, headers, body (none for HEAD) }
    function Response: string;
    property Status: Integer read FStatus write SetStatus;
    property ContentType: string read FContentType write SetContentType;
    property Answered: Boolean read FAnswered;
    property ResponseBody: string read GetResponseBody;
    property Closing: Boolean read FClose;
    property Deferred: Boolean read FDeferred;
    { the loop engine's connection (nil on the async engine) }
    property Conn: TLoopConn read FConn;
    { netrouter's context, reused (made on first use) }
    property Route: TRouteContext read GetRoute;
    { (internal) drops what a request left behind: the engines call it
      when the connection goes idle }
    procedure Shrink;
  end;

  THttpRouter = specialize TRouter<THttpContext>;

var
  HttpLimits: THttpLimits;
  { writes the answer to a request that could not be parsed (C.Status: its
    code; the connection closes after it); nil: a plain text one }
  HttpOnBadRequest: THttpHandler;

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

{ Serve the connections of Listener, each as a task (on the pool); does
  not return (run it with Wait, or spawn it). After runs after each answer
  has been sent, in the connection's task. }
function HttpServeAsync(Listener: TSocket; Handler: THttpHandler;
                        After: THttpHandler = nil; OnPool: Boolean = true): async;
{ (a router: HttpServeAsync(listener, RouterHandler(r))). OnPool false: the
  connections' tasks stay on the thread that runs HttpServeAsync. }

{ the file Name: content type by extension; a directory's index.html; 404 }
procedure ServeFile(C: THttpContext; const Name: string);
{ a handler for a catch-all route: serves Root + '/' + the route's '*'
  (or the request path when the route has none); '..' is a 403 }
function FileServer(Root: string): THttpRouter.THandler;
{ is a path free of '..' segments (and NULs)? }
function HttpSafePath(const P: string): Boolean;

{$ENDIF}

implementation

type
  { 16 bytes of a request at once (SIMD: SSE2, NEON) }
  THttpScan16 = vector[16] of Byte;

{ The first CR of P[i, limit), or limit: 16 bytes at a time, then one by
  one. }
function FindCR(P: PAnsiChar; i, limit: Integer): Integer;
var
  mask: QWord;
begin
  while i + 16 <= limit do
  begin
    mask := VectorMask(VectorLoad(THttpScan16, P + i) = 13);
    if mask <> 0 then
      exit(i + CountTrailingZeros(mask));
    i := i + 16;
  end;
  while (i < limit) and ((P + i)^ <> #13) do
    inc(i);
  Result := i;
end;

{ The start of the first CR LF CR LF in P[0, Len), or -1; hpHeadTooLarge
  in TooLarge when it is not within HttpLimits.MaxHeadBytes (as the byte
  loop it replaces decided: found at an offset up to the limit, or not
  found and Len over it). }
function FindHeadEnd(P: PAnsiChar; Len: Integer; out TooLarge: Boolean): Integer;
var
  i: Integer;
begin
  TooLarge := false;
  i := 0;
  while true do
  begin
    i := FindCR(P, i, Len - 3);
    if i >= Len - 3 then
      break;
    if i > HttpLimits.MaxHeadBytes then
    begin
      TooLarge := true;
      exit(-1);
    end;
    if ((P + i + 1)^ = #10) and ((P + i + 2)^ = #13) and ((P + i + 3)^ = #10) then
      exit(i);
    inc(i);
  end;
  TooLarge := Len > HttpLimits.MaxHeadBytes;
  Result := -1;
end;

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

{ P[0..Len) equals S, ignoring ASCII case in both }
function SameNoCaseBoth(P: PAnsiChar; Len: Integer; const S: string): Boolean;
var
  i: Integer;
begin
  if Len <> Length(S) then
    exit(false);
  for i := 0 to Len - 1 do
    if LowerAscii((P + i)^) <> LowerAscii(S[i + 1]) then
      exit(false);
  Result := true;
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
  hasCL, chunked, hasTE, http11, tooLarge: Boolean;
  connClose, connKeep: Boolean;
  bodyUsed: Integer;
  chunkBody: string;
begin
  Used := 0;
  { the head: up to the empty line }
  headEnd := FindHeadEnd(P, Len, tooLarge);
  if tooLarge then
    exit(hpHeadTooLarge);
  if headEnd < 0 then
    exit(hpIncomplete);
  C.FBuf := P;
  C.FNHeaders := 0;
  C.FChunked := false;
  C.FChunkedBody := '';
  C.FPathMade := false;
  C.FPath := '';
  { the request line: METHOD SP target SP HTTP/1.x }
  lineEnd := FindCR(P, 0, headEnd + 2);
  while (P + lineEnd + 1)^ <> #10 do
    lineEnd := FindCR(P, lineEnd + 1, headEnd + 2);
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
  C.FIsHead := (sp1 = 4) and (P^ = 'H') and ((P + 1)^ = 'E') and ((P + 2)^ = 'A') and
               ((P + 3)^ = 'D');
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
    i := FindCR(P, ls, headEnd + 2);
    while (P + i + 1)^ <> #10 do
      i := FindCR(P, i + 1, headEnd + 2);
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
  ResetAnswer;
end;

destructor THttpContext.Destroy;
begin
  if FHdr <> nil then FreeMem(FHdr);
  if FBodyBuf <> nil then FreeMem(FBodyBuf);
  Data.Free;
  FRoute.Free;
  inherited Destroy;
end;

function THttpContext.GetRoute: TRouteContext;
begin
  if FRoute = nil then
    FRoute := TRouteContext.Create;
  Result := FRoute;
end;

procedure THttpContext.Shrink;
begin
  if FHdr <> nil then FreeMem(FHdr);
  FHdr := nil;
  FHdrLen := 0;
  FHdrCap := 0;
  if FBodyBuf <> nil then FreeMem(FBodyBuf);
  FBodyBuf := nil;
  FBodyLen := 0;
  FBodyCap := 0;
  { an idle keep-alive connection keeps its context: make it small }
  FRoute.Free;
  FRoute := nil;
  SetLength(FNames, 0);
  SetLength(FValues, 0);
  FNHeaders := 0;
  FBuf := nil;
  FPath := '';
  FPathMade := false;
  FChunkedBody := '';
  FChunked := false;
  ResetAnswer;
end;

procedure THttpContext.ResetAnswer;
begin
  FStatus := 200;
  FContentType := '';
  FHdrLen := 0;                { (the buffers are kept for the next answer) }
  FBody := '';
  FBodyLen := 0;
  FBuffered := false;
  FHeadLength := -1;
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
  q, endq, i, ps, pe, ke: Integer;
  plain, match: Boolean;
  sp: THttpSpan;
begin
  Result := '';
  q := -1;
  for i := 0 to FTarget.Len - 1 do
    if (FBuf + FTarget.Off + i)^ = '?' then
    begin
      q := FTarget.Off + i + 1;
      break;
    end;
  if q < 0 then
    exit;
  endq := FTarget.Off + FTarget.Len;
  { the pairs are read where they are: only the value found is made }
  ps := q;
  while ps < endq do
  begin
    pe := ps;
    while (pe < endq) and ((FBuf + pe)^ <> '&') do
      Inc(pe);
    ke := ps;
    plain := true;
    while (ke < pe) and ((FBuf + ke)^ <> '=') do
    begin
      if ((FBuf + ke)^ = '%') or ((FBuf + ke)^ = '+') then
        plain := false;
      Inc(ke);
    end;
    if plain then
    begin
      match := ke - ps = Length(Name);
      i := 0;
      while match and (i < ke - ps) do
      begin
        if (FBuf + ps + i)^ <> Name[i + 1] then
          match := false;
        Inc(i);
      end;
    end
    else
    begin
      sp.Off := ps;
      sp.Len := ke - ps;
      match := HttpUrlDecode(Span(sp), true) = Name;
    end;
    if match then
    begin
      if ke >= pe then
        exit('');
      sp.Off := ke + 1;
      sp.Len := pe - ke - 1;
      exit(HttpUrlDecode(Span(sp), true));
    end;
    ps := pe + 1;
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
begin
  for i := 0 to FNHeaders - 1 do
    if SameNoCaseBoth(FBuf + FNames[i].Off, FNames[i].Len, Name) then
      exit(true);
  Result := false;
end;

function THttpContext.Header(const Name: string): string;
var
  i: Integer;
begin
  for i := 0 to FNHeaders - 1 do
    if SameNoCaseBoth(FBuf + FNames[i].Off, FNames[i].Len, Name) then
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

{ S at the end of the buffer P (Len bytes of Cap), growing it by doubling }
procedure BufAppend(var P: PAnsiChar; var Len, Cap: Integer; const S: string);
var
  n: Integer;
begin
  if S = '' then
    exit;
  if Len + Length(S) > Cap then
  begin
    n := Cap;
    if n < 256 then n := 256;
    while n < Len + Length(S) do
      n := n * 2;
    ReallocMem(P, n);
    Cap := n;
  end;
  Move(S[1], (P + Len)^, Length(S));
  Len := Len + Length(S);
end;

{ the header line that starts with Low (lowercase, with its colon) in
  P[0..Len): where it starts, and its length with the CRLF; -1: none }
function FindHeaderLine(P: PAnsiChar; Len: Integer; const Low: string;
                        out LineLen: Integer): Integer;
var
  i, e: Integer;
begin
  i := 0;
  while i < Len do
  begin
    e := i;
    while (e < Len) and ((P + e)^ <> #13) do
      Inc(e);
    if (e - i >= Length(Low)) and SameNoCase(P + i, Length(Low), Low) then
    begin
      LineLen := e + 2 - i;
      exit(i);
    end;
    i := e + 2;
  end;
  Result := -1;
end;

procedure THttpContext.SetHeader(const Name, Value: string);
var
  at, n: Integer;
begin
  { drop a line of the same name, then add }
  at := FindHeaderLine(FHdr, FHdrLen, LowerCase(Name) + ':', n);
  if at >= 0 then
  begin
    Move((FHdr + at + n)^, (FHdr + at)^, FHdrLen - at - n);
    FHdrLen := FHdrLen - n;
  end;
  BufAppend(FHdr, FHdrLen, FHdrCap, Name);
  BufAppend(FHdr, FHdrLen, FHdrCap, ': ');
  BufAppend(FHdr, FHdrLen, FHdrCap, Value);
  BufAppend(FHdr, FHdrLen, FHdrCap, CRLF);
  FAnswered := true;
end;

procedure THttpContext.AddHeader(const Name, Value: string);
begin
  BufAppend(FHdr, FHdrLen, FHdrCap, Name);
  BufAppend(FHdr, FHdrLen, FHdrCap, ': ');
  BufAppend(FHdr, FHdrLen, FHdrCap, Value);
  BufAppend(FHdr, FHdrLen, FHdrCap, CRLF);
  FAnswered := true;
end;

procedure THttpContext.Write(const S: string);
begin
  if not FBuffered then
  begin
    { what Send gave comes first }
    FBodyLen := 0;
    BufAppend(FBodyBuf, FBodyLen, FBodyCap, FBody);
    FBody := '';
    FBuffered := true;
  end;
  BufAppend(FBodyBuf, FBodyLen, FBodyCap, S);
  FAnswered := true;
end;

function THttpContext.BodyLength: Integer;
begin
  if FHeadLength >= 0 then
    Result := Integer(FHeadLength)
  else if FBuffered then
    Result := FBodyLen
  else
    Result := Length(FBody);
end;

function THttpContext.GetResponseBody: string;
begin
  if not FBuffered then
    exit(FBody);
  SetLength(Result, FBodyLen);
  if FBodyLen > 0 then
    Move(FBodyBuf^, Result[1], FBodyLen);
end;

procedure THttpContext.Send(Code: Integer; const CType, Content: string);
begin
  FStatus := Code;
  FContentType := CType;
  FBody := Content;
  FBuffered := false;
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

procedure THttpContext.Defer;
begin
  if FConn = nil then
    raise Exception.Create('nethttp: Defer is for the loop engine (HttpServeLoop)');
  FDeferred := true;
end;


{ S into R at Pos (R long enough) }
procedure Put(var R: string; var Pos: Integer; const S: string); inline;
begin
  if S <> '' then
  begin
    Move(S[1], R[Pos], Length(S));
    Pos := Pos + Length(S);
  end;
end;

function THttpContext.Response: string;
const
  CT = 'Content-Type: ';
  CL = 'Content-Length: ';
  CONN_KEEP = 'Connection: keep-alive'#13#10;
  CONN_CLOSE = 'Connection: close'#13#10;
var
  r, code, text, len, conn: string;
  noBody, withBody: Boolean;
  n, pos: Integer;
begin
  { one allocation: the length first, then the pieces }
  if KeepAlive then conn := CONN_KEEP else conn := CONN_CLOSE;
  noBody := (FStatus < 200) or (FStatus = 204) or (FStatus = 304);
  withBody := not noBody and not FIsHead;
  code := IntToStr(FStatus);
  text := HttpStatusText(FStatus);
  n := 9 + Length(code) + 1 + Length(text) + 2 + Length(conn) + FHdrLen + 2;
  if not noBody then
  begin
    len := IntToStr(BodyLength);
    n := n + Length(CL) + Length(len) + 2;
    if FContentType <> '' then
      n := n + Length(CT) + Length(FContentType) + 2;
  end;
  if withBody then
    n := n + BodyLength;
  SetLength(r, n);
  pos := 1;
  Put(r, pos, 'HTTP/1.1 ');
  Put(r, pos, code);
  Put(r, pos, ' ');
  Put(r, pos, text);
  Put(r, pos, CRLF);
  if not noBody then
  begin
    if FContentType <> '' then
    begin
      Put(r, pos, CT);
      Put(r, pos, FContentType);
      Put(r, pos, CRLF);
    end;
    Put(r, pos, CL);
    Put(r, pos, len);
    Put(r, pos, CRLF);
  end;
  Put(r, pos, conn);
  if FHdrLen > 0 then
  begin
    Move(FHdr^, r[pos], FHdrLen);
    pos := pos + FHdrLen;
  end;
  Put(r, pos, CRLF);
  if withBody then
    if FBuffered then
    begin
      if FBodyLen > 0 then
        Move(FBodyBuf^, r[pos], FBodyLen);
    end
    else
      Put(r, pos, FBody);
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
function c_fseek(f: Pointer; off: Int64; whence: LongInt): LongInt; cdecl; external 'c' name 'fseeko';
function c_ftell(f: Pointer): Int64; cdecl; external 'c' name 'ftello';

{ the size of an open file by seeking to its end and back; -1 when it cannot
  seek (a pipe) }
function OpenFileSize(f: Pointer): Int64;
begin
  Result := -1;
  if c_fseek(f, 0, 2) <> 0 then
    exit;
  Result := c_ftell(f);
  if c_fseek(f, 0, 0) <> 0 then
    Result := -1;
end;

{ the whole file: made once at its size and read straight into the result
  (the bytes it has when opened: a file that shrinks gives what is there,
  one that grows is cut at the size seen); a file that cannot say its size
  (a pipe) is read in pieces }
function ReadWholeFile(const Name: string; out Content: string): Boolean;
var
  f: Pointer;
  buf: array[0..65535] of AnsiChar;
  n, used, size: Int64;
  r: string;
begin
  Result := false;
  Content := '';
  f := c_fopen(PChar(Name), 'rb');
  if f = nil then
    exit;
  r := '';
  used := 0;
  size := OpenFileSize(f);
  if size >= 0 then
  begin
    if size > 0 then
    begin
      SetLengthRaw(r, size);
      used := c_fread(PChar(r), 1, size, f);
      if used < size then
        SetLength(r, used);
    end;
  end
  else
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

{ the size of the file, or -1 }
function FileLength(const Name: string): Int64;
var
  f: Pointer;
begin
  f := c_fopen(PChar(Name), 'rb');
  if f = nil then
    exit(-1);
  Result := OpenFileSize(f);
  c_fclose(f);
end;

procedure ServeFile(C: THttpContext; const Name: string);
var
  fn, content: string;
  size: Int64;
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
  if C.FIsHead then
  begin
    { HEAD: the length and type, without reading the file }
    size := FileLength(fn);
    if size < 0 then
    begin
      C.Text(500, 'the file could not be read' + #10);
      exit;
    end;
    C.Send(200, HttpContentType(fn), '');
    C.FHeadLength := size;
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
      rest := '/' + rest; { C.Path is already decoded: decoding again would read %2574 as t }
    if not HttpSafePath(rest) then
    begin
      C.Text(403, '403 forbidden' + #10);
      exit;
    end;
    ServeFile(C, base + rest);
  end;
end;

{ ---- a request that cannot be parsed ---- }

procedure BadRequest(Ctx: THttpContext; Res: THttpParse);
var
  code: Integer;
begin
  code := HttpErrorStatus(Res);
  Ctx.ResetAnswer;
  Ctx.FKeepAlive := false;
  Ctx.FIsHead := false;
  Ctx.FMethod.Len := 0;
  Ctx.FTarget.Len := 0;
  Ctx.FNHeaders := 0;
  Ctx.FBodySpan.Len := 0;
  Ctx.FChunked := false;
  Ctx.FStatus := code;
  if Assigned(HttpOnBadRequest) then
    HttpOnBadRequest(Ctx)
  else
    Ctx.Text(code, HttpStatusText(code) + #10);
  Ctx.Close;
end;

{ ---- serving on netloop ---- }

{ every whole request in the connection's input, answered in order; stops
  at a deferred one (its Complete goes on) }
procedure LoopPump(Ctx: THttpContext);
var
  c: TLoopConn;
  off, used: Integer;
  res: THttpParse;
begin
  c := Ctx.FConn;
  off := 0;
  Ctx.FInPump := true;
  while not c.Closing do
  begin
    res := HttpParseRequest(c.InPtr + off, c.InCount - off, Ctx, used);
    if res = hpIncomplete then
      break;
    if res <> hpOk then
    begin
      BadRequest(Ctx, res);
      c.Write(Ctx.Response);
      c.Close;
      if Assigned(Ctx.FAfter) then Ctx.FAfter(Ctx);
      off := c.InCount;
      break;
    end;
    Ctx.FHandler(Ctx);
    off := off + used;
    if Ctx.FDeferred then
    begin
      c.Hold := true;          { a peer that closes its side still gets it }
      break;
    end;
    c.Write(Ctx.Response);
    if Assigned(Ctx.FAfter) then Ctx.FAfter(Ctx);
    if not Ctx.KeepAlive then
      c.Close;
  end;
  Ctx.FInPump := false;
  c.Consume(off);
  if (c.InCount = 0) and not Ctx.FDeferred then
    if Ctx.Data = nil then
    begin
      { idle: no context at all (the next bytes make one) }
      c.Obj := nil;
      Ctx.Free;
    end
    else
      Ctx.Shrink;              { (it keeps the program's Data) }
end;

procedure THttpContext.Complete;
var
  c: TLoopConn;
begin
  if not FDeferred then
    exit;
  FDeferred := false;
  if FInPump then
    exit;                      { completed before its handler returned }
  if FOrphan then
  begin
    Free;                      { the connection is gone }
    exit;
  end;
  c := FConn;
  c.Hold := false;
  c.Write(Response);
  if Assigned(FAfter) then FAfter(Self);
  if not KeepAlive then
    c.Close
  else
    LoopPump(Self);            { the requests that came meanwhile }
end;

type
  { what a server's connections are served with (a lambda that passes a
    captured closure on as an argument would call it instead: p3 bug) }
  THttpLoopBinding = class
    Handler, After: THttpHandler;
  end;

procedure LoopOpen(C: TLoopConn);
begin
  C.Obj := nil;                { (a context comes with the first bytes) }
end;

procedure LoopClose(C: TLoopConn);
var
  ctx: THttpContext;
begin
  ctx := THttpContext(C.Obj);
  C.Obj := nil;
  if ctx = nil then
    exit;
  if ctx.FDeferred then
  begin
    ctx.FOrphan := true;       { Complete frees it }
    ctx.FConn := nil;
  end
  else
    ctx.Free;
end;

procedure LoopData(C: TLoopConn; B: THttpLoopBinding);
var
  ctx: THttpContext;
begin
  ctx := THttpContext(C.Obj);
  if ctx = nil then
  begin
    if C.InCount = 0 then
      exit;                    { (the peer closed, idle) }
    ctx := THttpContext.Create;
    ctx.FConn := C;
    ctx.FHandler := B.Handler;
    ctx.FAfter := B.After;
    C.Obj := ctx;
  end;
  if not ctx.FDeferred then
    LoopPump(ctx);             { (may free ctx: the connection is idle) }
end;

procedure HttpServeLoop(Server: TNetLoopServer; const Handler: THttpHandler;
                        const After: THttpHandler = nil);
var
  b: THttpLoopBinding;
begin
  b := THttpLoopBinding.Create;   { (lives as long as the server's callbacks) }
  b.Handler := Handler;
  b.After := After;
  Server.OnOpen := @LoopOpen;
  Server.OnClose := @LoopClose;
  Server.OnData := procedure(C: TLoopConn) => LoopData(C, b);
end;

{ ---- serving on netasync ---- }

const
{$IFDEF MSWINDOWS}
  EINTR_ = 10004;              { WSAEINTR }
  EAGAIN_ = 10035;             { WSAEWOULDBLOCK }
  MSG_NOSIGNAL_ = 0;
{$ELSE}
  EINTR_ = 4;
{$IFDEF DARWIN}
  EAGAIN_ = 35;
  MSG_NOSIGNAL_ = 0;           { (NetAccept's sockets have SO_NOSIGPIPE) }
{$ELSE}
  EAGAIN_ = 11;
  MSG_NOSIGNAL_ = $4000;
{$ENDIF}
{$ENDIF}

{$IFDEF MSWINDOWS}
function WSAGetLastError: LongInt; cdecl; external 'ws2_32' name 'WSAGetLastError';
{ Winsock: a SOCKET is pointer-sized, recv and send return an int }
function ws_recv(s: Int64; buf: Pointer; len: LongInt; flags: LongInt): LongInt; cdecl;
  external 'ws2_32' name 'recv';
function ws_send(s: Int64; buf: Pointer; len: LongInt; flags: LongInt): LongInt; cdecl;
  external 'ws2_32' name 'send';

{ the last socket error (Winsock keeps it per thread, not in errno) }
function c_errno: LongInt;
begin
  Result := WSAGetLastError;
end;

function c_recv(s: LongInt; buf: Pointer; len: Int64; flags: LongInt): Int64;
begin
  Result := ws_recv(s, buf, LongInt(len), flags);
end;

function c_send(s: LongInt; buf: Pointer; len: Int64; flags: LongInt): Int64;
begin
  Result := ws_send(s, buf, LongInt(len), flags);
end;
{$ELSE}
var
  c_errno: LongInt; external 'c' name 'errno';

function c_recv(s: LongInt; buf: Pointer; len: Int64; flags: LongInt): Int64; cdecl; external 'c' name 'recv';
function c_send(s: LongInt; buf: Pointer; len: Int64; flags: LongInt): Int64; cdecl; external 'c' name 'send';
{$ENDIF}

{ every byte, parking the task on EAGAIN (no task per call); false: the
  peer is gone, or RequestTimeoutMs passed without progress }
function SendAll(Sock: TSocket; P: PAnsiChar; Len: Integer): Boolean;
var
  off: Integer;
  n: Int64;
  e, bits: Integer;
begin
  off := 0;
  while off < Len do
  begin
    n := c_send(Sock, P + off, Len - off, MSG_NOSIGNAL_);
    if n > 0 then
    begin
      off := off + Integer(n);
      continue;
    end;
    e := c_errno;
    if (n < 0) and (e = EINTR_) then
      continue;
    if (n < 0) and (e = EAGAIN_) then
    begin
      bits := NetWaitFdUntil(Sock, false, true, HttpLimits.RequestTimeoutMs);
      if (bits and NW_TIMEOUT) <> 0 then
        exit(false);
      continue;
    end;
    exit(false);
  end;
  Result := true;
end;

function HttpServeConn(Sock: TSocket; Handler: THttpHandler; After: THttpHandler): async;
var
  ctx: THttpContext;
  buf: PAnsiChar;
  cap, len, start, used, e, bits, wait: Integer;
  n: Int64;
  res: THttpParse;
  resp: string;
  peerGone: Boolean;
begin
  NetSetNoDelay(Sock);
  ctx := THttpContext.Create;
  cap := 0;
  buf := nil;
  len := 0;
  start := 0;                   { the pipelined bytes before it are done with }
  peerGone := false;
  try
    while true do
    begin
      if len > start then
        res := HttpParseRequest(buf + start, len - start, ctx, used)
      else
        res := hpIncomplete;
      if res = hpIncomplete then
      begin
        if peerGone then
          exit;                 { (with half a request, or none) }
        if start > 0 then
        begin
          { move what is left down, once per receive (not per request) }
          len := len - start;
          if len > 0 then
            Move((buf + start)^, buf^, len);
          start := 0;
        end;
        if len = cap then
        begin
          if cap = 0 then cap := 4096 else cap := cap * 2;
          ReallocMem(buf, cap);
        end;
        n := c_recv(Sock, buf + len, cap - len, 0);
        if n > 0 then
        begin
          len := len + Integer(n);
          continue;
        end;
        if n = 0 then
        begin
          peerGone := true;
          continue;
        end;
        e := c_errno;
        if e = EINTR_ then
          continue;
        if e <> EAGAIN_ then
          exit;
        if len = 0 then
        begin
          { idle: no buffer (and a small context) while waiting }
          FreeMem(buf);
          buf := nil;
          cap := 0;
          ctx.Shrink;
          wait := HttpLimits.IdleTimeoutMs;
        end
        else
          wait := HttpLimits.RequestTimeoutMs;
        if wait > 0 then
          bits := NetWaitFdUntil(Sock, true, false, wait)
        else
          bits := NetWaitFd(Sock, true, false);
        if (bits and NW_TIMEOUT) <> 0 then
          exit;
        continue;
      end;
      if res <> hpOk then
      begin
        BadRequest(ctx, res);
        resp := ctx.Response;
        SendAll(Sock, PAnsiChar(resp), Length(resp));
        if Assigned(After) then After(ctx);
        exit;
      end;
      Handler(ctx);
      resp := ctx.Response;
      if not SendAll(Sock, PAnsiChar(resp), Length(resp)) then
        exit;
      if Assigned(After) then After(ctx);
      if not ctx.KeepAlive then
        exit;
      { the next request: what followed this one }
      start := start + used;
    end;
  finally
    if buf <> nil then
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

function HttpServeAsync(Listener: TSocket; Handler: THttpHandler; After: THttpHandler;
                        OnPool: Boolean): async;
begin
  while true do
    if OnPool then
      detached(HttpDropped) HttpLaunch(NetAcceptDirect(Listener), Handler, After)
    else
      detached(HttpDropped) HttpServeConn(NetAcceptDirect(Listener), Handler, After);
end;

initialization
  HttpLimits.MaxHeadBytes := 16384;
  HttpLimits.MaxHeaders := 100;
  HttpLimits.MaxBodyBytes := 8 * 1024 * 1024;
  HttpLimits.IdleTimeoutMs := 75000;
  HttpLimits.RequestTimeoutMs := 30000;

{$ENDIF}

end.
