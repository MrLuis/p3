{ async routines and methods of a unit, used by main.pas }
unit tasks;

{$H+}

interface

type
  TBox = class
    V: Integer;
    function Get(d: Integer): async Integer;
    class function Make(n: string): async string;
  end;

function Twice(x: Integer): async Integer;
function Greet(const s: string): async string;
procedure Note(const s: string): async;

implementation

function TBox.Get(d: Integer): async Integer;
begin
  Result := V + d;
end;

class function TBox.Make(n: string): async string;
begin
  Result := n + '!';
end;

function Twice(x: Integer): async Integer;
begin
  Result := 2 * x;
end;

function Greet(const s: string): async string;
begin
  Result := 'hi ' + s;
end;

procedure Note(const s: string): async;
begin
  Writeln('note: ', s);
end;

end.
