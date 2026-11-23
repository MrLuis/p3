{ nethttp's parser and answers, on buffers (no network): the request line,
  the decoded path and the query, headers, keep-alive rules, Content-Length
  and chunked bodies, pipelining, incomplete input, every error (400, 413,
  431, 505, the smuggling cases), and the shape of an answer (HEAD, 204,
  SetHeader, Redirect).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/nethttp-parse.expected
  }
{$mode objfpc}{$H+}
program nethttp_parse;
uses sysutils, nethttp;

var
  C: THttpContext;
  Keep: string;    { the request being looked at: the context reads it }

function Parse(const S: string; out Used: Integer): THttpParse;
begin
  if S = '' then
    Result := HttpParseRequest(nil, 0, C, Used)
  else
    Result := HttpParseRequest(PAnsiChar(S), Length(S), C, Used);
end;

function R(const S: string): string;
var
  used: Integer;
  res: THttpParse;
begin
  Keep := S;
  res := Parse(Keep, used);
  case res of
    hpOk: Result := 'ok(' + IntToStr(used) + ')';
    hpIncomplete: Result := 'incomplete';
  else
    Result := IntToStr(HttpErrorStatus(res));
  end;
end;

{ the head scanned 16 bytes at a time: line ends and the empty line at
  every offset of the blocks, a lone CR in a value, and a head cut short:
  each parses, with its values, or is incomplete }
procedure BlockOffsets;
var
  i, j, bad, used: Integer;
  pad, req: string;
