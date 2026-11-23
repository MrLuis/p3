unit canadatypes;
{ canada.json as records, for the typed benchmark (json generator). }
{$H+}
{$modeswitch advancedrecords}
interface

uses json;

type
  [Json]
  TProperties = partial record
    name: string;
  end;

  [Json]
  TGeometry = partial record
    [JsonName('type')] Kind: string;
    coordinates: array of array of array of Double;
  end;

  [Json]
  TFeature = partial record
    [JsonName('type')] Kind: string;
    properties: TProperties;
    geometry: TGeometry;
  end;

  [Json]
  TFeatureCollection = partial record
    [JsonName('type')] Kind: string;
    features: array of TFeature;
  end;

implementation

end.
