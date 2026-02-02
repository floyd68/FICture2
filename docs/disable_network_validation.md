# Disabling Network Validation (Advanced Users)

## ⚠️ Warning

Disabling certificate validation and network checks **reduces system security**. Only do this if:
- You're in an air-gapped/isolated environment
- You understand the security implications
- You've exhausted other options

## Why This Is Needed

Windows automatically validates digital signatures of:
- System DLLs (WIC, DirectX, Common Controls)
- COM components
- Application executables

This validation requires network access to:
- CRL (Certificate Revocation List) servers
- OCSP (Online Certificate Status Protocol) responders
- Microsoft Update servers

## Methods to Reduce Network Activity

### Method 1: Group Policy (Windows Pro/Enterprise Only)

#### Disable Certificate Revocation Checking

```
1. Open: gpedit.msc
2. Navigate to:
   Computer Configuration > Windows Settings > Security Settings > Public Key Policies
3. Right-click "Certificate Path Validation Settings" > Properties
4. Check "Define these policy settings"
5. Uncheck "Automatically update certificates in the Microsoft Root Certificate Program (recommended)"
6. Under "Certificate revocation", select "Disable"
7. Click OK
8. Run: gpupdate /force
9. Restart computer
```

#### Disable Windows Update for System Components

```
1. Open: gpedit.msc
2. Navigate to:
   Computer Configuration > Administrative Templates > Windows Components > Windows Update
3. Configure:
   - "Configure Automatic Updates" → Disabled
   - "Do not connect to any Windows Update Internet locations" → Enabled
4. Run: gpupdate /force
```

### Method 2: Registry (All Windows Versions)

**⚠️ Backup your registry before making changes!**

#### Disable CRL Checking (System-Wide)

Create a `.reg` file:

```reg
Windows Registry Editor Version 5.00

; Disable CRL checking for CAPI2
[HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Cryptography\OID\EncodingType 0\CertDllOpenStoreProv\CertNet]
"DisableRevocationChecking"=dword:00000001

; Disable automatic root updates
[HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\SystemCertificates\AuthRoot]
"DisableRootAutoUpdate"=dword:00000001

; Disable Windows Update CTL auto-update
[HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\SystemCertificates\TrustedPublisher]
"TreatRemovedServersAsUntrusted"=dword:00000000
```

Save as `disable_cert_validation.reg` and run as Administrator.

#### Disable Windows Defender Cloud Protection

```reg
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Windows Defender\Spynet]
"SpynetReporting"=dword:00000000
"SubmitSamplesConsent"=dword:00000002

[HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection]
"DisableBehaviorMonitoring"=dword:00000001
"DisableOnAccessProtection"=dword:00000001
"DisableScanOnRealtimeEnable"=dword:00000001
```

### Method 3: Hosts File Redirection (Workaround)

Redirect Microsoft CRL/OCSP servers to localhost to prevent network delays:

Edit `C:\Windows\System32\drivers\etc\hosts` as Administrator:

```
# Block Microsoft certificate validation endpoints
127.0.0.1 crl.microsoft.com
127.0.0.1 ctldl.windowsupdate.com
127.0.0.1 ocsp.digicert.com
127.0.0.1 ocsp.msocsp.com
127.0.0.1 www.download.windowsupdate.com
127.0.0.1 go.microsoft.com
127.0.0.1 fe2.update.microsoft.com
127.0.0.1 fe3.delivery.mp.microsoft.com
```

**Note:** This doesn't prevent attempts, just makes them fail fast (no timeout).

### Method 4: Windows Firewall Rules

Block outbound connections from system processes:

```powershell
# Run PowerShell as Administrator

# Block CRL/OCSP checking
New-NetFirewallRule -DisplayName "Block CRL Checks" -Direction Outbound -Action Block -RemotePort 80,443 -Program "C:\Windows\System32\cryptsp.dll" -ErrorAction SilentlyContinue

# Block Windows Update (affects system DLL validation)
New-NetFirewallRule -DisplayName "Block Windows Update" -Direction Outbound -Action Block -RemoteAddress WindowsUpdateServers -ErrorAction SilentlyContinue
```

**Warning:** This may break Windows Update entirely.

### Method 5: Application-Specific (FICture2)

Since FICture2 v1.1 uses static linking (`/MT`), it already eliminates VC Runtime DLL validation.

To further reduce validation, run from a **trusted location**:
- `C:\Program Files\FICture2\` (system recognizes as trusted)
- Avoid running from Downloads, Desktop, or removable drives

### Method 6: Offline Mode (Nuclear Option)

Disconnect from the internet entirely:
1. Disable network adapter
2. Run FICture2
3. Windows will cache validation failures
4. Subsequent runs will be faster (cached negative responses)

## Verification

After applying changes, test with:

```batch
# Run FICture2
FICture2.exe

# Monitor network in another terminal
netstat -ano | findstr FICture2

# Should see minimal or no connections
```

## Rollback

To re-enable security features:

### Registry Rollback

```reg
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Cryptography\OID\EncodingType 0\CertDllOpenStoreProv\CertNet]
"DisableRevocationChecking"=-

[HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\SystemCertificates\AuthRoot]
"DisableRootAutoUpdate"=-
```

### Group Policy Rollback

Set all policies back to "Not Configured".

### Firewall Rollback

```powershell
Remove-NetFirewallRule -DisplayName "Block CRL Checks"
Remove-NetFirewallRule -DisplayName "Block Windows Update"
```

## Impact Summary

| Method | Security Impact | Effectiveness | Reversibility |
|--------|----------------|---------------|---------------|
| Group Policy | Medium | High | Easy |
| Registry | Medium-High | High | Easy |
| Hosts File | Low | Medium | Easy |
| Firewall | High | Very High | Easy |
| Offline Mode | Extreme | Complete | Instant |

## Recommended Approach

For normal users: **Don't disable anything**. The network activity is minimal and standard.

For air-gapped systems:
1. Install FICture2 while connected (one-time)
2. Disconnect network
3. Use normally

For paranoid users:
1. Use Method 1 (Group Policy) OR Method 2 (Registry)
2. Keep Windows Defender enabled
3. Monitor with `test_network.bat`

## Additional Notes

- **SmartScreen**: Cannot be disabled via manifest, only system settings
- **Common Controls v6**: Required for modern UI, cannot be removed
- **WIC/DirectX**: System components, validation is unavoidable

---

*Last updated: 2026-01-28*

**Disclaimer:** These methods affect system-wide security. Use at your own risk.
