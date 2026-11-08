program errors;
{$else}
{$if foo}
{$endif}
{$if defined(A) and}
{$endif}
{$define}
{$define x := 3}
{$ifopt R+}{$endif}
{$ifdef A}
{$else}
{$else}
{$elseif true}
{$endif}
{$ifndef A}
{$elseif true}
{$endif}
{$ifdef A}
  {$if bad expression in a skipped branch is not checked}{$endif}
  {$define in a skipped branch: not checked either}
{$endif}
{$undef x}{$undef never_defined}
begin
end.
{$ifdef ZZ}
