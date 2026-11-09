{$macro on}
{$define PAIR := 1, 2}
{$define WORD := hello}
{$define TEXT := 'text'}
a PAIR b
{$if TEXT = 'text'} quoted {$endif}
{$if WORD = 'hello'} plain {$endif}
{$if 'abc' < 'abd'} ordered {$endif}
{$if 'pc' + 'c' = 'pcc'} concat {$endif}
{$if 'b' in ['a'..'c']} range {$endif}
{$macro off}
WORD
