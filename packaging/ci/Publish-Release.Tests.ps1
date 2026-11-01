# Mocked tests for Publish-Release.ps1 (no network, no real gh):
#   pwsh -NoProfile -File packaging/ci/Publish-Release.Tests.ps1
# Exits non-zero when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Publish-Release.ps1')

$script:failures = 0
function Check([bool]$condition, [string]$what) {
    if ($condition) { Write-Host "  ok   $what" }
    else { Write-Host "  FAIL $what"; $script:failures++ }
}

# A fake GitHub release store and gh CLI over it.
function New-FakeGh($store) {
    {
        param([string[]]$a)
        $global:LASTEXITCODE = 0
        $store.calls.Add(($a[0..1] -join ' '))
        $tag = $a[2]
        switch ($a[1]) {
            'view' {
                if (-not $store.exists) { $global:LASTEXITCODE = 1; return }
                $assets = @($store.assets.Keys | ForEach-Object { @{ name = $_ } })
                return (@{ isDraft = $store.isDraft; assets = $assets } | ConvertTo-Json -Depth 4)
            }
            'create' {
                if ($store.exists) { $global:LASTEXITCODE = 1; return }
                $store.exists = $true
                $store.isDraft = ($a -contains '--draft')
                $store.assets[(Split-Path $a[3] -Leaf)] = [IO.File]::ReadAllText($a[3])
            }
            'upload' {
                $name = Split-Path $a[3] -Leaf
                if ($store.assets.ContainsKey($name) -and ($a -notcontains '--clobber')) { $global:LASTEXITCODE = 1; return }
                $store.assets[$name] = [IO.File]::ReadAllText($a[3])
            }
            'download' {
                $name = $a[$a.IndexOf('--pattern') + 1]; $dir = $a[$a.IndexOf('--dir') + 1]
                [IO.File]::WriteAllText((Join-Path $dir $name), $store.assets[$name])
            }
            'edit' { if ($a -contains '--draft=false') { $store.isDraft = $false } }
            default { $global:LASTEXITCODE = 2 }
        }
    }.GetNewClosure()
}

