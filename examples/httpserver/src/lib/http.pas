unit http;
{ HTTP/1.0-style requests and responses: one request per connection. }
{$H+}
interface

uses sockets;

type
  THttpRequest = record
    Method: string;   { GET, HEAD, ... }
    Target: string;   { as sent: /docs/a%20b.html?x=1 }
    Path: string;     { decoded, without the query: /docs/a b.html }
    Query: string;    { x=1 }
    Version: string;  { HTTP/1.1 }
  end;

const
  MaxRequestSize = 8192; { request line and headers }

{ Reads a request's head from the connection and parses its first line;
  false if the connection ended or the request is malformed or too long. }
function ReadRequest(s: TSocket; var req: THttpRequest): boolean;
{ Sends a response: the status line, headers, and the body unless headOnly
  (the answer to HEAD). False if the connection broke. }
function SendResponse(s: TSocket; status: longint; const contentType, body: string;
                      headOnly: boolean): boolean;
{ A small HTML page saying what went wrong. }
function ErrorPage(status: longint; const detail: string): string;
function StatusText(status: longint): string;
{ %XX and + decoded. }
function UrlDecode(const s: string): string;
{ & < > " as HTML entities. }
function HtmlEscape(const s: string): string;
{ The media type for a file name, by its extension. }
function ContentTypeOf(const name: string): string;

implementation

uses sysutils;

const
  CRLF = #13#10;

function HexValue(c: char): longint;
begin
  case c of
    '0'..'9': HexValue := ord(c) - ord('0');
    'a'..'f': HexValue := ord(c) - ord('a') + 10;
    'A'..'F': HexValue := ord(c) - ord('A') + 10;
  else
    HexValue := -1;
  end;
end;

function UrlDecode(const s: string): string;
var
  i, hi, lo: longint;
  r: string;
begin
  r := '';
  i := 1;
  while i <= length(s) do
  begin
    if (s[i] = '%') and (i + 2 <= length(s)) then
    begin
      hi := HexValue(s[i + 1]);
      lo := HexValue(s[i + 2]);
      if (hi >= 0) and (lo >= 0) then
      begin
        r := r + chr(hi * 16 + lo);
        i := i + 3;
        continue;
      end;
    end;
    if s[i] = '+' then
      r := r + ' '
    else
      r := r + s[i];
    i := i + 1;
  end;
  UrlDecode := r;
end;

function HtmlEscape(const s: string): string;
var
  i: longint;
  r: string;
begin
  r := '';
  for i := 1 to length(s) do
    case s[i] of
      '&': r := r + '&amp;';
      '<': r := r + '&lt;';
      '>': r := r + '&gt;';
      '"': r := r + '&quot;';
    else
      r := r + s[i];
    end;
  HtmlEscape := r;
end;

function StatusText(status: longint): string;
begin
  case status of
    200: StatusText := 'OK';
    400: StatusText := 'Bad Request';
    403: StatusText := 'Forbidden';
    404: StatusText := 'Not Found';
    405: StatusText := 'Method Not Allowed';
    500: StatusText := 'Internal Server Error';
  else
    StatusText := 'Unknown';
  end;
end;

function ContentTypeOf(const name: string): string;
var
  ext: string;
begin
  ext := LowerCase(ExtractFileExt(name));
  if (ext = '.html') or (ext = '.htm') then
    ContentTypeOf := 'text/html; charset=utf-8'
  else if ext = '.css' then
    ContentTypeOf := 'text/css; charset=utf-8'
  else if ext = '.js' then
    ContentTypeOf := 'text/javascript; charset=utf-8'
  else if ext = '.json' then
    ContentTypeOf := 'application/json'
  else if (ext = '.txt') or (ext = '.pas') or (ext = '.md') then
    ContentTypeOf := 'text/plain; charset=utf-8'
  else if ext = '.svg' then
    ContentTypeOf := 'image/svg+xml'
  else if ext = '.png' then
    ContentTypeOf := 'image/png'
  else if (ext = '.jpg') or (ext = '.jpeg') then
    ContentTypeOf := 'image/jpeg'
  else if ext = '.gif' then
    ContentTypeOf := 'image/gif'
  else if ext = '.ico' then
    ContentTypeOf := 'image/x-icon'
  else
    ContentTypeOf := 'application/octet-stream';
end;

function ErrorPage(status: longint; const detail: string): string;
begin
  ErrorPage := '<!doctype html><html><head><title>' + IntToStr(status) + ' ' +
               StatusText(status) + '</title></head><body><h1>' + IntToStr(status) +
               ' ' + StatusText(status) + '</h1><p>' + HtmlEscape(detail) +
               '</p></body></html>' + #10;
end;

{ The request line "METHOD target VERSION" into req; false if it is not
  one. }
function ParseRequestLine(const line: string; var req: THttpRequest): boolean;
var
  a, b, q: longint;
  rest: string;
begin
  ParseRequestLine := false;
  a := pos(' ', line);
  if a < 2 then
    exit;
  rest := copy(line, a + 1, length(line) - a);
  b := pos(' ', rest);
  if b < 2 then
    exit;
  req.Method := copy(line, 1, a - 1);
  req.Target := copy(rest, 1, b - 1);
  req.Version := copy(rest, b + 1, length(rest) - b);
  if (copy(req.Version, 1, 5) <> 'HTTP/') or (req.Target[1] <> '/') then
    exit;
  q := pos('?', req.Target);
  if q > 0 then
  begin
    req.Path := UrlDecode(copy(req.Target, 1, q - 1));
    req.Query := copy(req.Target, q + 1, length(req.Target) - q);
  end
  else
  begin
    req.Path := UrlDecode(req.Target);
    req.Query := '';
  end;
  ParseRequestLine := true;
end;

function ReadRequest(s: TSocket; var req: THttpRequest): boolean;
var
  buf: array[0..1023] of char;
  head: string;
  n, i, eol: longint;
begin
  ReadRequest := false;
  head := '';
  { up to the empty line that ends the headers }
  while pos(CRLF + CRLF, head) = 0 do
  begin
    if length(head) > MaxRequestSize then
      exit;
    n := Receive(s, buf, sizeof(buf));
    if n <= 0 then
      exit;
    for i := 0 to n - 1 do
      head := head + buf[i];
  end;
  eol := pos(CRLF, head);
  ReadRequest := ParseRequestLine(copy(head, 1, eol - 1), req);
end;

function SendResponse(s: TSocket; status: longint; const contentType, body: string;
                      headOnly: boolean): boolean;
var
  head: string;
begin
  head := 'HTTP/1.1 ' + IntToStr(status) + ' ' + StatusText(status) + CRLF +
          'Server: pcc-httpserver' + CRLF +
          'Content-Type: ' + contentType + CRLF +
          'Content-Length: ' + IntToStr(length(body)) + CRLF +
          'Connection: close' + CRLF + CRLF;
  if headOnly then
    SendResponse := SendAll(s, head)
  else
    SendResponse := SendAll(s, head + body);
end;

end.
