# smm2-dx Build Helper: .\build-helper.ps1 [-Package] [-Clean] [-Run] [-Jobs n] ... (all options: recomp\build-helper.ps1)
# Needs only Windows. Builds the app into dist\.
& (Join-Path $PSScriptRoot 'recomp\build-helper.ps1') @args
exit $LASTEXITCODE
