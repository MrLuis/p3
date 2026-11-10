program errors;
type
  TV = record
    case boolean of
      true: (a: ansistring);
      false: (b: longint);
  end;
  TF = file of ansistring;
  TR = record s: ansistring; end;

procedure pv(var s: ansistring); begin end;
procedure pc(const s: ansistring); begin s := 'x'; s[1] := 'y'; setlength(s, 2); end;
var
  a: ansistring;
  ss: shortstring;
  n: longint;
  r: real;
begin
  pv(ss);
  n := a;
  a := n;
  a := r;
  setlength(n, 1);
  setlength(a);
  setlength(a, 'x');
  a['x'] := 'y';
  writeln(high(a));
  n := longint(a);
end.
