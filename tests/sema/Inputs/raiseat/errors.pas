program errors;
var
  I: Integer;
  R: Real;
  S: string;
  P: ^Integer;
begin
  raise Exception.Create('x') at I;
  raise Exception.Create('x') at nil, R;
  raise Exception.Create('x') at S, 'abc';
  raise Exception.Create('x') at P, P;
  I := ExceptAddr;
  P := ExceptAddr(1);
end.
