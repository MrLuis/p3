unit greeting;
interface
type
  TGreeter = class
  private
    FWord: string;
    FCount: Integer;
  public
    constructor Create(const AWord: string);
    function Greet(const Name: string): string;
    property Count: Integer read FCount;
  end;
implementation
constructor TGreeter.Create(const AWord: string);
begin
  FWord := AWord;
  FCount := 0;
end;

function TGreeter.Greet(const Name: string): string;
begin
  FCount := FCount + 1;
  Greet := FWord + ', ' + Name + '!';
end;
end.
