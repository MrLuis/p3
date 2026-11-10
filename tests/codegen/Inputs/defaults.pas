program defaults;
procedure P(a: longint; b: longint = 7; const c: string = 'x');
begin
end;
begin
  P(1);
  P(1, 2);
end.
