program host;
uses registry;
type
  TAudio = class(TPlugin)
    constructor Create(const t: string); override;
    class function Title: string; override;
  end;
constructor TAudio.Create(const t: string);
begin
  inherited Create(t + '!');
end;
class function TAudio.Title: string;
begin
  Title := 'audio';
end;
var
  kinds: array[1..2] of TPluginClass;
  p: TPlugin;
  i: integer;
begin
  kinds[1] := TPlugin;
  kinds[2] := TAudio;
  for i := 1 to 2 do
  begin
    p := kinds[i].Load('p' + chr(ord('0') + i));
    writeln(p.ClassName, ' ', p.Tag, ' ', kinds[i].Title);
    p.Free;
  end;
  writeln('loaded ', TPlugin.Loaded, ' ', TAudio.Loaded);
end.
