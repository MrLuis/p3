program ast;
procedure Connect(Host: string; Port: Integer = 5432; Timeout: Integer = 30;
                  UseTls: Boolean = True);
begin
end;
begin
  Connect('a', Timeout := 5);
  Connect(UseTls := False, Host := 'b');
end.
