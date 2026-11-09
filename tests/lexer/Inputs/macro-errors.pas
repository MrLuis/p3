{$macro on}
{$define A := A + 1}
{$define S := 'x'}
{$define N := 4}
A
{$if S > 1}{$endif}
{$if N div 0 = 1}{$endif}
{$if 9223372036854775807 + 1 > 0}{$endif}
{$if N}{$endif}
{$if N +}{$endif}
{$if not N}{$endif}
{$macro maybe}
{$define X := Y}
{$define Y := X}
{$if X = 1}{$endif}
