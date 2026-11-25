$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
if (-not (Test-Path (Join-Path $repo 'tools\pack_build.py'))) { throw "Expected open-field repository at: $repo" }
$pack = Join-Path $repo 'analogbeast.pack'
Push-Location $repo
try {
    python -I tools/pack_build.py card --recipe analogbeast_field/recipe.json --out analogbeast_field/analogbeast_field_card.s
    if ($LASTEXITCODE -ne 0) { throw 'card generation failed' }
    python -I tools/pack_build.py build3 --recipe analogbeast_field/recipe.json --profile profiles/op1f-1.7.3.json --out analogbeast.pack
    if ($LASTEXITCODE -ne 0) { throw 'build3 failed' }
    python -I tools/pack_build.py check --pack analogbeast.pack
    if ($LASTEXITCODE -ne 0) { throw 'pack check failed' }
    Write-Host "OK: $pack" -ForegroundColor Green
} finally { Pop-Location }
