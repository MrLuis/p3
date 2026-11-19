program table_fallback;
procedure ReRaise;
begin
  try
    raise Exception.Create('x')
  except
    raise
  end
end;
procedure HandlerRaise;
begin
  try
    raise Exception.Create('x')
  except
    raise Exception.Create('y')
  end
end;
procedure Nested;
begin
  try
    try
      raise Exception.Create('x')
    except
      writeln('inner')
    end
  except
    writeln('outer')
  end
end;
begin end.
