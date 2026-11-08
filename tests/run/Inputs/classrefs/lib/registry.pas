unit registry;
{ A class with a class variable, class methods and a virtual constructor;
  descendants are created through class references. }
interface
type
  TPlugin = class
  public
    class var Loaded: integer;
    Tag: string;
    constructor Create(const t: string); virtual;
    class function Title: string; virtual;
    class function Load(const t: string): TPlugin;
  end;
  TPluginClass = class of TPlugin;
implementation
constructor TPlugin.Create(const t: string);
begin
  Tag := t;
  Loaded := Loaded + 1;
end;
class function TPlugin.Title: string;
begin
  Title := 'plugin';
end;
class function TPlugin.Load(const t: string): TPlugin;
begin
  Load := Create(t + '@' + Title);
end;
end.
