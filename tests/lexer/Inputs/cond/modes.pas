{$ifdef PCC} a {$endif}
{$ifdef PCC_ISO} iso {$endif}
{$ifdef PCC_TP} tp {$endif}
{$ifdef PCC_OBJFPC} objfpc {$endif}
{$ifdef CMDLINE} cmdline {$endif}
{$if defined(pcc) and not defined(Cmdline)} nocmdline {$ifend}
{$ifdef never} x { y } (* z *) 'w {$endif} '
{$endif}
{$ifdef never} x (* {$endif} *) y {$endif} z
