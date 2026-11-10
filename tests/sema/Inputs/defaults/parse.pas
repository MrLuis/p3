program parse;
procedure B1(a: longint = 1; b: longint); begin end;
procedure B2(a: longint = 1; procedure b); begin end;
procedure B3(a, b: longint = 1); begin end;
procedure B4(var a: longint = 1); begin end;
procedure B5(const a = 1); begin end;
begin
end.