begin
  bad := 0;
  for i := 0 to 40 do
    for j := 0 to 20 do
    begin
      pad := '';
      while Length(pad) < i do
        pad := pad + Chr(Ord('a') + Length(pad) mod 26);
      req := 'GET /' + Copy(pad, 1, j) + ' HTTP/1.1'#13#10'Host: h'#13#10 +
             'X-Pad: ' + pad + #13#10'X-Cr: a'#13'b'#13#10#13#10;
      Keep := req;
      if (Parse(Keep, used) <> hpOk) or (used <> Length(req)) or
         (C.Header('x-pad') <> pad) or (C.Header('x-cr') <> 'a' + #13 + 'b') then
        bad := bad + 1;
      Keep := Copy(req, 1, Length(req) - 1);
      if Parse(Keep, used) <> hpIncomplete then
        bad := bad + 1;
    end;
  Writeln('block offsets: ', bad, ' wrong');
end;

function Show(const S: string): string;
var
  i: Integer;
begin
  Result := '';
  for i := 1 to Length(S) do
    if S[i] = #13 then Result := Result + '\r'
    else if S[i] = #10 then Result := Result + '\n'
    else Result := Result + S[i];
end;

const
  NL = #13#10;
var
  req, big: string;
  used, i: Integer;
begin
  C := THttpContext.Create;

  Writeln('-- request line, path, query');
  req := 'GET /a%20b/c?x=1&y=%41+B&flag HTTP/1.1' + NL + 'Host: h' + NL + NL;
  Writeln(R(req), ' method=', C.Method, ' path=', C.Path, ' raw=', C.RawPath,
          ' query=', C.Query, ' version=', C.Version);
  Writeln('y=', C.QueryParam('y'), ' x=', C.QueryParam('x'), ' flag=[', C.QueryParam('flag'),
          '] none=[', C.QueryParam('none'), ']');
  Writeln(R('OPTIONS * HTTP/1.1' + NL + NL), ' ', C.Method, ' ', C.Target);
  Writeln(R('PURGE /cache HTTP/1.1' + NL + NL), ' ', C.Method);

  Writeln('-- headers');
  req := 'GET / HTTP/1.1' + NL + 'Host: example' + NL + 'X-Thing:   spaced value  ' + NL +
         'Accept: */*' + NL + NL;
  Writeln(R(req), ' count=', C.HeaderCount, ' x-thing=[', C.Header('x-thing'), '] HOST=[',
          C.Header('HOST'), '] has accept=', C.HasHeader('Accept'), ' has cookie=',
          C.HasHeader('cookie'), ' first=', C.HeaderName(0), ':', C.HeaderValue(0));

  Writeln('-- keep-alive');
  R('GET / HTTP/1.1' + NL + NL);                      Writeln('1.1: ', C.KeepAlive);
  R('GET / HTTP/1.1' + NL + 'Connection: close' + NL + NL); Writeln('1.1 close: ', C.KeepAlive);
  R('GET / HTTP/1.1' + NL + 'Connection: Upgrade, Close' + NL + NL); Writeln('1.1 upgrade, close: ', C.KeepAlive);
  R('GET / HTTP/1.0' + NL + NL);                      Writeln('1.0: ', C.KeepAlive);
  R('GET / HTTP/1.0' + NL + 'Connection: keep-alive' + NL + NL); Writeln('1.0 keep-alive: ', C.KeepAlive);

  Writeln('-- bodies and pipelining');
  req := 'POST /p HTTP/1.1' + NL + 'Content-Length: 5' + NL + NL + 'hello';
  Writeln(R(req), ' body=', C.Body);
  Writeln('short body: ', R(Copy(req, 1, Length(req) - 2)));
  req := req + 'GET /second HTTP/1.1' + NL + NL;
  Keep := req;
  Parse(Keep, used);
  Writeln('pipelined: first used ', used, ' of ', Length(req), '; then ',
          R(Copy(req, used + 1, Length(req))), ' ', C.Path);
  req := 'POST /c HTTP/1.1' + NL + 'Transfer-Encoding: chunked' + NL + NL +
         '4' + NL + 'Wiki' + NL + '5;ext=1' + NL + 'pedia' + NL + '0' + NL + 'X-Trailer: t' + NL + NL;
  Writeln('chunked: ', R(req), ' body=', C.Body, ' (all ', Length(req), ')');
  Writeln('chunked, cut: ', R(Copy(req, 1, Length(req) - 3)));
  Writeln('incomplete head: ', R('GET / HTTP/1.1' + NL + 'Host: x'));
  Writeln('nothing: ', R(''));

  Writeln('-- errors');
  Writeln('no target: ', R('GET' + NL + NL));
  Writeln('target without /: ', R('GET x HTTP/1.1' + NL + NL));
  Writeln('HTTP/2.0: ', R('GET / HTTP/2.0' + NL + NL));
  Writeln('not HTTP: ', R('GET / FTP/1.1' + NL + NL));
  Writeln('CL and TE: ', R('POST / HTTP/1.1' + NL + 'Content-Length: 4' + NL +
          'Transfer-Encoding: chunked' + NL + NL + '0' + NL + NL));
  Writeln('two CLs: ', R('POST / HTTP/1.1' + NL + 'Content-Length: 4' + NL +
          'Content-Length: 5' + NL + NL + 'hello'));
  Writeln('same CL twice: ', R('POST / HTTP/1.1' + NL + 'Content-Length: 5' + NL +
          'Content-Length: 5' + NL + NL + 'hello'));
  Writeln('CL not a number: ', R('POST / HTTP/1.1' + NL + 'Content-Length: 1x' + NL + NL));
  Writeln('CL too big: ', R('POST / HTTP/1.1' + NL + 'Content-Length: 4294967296' + NL + NL));
  Writeln('TE gzip: ', R('POST / HTTP/1.1' + NL + 'Transfer-Encoding: gzip' + NL + NL));
  Writeln('folded header: ', R('GET / HTTP/1.1' + NL + 'X: a' + NL + ' b' + NL + NL));
  Writeln('space in a name: ', R('GET / HTTP/1.1' + NL + 'Bad Name: v' + NL + NL));
  big := '';
  for i := 1 to 200 do
    big := big + 'X-H' + IntToStr(i) + ': v' + NL;
  Writeln('201 headers: ', R('GET / HTTP/1.1' + NL + big + NL));
  SetLength(big, 20000);
  FillChar(big[1], 20000, 'a');
  Writeln('a 20 KB head: ', R('GET /' + big));
  Writeln('bad chunk size: ', R('POST / HTTP/1.1' + NL + 'Transfer-Encoding: chunked' + NL + NL + 'zz' + NL));

  Writeln('-- answers');
  R('GET / HTTP/1.1' + NL + NL);
  C.Text(200, 'hi');
  Writeln(Show(C.Response));
  R('HEAD / HTTP/1.1' + NL + NL);
  C.Text(200, 'hi');
  Writeln(Show(C.Response));
  R('GET / HTTP/1.0' + NL + NL);
  C.Status := 204;
  Writeln(Show(C.Response));
  R('GET / HTTP/1.1' + NL + NL);
  C.SetHeader('X-A', '1');
  C.SetHeader('X-B', '2');
  C.SetHeader('x-a', '3');
  C.Json(201, '{}');
  C.Close;
  Writeln(Show(C.Response));
  R('GET / HTTP/1.1' + NL + NL);
  C.Redirect(302, '/there');
  Writeln(Show(C.Response));
  BlockOffsets;
  C.Free;
end.
