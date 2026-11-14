unit sockets;
{ TCP sockets through the C library (Linux and macOS): a listening socket,
  accepted connections, and reading and writing them. }
{$H+}
interface

type
  TSocket = longint;

const
  InvalidSocket = -1;

{ A socket listening on port (0: any free port) of 127.0.0.1, or of every
  address if anyAddress; InvalidSocket if that fails. }
function Listen(port: longint; anyAddress: boolean): TSocket;
{ The port a listening socket is bound to (0 if unknown). }
function BoundPort(s: TSocket): longint;
{ Waits for the next connection; InvalidSocket if that fails. }
function Accept(listener: TSocket): TSocket;
{ Reads up to size bytes into buf: their number, 0 at the end of the
  connection, negative on an error. }
function Receive(s: TSocket; var buf; size: longint): longint;
{ Writes all of data; false if the connection broke. }
function SendAll(s: TSocket; const data: string): boolean;
procedure CloseSocket(s: TSocket);

implementation

type
  { struct sockaddr_in }
  TSockAddrIn = record
{$IFDEF DARWIN}
    len: byte;
    family: byte;
{$ELSE}
    family: word;
{$ENDIF}
    port: word;       { network byte order }
    addr: longword;   { network byte order }
    zero: array[0..7] of byte;
  end;

const
  AF_INET = 2;
  SOCK_STREAM = 1;
{$IFDEF DARWIN}
  SOL_SOCKET = $FFFF;
  SO_REUSEADDR = 4;
  SO_NOSIGPIPE = $1022;
  SendFlags = 0;
{$ELSE}
  SOL_SOCKET = 1;
  SO_REUSEADDR = 2;
  SendFlags = $4000;  { MSG_NOSIGNAL: a closed connection is an error, not a signal }
{$ENDIF}

function c_socket(domain, kind, protocol: longint): longint; cdecl; external 'c' name 'socket';
function c_setsockopt(s, level, option: longint; var value: longint; size: longword): longint; cdecl; external 'c' name 'setsockopt';
function c_bind(s: longint; var addr: TSockAddrIn; size: longword): longint; cdecl; external 'c' name 'bind';
function c_listen(s, backlog: longint): longint; cdecl; external 'c' name 'listen';
function c_accept(s: longint; addr, size: pointer): longint; cdecl; external 'c' name 'accept';
function c_getsockname(s: longint; var addr: TSockAddrIn; var size: longword): longint; cdecl; external 'c' name 'getsockname';
function c_recv(s: longint; var buf; size: int64; flags: longint): int64; cdecl; external 'c' name 'recv';
function c_send(s: longint; const data; size: int64; flags: longint): int64; cdecl; external 'c' name 'send';
function c_close(s: longint): longint; cdecl; external 'c' name 'close';

{ A 16-bit value in network byte order (the host's is little-endian). }
function Swap16(v: longint): word;
begin
  Swap16 := word(((v and $FF) shl 8) or ((v shr 8) and $FF));
end;

function Listen(port: longint; anyAddress: boolean): TSocket;
var
  s, one: longint;
  addr: TSockAddrIn;
begin
  Listen := InvalidSocket;
  s := c_socket(AF_INET, SOCK_STREAM, 0);
  if s < 0 then
    exit;
  one := 1;
  c_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, one, sizeof(one));
  FillChar(addr, sizeof(addr), 0);
{$IFDEF DARWIN}
  addr.len := sizeof(addr);
{$ENDIF}
  addr.family := AF_INET;
  addr.port := Swap16(port);
  if anyAddress then
    addr.addr := 0
  else
    addr.addr := $0100007F; { 127.0.0.1 in network byte order }
  if (c_bind(s, addr, sizeof(addr)) <> 0) or (c_listen(s, 16) <> 0) then
  begin
    c_close(s);
    exit;
  end;
  Listen := s;
end;

function BoundPort(s: TSocket): longint;
var
  addr: TSockAddrIn;
  size: longword;
begin
  BoundPort := 0;
  size := sizeof(addr);
  FillChar(addr, sizeof(addr), 0);
  if c_getsockname(s, addr, size) = 0 then
    BoundPort := Swap16(addr.port);
end;

function Accept(listener: TSocket): TSocket;
var
  s: longint;
{$IFDEF DARWIN}
  one: longint;
{$ENDIF}
begin
  s := c_accept(listener, nil, nil);
  if s < 0 then
    exit(InvalidSocket);
{$IFDEF DARWIN}
  one := 1;
  c_setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, one, sizeof(one));
{$ENDIF}
  Accept := s;
end;

function Receive(s: TSocket; var buf; size: longint): longint;
begin
  Receive := longint(c_recv(s, buf, size, 0));
end;

function SendAll(s: TSocket; const data: string): boolean;
var
  sent, n: int64;
begin
  SendAll := false;
  sent := 0;
  while sent < length(data) do
  begin
    n := c_send(s, data[sent + 1], length(data) - sent, SendFlags);
    if n <= 0 then
      exit;
    sent := sent + n;
  end;
  SendAll := true;
end;

procedure CloseSocket(s: TSocket);
begin
  c_close(s);
end;

end.
