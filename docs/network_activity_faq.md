# Network Activity FAQ

## Does FICture2 connect to the internet?

**FICture2 itself does NOT initiate any network connections.**

However, Windows may make network requests when FICture2 is running due to:

### Windows System Services

1. **Certificate Validation (CRL/OCSP)**
   - Windows validates digital signatures of system components (WIC, COM, etc.)
   - This requires checking certificate revocation lists from Microsoft servers
   - Domains: `*.microsoft.com`, `*.windowsupdate.com`

2. **SmartScreen Filter**
   - Windows checks application reputation
   - Connections to `smartscreen.microsoft.com`

3. **Windows Defender Cloud Protection**
   - Real-time protection may check files against cloud database
   - Connections to Microsoft security services

4. **Windows Update / Telemetry**
   - Windows may check for component updates
   - Domains: `*.delivery.mp.microsoft.com`, `*.update.microsoft.com`

## Why does this happen?

FICture2 uses **Windows Imaging Component (WIC)**, a built-in Windows COM component for image processing. When any application loads Windows system components, Windows performs security checks that may require internet connectivity.

### Main Sources of Network Activity

1. **WIC (Windows Imaging Component)**
   - Built-in Windows DLLs for image decoding
   - Windows validates digital signatures → CRL/OCSP checks
   - DirectX components may trigger similar checks

2. **~~VC Runtime DLLs (vcruntime140.dll, msvcp140.dll)~~** *(Eliminated in v1.1+)*
   - ~~Windows validates Microsoft-signed DLLs~~
   - ~~May check certificate revocation lists~~
   - **Fixed:** FICture2 now uses static linking (`/MT`) to eliminate this dependency

**This is not FICture2-specific behavior** - any application using WIC, DirectX, or other Windows components will trigger similar network activity.

## How to disable these connections?

These connections are controlled by Windows system policies, not by FICture2. 

### ⚠️ Quick Summary

- **For normal users:** Don't disable. Network activity is minimal and standard.
- **For air-gapped systems:** Install while connected, then disconnect.
- **For advanced users:** See detailed guide below.

### 🔧 Available Methods

**Basic (Windows Settings):**
- Privacy Settings: Disable SmartScreen, telemetry
- Windows Update: Pause updates
- Windows Defender: Disable cloud protection

**Advanced (System-Wide):**
- Group Policy: Disable certificate revocation checking
- Registry: Disable CRL/OCSP validation
- Hosts File: Redirect Microsoft servers to localhost
- Firewall: Block outbound connections to validation servers

### 📘 Detailed Guide

For comprehensive instructions on each method, including:
- Step-by-step configuration
- Registry scripts
- PowerShell commands
- Security implications
- Rollback procedures

See: **[docs/disable_network_validation.md](disable_network_validation.md)**

**⚠️ Warning:** Disabling validation features reduces system security. Only recommended for:
- Air-gapped/isolated environments
- Advanced users who understand the risks
- Testing/debugging purposes

## Verification

You can verify FICture2's network activity using:
- **Process Monitor** (Sysinternals) - Most detailed
- **Resource Monitor** (built-in) - Network tab
- **Wireshark** - Full packet capture
- **netstat** - Command-line monitoring

See `test_network.bat` in the repository for automated testing scripts.

## Summary

- ✅ FICture2 code does **NOT** make network requests
- ✅ All network activity is from **Windows system services**
- ✅ This is **standard behavior** for Windows applications
- ✅ Network activity can be reduced via Windows settings/policies

---

*Last updated: 2026-01-28*
