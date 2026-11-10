unit getter;
{$H+}
interface
type
  TC = class
    procedure Get(out s: ansistring);
  end;
procedure Fill(out s: ansistring; out n: longint);
implementation
procedure TC.Get(out s: ansistring);
begin
  s := s + 'method';
end;
procedure Fill(out s: ansistring; out n: longint);
begin
  s := s + 'filled';
  n := length(s);
end;
end.
