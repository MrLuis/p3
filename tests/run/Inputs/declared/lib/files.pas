unit files;
interface
uses paths;
type
  TFilenameCaseMatch = (mkNone, mkExactMatch, mkSingleMatch, mkAmbiguous);
function Describe: string;
implementation
{ the used unit's names are seen in a unit too }
{$if declared(PathDelim) and declared(paths)}
function Describe: string; begin Describe := 'with ' + PathDelim end;
{$else}
function Describe: string; begin Describe := 'without' end;
{$endif}
end.