function New-Store { @{ exists = $false; isDraft = $false; assets = @{}; calls = New-Object System.Collections.Generic.List[string] } }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('fic2-publish-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $notes = Join-Path $tmp 'notes.md'; Set-Content $notes 'notes'
    $buildA = Join-Path $tmp 'a'; $buildB = Join-Path $tmp 'b'
    New-Item -ItemType Directory -Path $buildA, $buildB | Out-Null
    $installerA = Join-Path $buildA 'FICture2-Setup-1.7.exe'; [IO.File]::WriteAllText($installerA, 'installer from build A')
    $installerB = Join-Path $buildB 'FICture2-Setup-1.7.exe'; [IO.File]::WriteAllText($installerB, 'installer from build B')
    $shaA = (Get-FileHash $installerA -Algorithm SHA256).Hash
    $shaB = (Get-FileHash $installerB -Algorithm SHA256).Hash
    $zipName = 'winget-manifests-1.7.224.0.zip'
    $script:manifestCalls = New-Object System.Collections.Generic.List[string]
    $manifests = {
        param($url, $sha)
        $script:manifestCalls.Add($sha)
        $zip = Join-Path $tmp $zipName
        [IO.File]::WriteAllText($zip, "manifests for $sha at $url")
        $zip
    }
    $common = @{ Tag = 'v1.7.224.0'; Repository = 'owner/repo'; NotesFile = $notes; ManifestsZipName = $zipName }

    Write-Host 'Fresh release: draft, assets, then published'
    $store = New-Store
    $r = Publish-Release @common -Installer $installerA -NewManifests $manifests -Gh (New-FakeGh $store) -WorkDir (Join-Path $tmp 'w1')
    Check ($store.exists -and -not $store.isDraft) 'release is published at the end'
    Check ($store.calls[1] -eq 'release create') 'created after the view'
    Check ((@($store.calls) -join ',') -match 'create.*upload.*edit') 'created as a draft, manifests uploaded, then published'
    Check ($store.assets.ContainsKey('FICture2-Setup-1.7.exe') -and $store.assets.ContainsKey($zipName)) 'installer and manifests attached'
    Check ($r.Sha256 -eq $shaA -and $r.Created -and $r.PublishedNow) 'hash of this build; created and published now'
    Check ($r.Url -eq 'https://github.com/owner/repo/releases/download/v1.7.224.0/FICture2-Setup-1.7.exe') 'public asset URL'

    Write-Host 'Failure after the release was created, then a re-run'
    $store = New-Store
    $failing = { param($url, $sha) throw 'manifest generation failed' }
    $threw = $false
    try { Publish-Release @common -Installer $installerA -NewManifests $failing -Gh (New-FakeGh $store) -WorkDir (Join-Path $tmp 'w2') | Out-Null } catch { $threw = $true }
    Check $threw 'the first run fails'
    Check ($store.exists -and $store.isDraft) 'the release stays a draft (nothing public)'
    Check (-not $store.assets.ContainsKey($zipName)) 'no manifests yet'
    $store.calls.Clear()
    $r = Publish-Release @common -Installer $installerB -NewManifests $manifests -Gh (New-FakeGh $store) -WorkDir (Join-Path $tmp 'w3')
    Check (-not ($store.calls -contains 'release create')) 'the re-run does not create again'
    Check (-not $store.isDraft) 'the re-run publishes the draft'
    Check ($store.assets['FICture2-Setup-1.7.exe'] -eq 'installer from build B') 'the draft gets the re-run build installer'
    Check ($r.Sha256 -eq $shaB -and $store.assets[$zipName] -match $shaB) 'manifests match the published installer'

    Write-Host 'Published release without manifests: only the zip is added'
    $store = New-Store
    $store.exists = $true; $store.isDraft = $false
    $store.assets['FICture2-Setup-1.7.exe'] = 'installer from build A'
    $r = Publish-Release @common -Installer $installerB -NewManifests $manifests -Gh (New-FakeGh $store) -WorkDir (Join-Path $tmp 'w4')
    Check ($store.assets['FICture2-Setup-1.7.exe'] -eq 'installer from build A') 'the published installer is not replaced'
    Check ($r.Sha256 -eq $shaA) 'hash of the published installer, not of the rebuild'
    Check ($store.assets[$zipName] -match $shaA) 'manifests use the published hash'
    Check (-not ($store.calls -contains 'release edit') -and -not ($store.calls -contains 'release create')) 'no create or edit'
    Check ($r.UploadedManifests -and -not $r.PublishedNow) 'only the manifests were added'

    Write-Host 'Published release that is complete: nothing changes'
    $store = New-Store
    $store.exists = $true; $store.isDraft = $false
    $store.assets['FICture2-Setup-1.7.exe'] = 'installer from build A'
    $store.assets[$zipName] = 'existing manifests'
    $r = Publish-Release @common -Installer $installerB -NewManifests $manifests -Gh (New-FakeGh $store) -WorkDir (Join-Path $tmp 'w5')
    Check ($store.assets[$zipName] -eq 'existing manifests') 'the attached manifests are kept'
    Check (-not ($store.calls | Where-Object { $_ -in 'release upload', 'release edit', 'release create' })) 'no upload, edit or create'
    Check ($r.Sha256 -eq $shaA) 'local manifests are generated from the published installer (for winget)'

    Write-Host 'Published release without its installer: refused'
    $store = New-Store
    $store.exists = $true; $store.isDraft = $false
    $threw = $false
    try { Publish-Release @common -Installer $installerB -NewManifests $manifests -Gh (New-FakeGh $store) -WorkDir (Join-Path $tmp 'w6') | Out-Null } catch { $threw = $true }
    Check $threw 'stops instead of modifying the published release'
    Check (-not ($store.calls | Where-Object { $_ -in 'release upload', 'release edit', 'release create' })) 'nothing changed'
}
finally {
    Remove-Item -Recurse -Force $tmp
}

if ($script:failures) { Write-Host "$script:failures check(s) failed"; exit 1 }
Write-Host 'All checks passed'
