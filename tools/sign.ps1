# Signs a built executable with a self-signed code-signing certificate.
#
# The certificate lives in the current user's personal store under the subject
# below. It is created on first use and reused afterwards, so every build is
# signed by the same identity. Nothing is written to the repository.
#
#   sign.ps1 -Path build\RearViewMirror.exe        sign one file
#   sign.ps1 -Trust                                 also trust the certificate
#                                                   on this machine (optional;
#                                                   makes Windows show it as a
#                                                   verified publisher)
#   sign.ps1 -Remove                                delete the certificate

[CmdletBinding()]
param(
    [string]$Path,
    [switch]$Trust,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$Subject = 'CN=Rear View Mirror'
$FriendlyName = 'Rear View Mirror code signing'

function Find-SignTool {
    if ($env:WindowsSdkVerBinPath) {
        $p = Join-Path $env:WindowsSdkVerBinPath 'x64\signtool.exe'
        if (Test-Path $p) { return $p }
    }
    $kits = 'C:\Program Files (x86)\Windows Kits\10\bin'
    $found = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\x64\signtool.exe' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($found) { return $found.FullName }
    throw 'signtool.exe not found; install the Windows SDK.'
}

function Find-SigningCert {
    $certs = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(1) } |
        Sort-Object NotAfter -Descending
    if ($certs) { return $certs[0] }
    return $null
}

function Get-SigningCert {
    $cert = Find-SigningCert
    if ($cert) { return $cert }

    # Parallel build steps must not each create one.
    $mutex = New-Object System.Threading.Mutex($false, 'Local\RearViewMirrorSigning')
    [void]$mutex.WaitOne()
    try {
        $cert = Find-SigningCert
        if ($cert) { return $cert }
        Write-Host "Creating self-signed code-signing certificate '$Subject' in CurrentUser\My"
        return New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $Subject `
            -FriendlyName $FriendlyName `
            -KeyAlgorithm RSA -KeyLength 3072 `
            -HashAlgorithm SHA256 `
            -KeyUsage DigitalSignature `
            -KeyExportPolicy NonExportable `
            -TextExtension @('2.5.29.19={critical}{text}ca=0') `
            -CertStoreLocation Cert:\CurrentUser\My `
            -NotAfter (Get-Date).AddYears(10)
    } finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

if ($Remove) {
    $removed = 0
    foreach ($store in 'My', 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\CurrentUser\$store" | Where-Object { $_.Subject -eq $Subject } | ForEach-Object {
            Remove-Item $_.PSPath
            $removed++
        }
    }
    Write-Host "Removed $removed certificate(s)."
    exit 0
}

$cert = Get-SigningCert

if ($Trust) {
    # Trusting a self-signed certificate is a per-user decision, never done by
    # the build itself. Root makes the chain valid; TrustedPublisher removes
    # the publisher prompt for this identity.
    $cerPath = Join-Path $env:TEMP 'rvm-signing.cer'
    Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
    try {
        foreach ($store in 'Root', 'TrustedPublisher') {
            $already = Get-ChildItem "Cert:\CurrentUser\$store" | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
            if (-not $already) {
                Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\CurrentUser\$store" | Out-Null
                Write-Host "Trusted in CurrentUser\$store"
            }
        }
    } finally {
        Remove-Item $cerPath -ErrorAction SilentlyContinue
    }
}

if (-not $Path) { exit 0 }
if (-not (Test-Path $Path)) { throw "Nothing to sign at $Path" }

$signtool = Find-SignTool
$file = (Resolve-Path $Path).Path

# A timestamp keeps the signature valid after the certificate expires, but
# needs the network; sign without one rather than fail the build offline.
$common = @('sign', '/sha1', $cert.Thumbprint, '/fd', 'SHA256', '/q')
# Under 'Stop', Windows PowerShell turns a native tool's redirected stderr
# into a terminating error, which would end the script before the fallback.
$stamped = $false
& {
    $ErrorActionPreference = 'Continue'
    & $signtool @common /tr http://timestamp.digicert.com /td SHA256 $file 2>$null | Out-Null
}
if ($LASTEXITCODE -eq 0) { $stamped = $true }
if (-not $stamped) {
    & $signtool @common $file
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $file" }
    Write-Host "Signed (no timestamp): $file"
} else {
    Write-Host "Signed: $file"
}
