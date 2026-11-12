program objfpcfiles;
var
  t, o: text;
  s: string;
begin
  AssignFile(t, paramstr(1));
  Rewrite(t);
  WriteLn(t, 'via AssignFile');
  CloseFile(t);
  AssignFile(t, paramstr(1));
  Reset(t);
  ReadLn(t, s);
  CloseFile(t);
  WriteLn(s);
  Assign(o, '');
  Rewrite(o);
  WriteLn(o, 'to output');
  Close(o);
end.
