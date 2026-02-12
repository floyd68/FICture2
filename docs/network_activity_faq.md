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

1. **Certificate Validation (CRL/OCSP)** - Most Common Cause
   - Windows validates digital signatures of:
     - System DLLs (WIC, DirectX, COM components)
     - The executable itself (code signing verification)
     - Trust chain validation for all loaded modules
   - This happens automatically via Windows Trust/Crypto API
   - **Developer cannot control this** - it's outside application code
   - Connections may go through CDNs (Google, Cloudflare, Microsoft)
   - **Especially common in automated scanning environments** (VirusTotal, NexusMods scanner)

2. **WIC (Windows Imaging Component) & DirectX**
   - FICture2 uses built-in Windows DLLs for image decoding
   - When loading these system components, Windows performs:
     - Digital signature validation
     - Certificate chain verification
     - CRL/OCSP checks (network required)
   - This is standard Windows behavior for any app using WIC/DirectX

3. **~~VC Runtime DLLs (vcruntime140.dll, msvcp140.dll)~~** *(Eliminated in v1.1+)*
   - ~~Windows validates Microsoft-signed DLLs~~
   - ~~May check certificate revocation lists~~
   - **Fixed:** FICture2 now uses static linking (`/MT`) to eliminate this dependency

**This is not FICture2-specific behavior** - any application using WIC, DirectX, or other Windows system components will trigger similar network activity through Windows security validation.

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

## Common Domains Contacted

Based on security scanner reports (e.g., NexusMods VirusTotal), these domains may be contacted:

**Microsoft Services:**
- `*.microsoft.com`, `*.windowsupdate.com`
- `*.update.microsoft.com`, `*.delivery.mp.microsoft.com`
- `smartscreen.microsoft.com`

**Google/CDN Services (Certificate Validation):**
- `optimizationguide-pa.googleapis.com`
- `redirector.gvt1.com`
- Other `*.googleapis.com` domains

**Why Google domains?**
- Windows certificate validation uses CDNs for CRL/OCSP checks
- Google/Cloudflare infrastructure is commonly used for certificate distribution
- **This does NOT mean FICture2 uses Chrome/Edge/WebView2**
- It means Windows is checking certificate validity through available CDN infrastructure

**Network Ranges:**
- 142.250.0.0/16 (Google CDN)
- AS15169 (Google), AS13335 (Cloudflare)

All connections originate from Windows system components, not FICture2 application code.

## Verification

You can verify FICture2's network activity using:
- **Process Monitor** (Sysinternals) - Most detailed
- **Resource Monitor** (built-in) - Network tab
- **Wireshark** - Full packet capture
- **netstat** - Command-line monitoring

See `test_network.bat` in the repository for automated testing scripts.

## For NexusMods Users

If you see network activity warnings from NexusMods virus scanner:

**This is normal and safe.** The scanner runs FICture2 in an automated sandboxed environment, which triggers Windows security validation:

**Primary Cause: Certificate Validation**
- Windows automatically validates digital signatures of all loaded system components
- Certificate revocation checks (CRL/OCSP) require network access
- These checks use CDN infrastructure (Google, Cloudflare, Microsoft)
- **This happens outside the application code** - controlled by Windows Trust/Crypto API
- Automated scanning environments trigger more aggressive validation than normal usage

**What you might see:**
- Google domains (`*.googleapis.com`, `redirector.gvt1.com`) - CDN infrastructure for certificate distribution
- Microsoft domains (`*.microsoft.com`, `*.windowsupdate.com`) - Direct certificate validation
- IP addresses in Google (AS15169) or Cloudflare (AS13335) ranges - CDN networks

**Why 0/92 virus detections?**
- These domains are legitimate certificate/security infrastructure
- No malware/virus scanners flag them as malicious
- Network activity is **initiated by Windows security validation**, not FICture2 code
- The scanner sees "network access" but it's actually Windows checking if the executable is safe to run

## Summary

- ✅ FICture2 code does **NOT** make network requests
- ✅ All network activity is from **Windows security validation** (certificate checks, CRL/OCSP)
- ✅ Windows validates system components (WIC, DirectX, COM) automatically
- ✅ Certificate validation uses CDN infrastructure (Google, Cloudflare, Microsoft)
- ✅ This is **standard behavior** for Windows applications
- ✅ Network activity can be reduced via Windows settings/policies
- ✅ **NexusMods virus scanner reports are false positives** - connections are Windows validating if the app is safe to run

---

*Last updated: 2026-02-08*
