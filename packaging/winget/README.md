# FICture2 Winget Packaging Guide

This guide describes the process for packaging and submitting FICture2 to the Windows Package Manager (winget).

## Prerequisites

### Required Software
- **Visual Studio 2022** with C++ desktop development
- **CMake** 3.21 or later
- **Inno Setup 6** - [Download](https://jrsoftware.org/isdl.php)
- **Git** for version control
- **GitHub CLI (`gh`)** - [Download](https://cli.github.com/) (recommended)

### GitHub Setup
1. Fork [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)
2. Configure GitHub CLI: `gh auth login`

## Workflow Overview

```
┌─────────────────┐
│ 1. Build        │  build_winget.bat
│    & Test       │  → Manual smoke test
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 2. Package      │  pack_winget.bat
│    & Verify     │  → Manual install test
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 3. Release      │  release_winget.bat
│    to GitHub    │  → Creates tag & release
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 4. Submit to    │  submit_winget.bat
│    Winget       │  → Creates PR
└─────────────────┘
```

## Detailed Steps

### Step 1: Build & Test

```cmd
build_winget.bat
```

**What it does:**
- Creates `build_winget` directory
- Configures CMake with `FICTURE2_DISTRIBUTION_CHANNEL=winget`
- Builds MinSizeRel configuration
- Generates Inno Setup script

**Manual verification:**
- Run `build_winget\bin\MinSizeRel\FICture2.exe`
- Test basic functionality:
  - Open various image formats
  - Navigate between images
  - Verify no crashes or errors
  - Check version in About dialog

### Step 2: Package & Test Installation

```cmd
pack_winget.bat
```

**What it does:**
- Runs Inno Setup Compiler
- Generates installer in `build_winget\Output\`
- Calculates SHA256 hash
- Saves installer info to `installer_info.txt`

**Manual verification:**
- Run the generated installer
- Verify installation directory
- Test file associations (optional)
- Test thumbnail provider (optional)
- Uninstall and verify clean removal

**Common issues:**
- If Inno Setup Compiler not found, install from [jrsoftware.org](https://jrsoftware.org/isdl.php)
- Check that all required DLLs are included

### Step 3: Release to GitHub

```cmd
release_winget.bat
```

**What it does:**
- Extracts version from CMake cache
- Checks for uncommitted changes
- Creates git tag `v{VERSION}`
- Pushes code and tag to GitHub
- Creates GitHub release with installer
- Updates CMake configuration with release URL and SHA256
- Generates winget manifest files

**Prerequisites:**
- GitHub CLI authenticated: `gh auth login`
- Clean git working directory (or accept warning)

**Output:**
- GitHub release with installer attached
- Three manifest files in `build_winget\winget_manifests\`:
  - `floyd68.FICture2.yaml` (version manifest)
  - `floyd68.FICture2.installer.yaml` (installer details)
  - `floyd68.FICture2.locale.en-US.yaml` (metadata)

### Step 4: Submit to Winget

```cmd
submit_winget.bat
```

**What it does:**
- Validates manifest files using `winget validate`
- Clones your winget-pkgs fork (if needed)
- Syncs with upstream microsoft/winget-pkgs
- Creates a new branch for the submission
- Copies manifest files to correct location
- Commits changes
- Pushes branch and creates PR

**First-time setup:**
- Script will prompt for your GitHub username
- Clones your winget-pkgs fork

**Manual PR submission:**
If you prefer manual submission or if `gh` CLI is not available:

1. Copy manifests from `build_winget\winget_manifests\` to:
   ```
   winget-pkgs/manifests/f/floyd68/FICture2/{VERSION}/
   ```

2. Create PR with title:
   ```
   New version: floyd68.FICture2 version {VERSION}
   ```

3. In PR description include:
   ```
   - [x] Tested installation
   - [x] Tested uninstallation
   - Link: https://github.com/floyd68/FICture2/releases/tag/v{VERSION}
   ```

## Winget Manifest Structure

FICture2 uses the 3-file manifest structure:

### 1. Version Manifest (`floyd68.FICture2.yaml`)
- Links the other two manifests
- Minimal file

### 2. Installer Manifest (`floyd68.FICture2.installer.yaml`)
- Installer URL and SHA256
- Supported architectures (x64)
- File extensions
- Install switches
- Silent install options

### 3. Locale Manifest (`floyd68.FICture2.locale.en-US.yaml`)
- Package description
- Publisher information
- Tags and keywords
- License information
- URLs (package, support, etc.)

## Troubleshooting

### Manifest Validation Fails

```cmd
winget validate build_winget\winget_manifests
```

Common issues:
- Invalid YAML syntax (use YAML validator)
- Missing required fields
- Invalid URL or SHA256
- Version format mismatch

### GitHub Release Creation Fails

Check:
- GitHub CLI authenticated: `gh auth status`
- Tag doesn't already exist: `git tag -l`
- Repository permissions

### PR Rejected by Winget

Common reasons:
- Manifest validation errors
- Installer not accessible
- SHA256 mismatch
- SmartScreen warnings (consider code signing)

## Best Practices

1. **Version Numbering**
   - Use semantic versioning: `MAJOR.MINOR.PATCH.BUILD`
   - Don't skip versions

2. **Testing**
   - Always test installer manually before release
   - Test on clean Windows installation if possible
   - Verify both installation and uninstallation

3. **Release Notes**
   - Keep GitHub release notes up to date
   - Document breaking changes

4. **Code Signing** (Recommended)
   - Reduces SmartScreen warnings
   - Increases user trust
   - Required by some organizations

## Automation Options

For future automation, consider:
- GitHub Actions workflow for building
- Automated smoke tests
- Automatic PR creation
- Integration with CI/CD pipeline

## References

- [Winget Documentation](https://docs.microsoft.com/en-us/windows/package-manager/)
- [Manifest Schema](https://github.com/microsoft/winget-pkgs/tree/master/doc/manifest)
- [Contribution Guidelines](https://github.com/microsoft/winget-pkgs/blob/master/CONTRIBUTING.md)
- [Inno Setup Documentation](https://jrsoftware.org/ishelp/)

## Support

For issues with:
- **FICture2**: [GitHub Issues](https://github.com/floyd68/FICture2/issues)
- **Winget submission**: [winget-pkgs Issues](https://github.com/microsoft/winget-pkgs/issues)
