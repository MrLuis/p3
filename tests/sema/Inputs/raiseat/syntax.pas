program syntax;
begin
  raise Exception.Create('x') at;
  raise Exception.Create('x') at nil,;
end.
