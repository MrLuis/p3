{$if sizeof(char) = 1} one {$endif}
{$if SizeOf ( CHAR ) = 2} two {$else} notwo {$endif}
{$if sizeof(longint) * 2 = sizeof(int64)} arith {$endif}
{$if false}{$if sizeof(nosuchtype) = 1}{$endif}{$endif}
{$macro on}
{$define sizeof := 7}
{$if sizeof = 7} macro {$endif}
{$undef sizeof}
{$if sizeof(word) = 2} again {$endif}
