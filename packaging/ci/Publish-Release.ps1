# Publishes a tagged release from CI so that a failed run can be re-run:
#
# - A new release is created as a draft with the installer, gets its winget
#   manifests zip, and is published only then. Winget validation needs the
#   asset URL to be public, so submission happens after this.
# - On a re-run, an existing draft is resumed: the installer is replaced by
#   this build, the manifests zip is uploaded, and the draft is published.
# - A published release is never overwritten. Its manifests are generated from
#   the published installer's URL and SHA256 (a rebuild may differ), and only a
#   missing manifests zip is added.
#
# `gh` is reached through -Gh (the real CLI by default), so the tests can
# replace it.

function Publish-Release {
    param(
        [Parameter(Mandatory)][string]$Tag,
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$Installer,
        [Parameter(Mandatory)][string]$NotesFile,
        [Parameter(Mandatory)][string]$ManifestsZipName,
        # param([string]$Url, [string]$Sha256) -> path of the manifests zip
        [Parameter(Mandatory)][scriptblock]$NewManifests,
        [scriptblock]$Gh = { param([string[]]$GhArgs) & gh @GhArgs },
        [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) ("fic2-release-" + [guid]::NewGuid().ToString('N')))
    )

    $invoke = {
        param([string[]]$GhArgs, [switch]$AllowFailure)
        $global:LASTEXITCODE = 0
        $out = & $Gh $GhArgs
        if ($LASTEXITCODE -and -not $AllowFailure) {
            throw "gh $($GhArgs -join ' ') failed with exit code $LASTEXITCODE"
        }
        $out
    }

    $installerName = Split-Path $Installer -Leaf
    $view = & $invoke -GhArgs @('release', 'view', $Tag, '--repo', $Repository, '--json', 'isDraft,assets') -AllowFailure
    $exists = ($LASTEXITCODE -eq 0)
    $created = $false

    if (-not $exists) {
        & $invoke -GhArgs @('release', 'create', $Tag, $Installer, '--repo', $Repository, '--draft', '--verify-tag',
            '--title', "FICture2 $Tag", '--notes-file', $NotesFile) | Out-Null
        $isDraft = $true
        $assets = @($installerName)
        $created = $true
    }
    else {
        $state = ($view | Out-String) | ConvertFrom-Json
        $isDraft = [bool]$state.isDraft
        $assets = @($state.assets | ForEach-Object { $_.name })
    }

    if ($isDraft) {
        # Not public yet: this build's installer is the one that gets published.
        if (-not $created) {
            & $invoke -GhArgs @('release', 'upload', $Tag, $Installer, '--repo', $Repository, '--clobber') | Out-Null
        }
        $sha = (Get-FileHash $Installer -Algorithm SHA256).Hash
    }
    else {
        if ($assets -notcontains $installerName) {
            throw "Release $Tag is published without $installerName; it is not modified automatically."
        }
        New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
        & $invoke -GhArgs @('release', 'download', $Tag, '--repo', $Repository, '--pattern', $installerName,
            '--dir', $WorkDir, '--clobber') | Out-Null
        $sha = (Get-FileHash (Join-Path $WorkDir $installerName) -Algorithm SHA256).Hash
    }

    $url = "https://github.com/$Repository/releases/download/$Tag/$installerName"
    $zip = & $NewManifests $url $sha
    $uploadedManifests = $false
    if ($isDraft) {
        & $invoke -GhArgs @('release', 'upload', $Tag, $zip, '--repo', $Repository, '--clobber') | Out-Null
        $uploadedManifests = $true
    }
    elseif ($assets -notcontains $ManifestsZipName) {
        & $invoke -GhArgs @('release', 'upload', $Tag, $zip, '--repo', $Repository) | Out-Null
        $uploadedManifests = $true
    }

    $published = $false
    if ($isDraft) {
        & $invoke -GhArgs @('release', 'edit', $Tag, '--repo', $Repository, '--draft=false', '--latest') | Out-Null
        $published = $true
    }

    [pscustomobject]@{
        Url = $url
        Sha256 = $sha
        Created = $created
        PublishedNow = $published
        UploadedManifests = $uploadedManifests
    }
}
