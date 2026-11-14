# Memasang Tsunagu-Pad Display, driver monitor virtual untuk layar kedua.
#
# Dijalankan pemasang Tsunagu-Pad dengan hak administrator. Windows hanya
# memasang driver yang tanda tangannya dipercaya. Supaya tidak bergantung
# pada pihak lain dan tanpa biaya, tanda tangan dibuat di komputer ini:
#
#   1. sertifikat penandatangan kode sekali pakai dibuat di sini;
#   2. katalog driver (daftar sidik berkas driver) ditandatangani dengannya;
#   3. sertifikat itu (tanpa kunci rahasianya) dicatat sebagai tepercaya;
#   4. kunci rahasianya dihapus, jadi tidak ada siapa pun yang bisa
#      menandatangani apa pun lagi dengan sertifikat itu.
#
# Driver ini berjalan di mode pengguna (UMDF), bukan di kernel. Saat
# Tsunagu-Pad dihapus, driver dan sertifikatnya ikut dihapus
# (tsunagupad setup display-remove).
#
# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory = $true)][string]$DriverDir,
    [Parameter(Mandatory = $true)][string]$Exe
)
$ErrorActionPreference = 'Stop'
$subject = 'CN=Tsunagu-Pad Display Driver (local)'

# Windows PowerShell 5.1 memakai modulnya sendiri, juga bila pemasang dibuka
# dari PowerShell 7 (yang mewariskan PSModulePath-nya dan membuat drive Cert:
# hilang karena modul Security versi 7 termuat).
$env:PSModulePath = @("$env:ProgramFiles\WindowsPowerShell\Modules",
                      "$env:SystemRoot\System32\WindowsPowerShell\v1.0\Modules") -join ';'
Import-Module Microsoft.PowerShell.Security, PKI

# Catatan pemasangan, untuk dilihat bila layar kedua tidak muncul.
$log = Join-Path $env:ProgramData 'TsunaguPad\display-install.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
Start-Transcript -Path $log -Force | Out-Null
$code = 1
try {

    # Pasangan sebelumnya (driver dan sertifikat lama) dibersihkan dulu.
    & $Exe setup display-remove | Out-Host

    $cat = Join-Path $DriverDir 'TsunaguPadDisplay.cat'
    if (-not (Test-Path $cat)) { throw "Katalog driver tidak ditemukan: $cat" }

    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation 'Cert:\LocalMachine\My' -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(30)
    try {
        $sig = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
        if (-not $sig.SignerCertificate) { throw "Katalog driver tidak bisa ditandatangani: $($sig.StatusMessage)" }

        $cer = Join-Path $env:TEMP "tsunagupad-display-$PID.cer"
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        try {
            Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
            Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
        } finally {
            Remove-Item $cer -Force -ErrorAction SilentlyContinue
        }
    } finally {
        # Kunci rahasia dihapus apa pun hasilnya.
        Remove-Item -Path "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path "Cert:\LocalMachine\My\$($cert.Thumbprint)") { throw 'Kunci rahasia sertifikat sekali pakai tidak terhapus' }
    "Katalog driver ditandatangani di komputer ini ($($cert.Thumbprint)); kunci rahasianya sudah dihapus."

    & $Exe setup display-install $DriverDir | Out-Host
    $code = $LASTEXITCODE
} catch {
    "GAGAL: $_"
    $code = 1
} finally {
    Stop-Transcript | Out-Null
}
exit $code
