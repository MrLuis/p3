program httpserver;
{ A basic HTTP server.

    httpserver [--port N] [--root DIR] [--any] [--requests N]

  Serves the files under DIR (default: public) to GET and HEAD requests,
  one connection at a time; a directory gives its index.html; /status is a
  page made by the server. It listens on 127.0.0.1 (--any: every address),
  port 8080 (--port 0: any free port, which it prints), and stops after N
  requests if --requests is given. }
{$H+}
uses sysutils, sockets, http;

var
  Port: longint = 8080;
  Root: string = 'public';
  AnyAddress: boolean = false;
  MaxRequests: longint = 0; { 0: no limit }
  Served: longint = 0;
  StartedAt: int64;

{ C's stdio, for reading files of any content and size. }
function c_fopen(name, mode: pchar): pointer; cdecl; external 'c' name 'fopen';
function c_fread(var buf; size, count: int64; f: pointer): int64; cdecl; external 'c' name 'fread';
function c_fclose(f: pointer): longint; cdecl; external 'c' name 'fclose';

{ Writes a line of the log now, also when output is a file or a pipe. }
procedure Log(const line: string);
begin
  writeln(line);
  flush(output);
end;

procedure Usage;
begin
  writeln('usage: httpserver [--port N] [--root DIR] [--any] [--requests N]');
  halt(1);
end;

procedure ParseArguments;
var
  i: longint;
  a: string;

  function Value: string;
  begin
    if i = ParamCount then
      Usage;
    i := i + 1;
    Value := ParamStr(i);
  end;

begin
  i := 1;
  while i <= ParamCount do
  begin
    a := ParamStr(i);
    if a = '--port' then
      Port := StrToIntDef(Value, -1)
    else if a = '--root' then
      Root := Value
    else if a = '--any' then
      AnyAddress := true
    else if a = '--requests' then
      MaxRequests := StrToIntDef(Value, -1)
    else
      Usage;
    i := i + 1;
  end;
  if (Port < 0) or (Port > 65535) or (MaxRequests < 0) then
    Usage;
end;

{ The whole file into content; false if it cannot be read. }
function ReadFile(const name: string; var content: string): boolean;
var
  f: pointer;
  buf: array[0..4095] of char;
  n, i, used: int64;
begin
  ReadFile := false;
  content := '';
  f := c_fopen(pchar(name), 'rb');
  if f = nil then
    exit;
  used := 0;
  repeat
    n := c_fread(buf, 1, sizeof(buf), f);
    if n > 0 then
    begin
      setlength(content, used + n);
      for i := 0 to n - 1 do
        content[used + i + 1] := buf[i];
      used := used + n;
    end;
  until n < sizeof(buf);
  c_fclose(f);
  ReadFile := true;
end;

{ Is path free of '..' segments (which could leave the root)? }
function IsSafePath(const path: string): boolean;
var
  i, start: longint;
begin
  IsSafePath := false;
  if pos(#0, path) > 0 then
    exit;
  start := 1;
  for i := 1 to length(path) + 1 do
    if (i > length(path)) or (path[i] = '/') or (path[i] = '\') then
    begin
      if copy(path, start, i - start) = '..' then
        exit;
      start := i + 1;
    end;
  IsSafePath := true;
end;

function StatusPage: string;
var
  seconds: int64;
begin
  seconds := (GetTickCount64 - StartedAt) div 1000;
  StatusPage := '<!doctype html><html><head><title>Status</title></head><body>' +
                '<h1>httpserver</h1><ul>' +
                '<li>requests served: ' + IntToStr(Served) + '</li>' +
                '<li>running for: ' + IntToStr(seconds) + ' s</li>' +
                '<li>root: ' + HtmlEscape(Root) + '</li>' +
                '</ul><p><a href="/">home</a></p></body></html>' + #10;
end;

{ Answers one request; returns the status sent. }
function Respond(s: TSocket; const req: THttpRequest): longint;
var
  headOnly: boolean;
  name, body: string;

  function Fail(status: longint; const detail: string): longint;
  begin
    SendResponse(s, status, 'text/html; charset=utf-8', ErrorPage(status, detail), headOnly);
    Fail := status;
  end;

begin
  headOnly := req.Method = 'HEAD';
  if (req.Method <> 'GET') and not headOnly then
    exit(Fail(405, 'Only GET and HEAD are supported.'));
  if req.Path = '/status' then
  begin
    SendResponse(s, 200, 'text/html; charset=utf-8', StatusPage, headOnly);
    exit(200);
  end;
  if not IsSafePath(req.Path) then
    exit(Fail(403, 'The path leaves the served directory.'));
  name := Root + req.Path;
  if DirectoryExists(name) then
  begin
    if name[length(name)] <> '/' then
      name := name + '/';
    name := name + 'index.html';
  end;
  if not FileExists(name) then
    exit(Fail(404, 'There is no ' + req.Path + ' here.'));
  if not ReadFile(name, body) then
    exit(Fail(500, 'The file could not be read.'));
  SendResponse(s, 200, ContentTypeOf(name), body, headOnly);
  Respond := 200;
end;

procedure Serve(listener: TSocket);
var
  s: TSocket;
  req: THttpRequest;
  status: longint;
begin
  while (MaxRequests = 0) or (Served < MaxRequests) do
  begin
    s := Accept(listener);
    if s = InvalidSocket then
      continue;
    if ReadRequest(s, req) then
    begin
      status := Respond(s, req);
      Log(req.Method + ' ' + req.Target + ' ' + IntToStr(status));
    end
    else
    begin
      SendResponse(s, 400, 'text/html; charset=utf-8',
                   ErrorPage(400, 'The request could not be understood.'), false);
      Log('bad request 400');
    end;
    CloseSocket(s);
    Served := Served + 1;
  end;
end;

var
  Listener: TSocket;
begin
  ParseArguments;
  if not DirectoryExists(Root) then
  begin
    writeln('httpserver: no directory ', Root, ' to serve (--root DIR)');
    halt(1);
  end;
  Listener := Listen(Port, AnyAddress);
  if Listener = InvalidSocket then
  begin
    writeln('httpserver: cannot listen on port ', Port);
    halt(1);
  end;
  StartedAt := GetTickCount64;
  Log('httpserver: serving ' + Root + ' at http://127.0.0.1:' +
      IntToStr(BoundPort(Listener)) + '/');
  Serve(Listener);
  CloseSocket(Listener);
  Log('httpserver: stopped after ' + IntToStr(Served) + ' request(s)');
end.
