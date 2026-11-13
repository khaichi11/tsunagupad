# Tsunagu-Pad: Mobile Hotspot Windows untuk iPad.
#
#   hotspot.ps1 status | on | off | capability
#   hotspot.ps1 config NAMA_WIFI    (kata sandi dibaca dari stdin)
#
# Windows menjalankan hotspot di adaptor virtual "Wi-Fi Direct", jadi Wi-Fi
# utama tetap tersambung dan internetnya ikut dibagikan ke iPad, sama seperti
# hotspot ap0 di versi Linux. Hasilnya satu baris JSON.
#
# SPDX-License-Identifier: MIT
param(
    [Parameter(Position = 0)][string]$Action = 'status',
    [Parameter(Position = 1)][string]$Ssid = ''
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
[Console]::InputEncoding = New-Object System.Text.UTF8Encoding $false

function Out-Result([hashtable]$Data) {
    $Data['event'] = 'hotspot'
    [Console]::Out.WriteLine(($Data | ConvertTo-Json -Compress))
}

function Fail([string]$Message) {
    [Console]::Out.WriteLine((@{ event = 'error'; message = $Message } | ConvertTo-Json -Compress))
    exit 1
}

try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $null = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]
    $null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
    $null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringAccessPointConfiguration, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
    $null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
} catch {
    Fail 'Mobile Hotspot tidak tersedia di Windows ini.'
}

$methods = [System.WindowsRuntimeSystemExtensions].GetMethods()
$asTaskOp = $methods | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
$asTaskAction = $methods | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
} | Select-Object -First 1

function Await($Operation, [Type]$ResultType) {
    $task = $asTaskOp.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    $null = $task.Wait(60000)
    $task.Result
}

function Await-Action($Operation) {
    $task = $asTaskAction.Invoke($null, @($Operation))
    $null = $task.Wait(60000)
}

# Koneksi yang dibagikan: koneksi internet saat ini, atau koneksi apa pun
# yang tersambung bila internetnya belum ada (mis. Wi-Fi kampus sebelum login).
$info = [Windows.Networking.Connectivity.NetworkInformation]
$conn = $info::GetInternetConnectionProfile()
if (-not $conn) {
    $conn = $info::GetConnectionProfiles() | Where-Object {
        $_.GetNetworkConnectivityLevel() -ne [Windows.Networking.Connectivity.NetworkConnectivityLevel]::None
    } | Select-Object -First 1
}

$tm = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]
if (-not $conn) {
    if ($Action -eq 'capability') { Out-Result @{ capability = 'unknown' }; exit 0 }
    if ($Action -eq 'status') {
        Out-Result @{ state = 'inactive'; capability = 'unknown'
                      reason = 'Sambungkan Wi-Fi atau Ethernet dulu; Mobile Hotspot membagikan sambungan itu.' }
        exit 0
    }
    Fail 'Sambungkan Wi-Fi atau Ethernet dulu; Mobile Hotspot membagikan sambungan itu.'
}

$cap = $tm::GetTetheringCapabilityFromConnectionProfile($conn)
$supported = $cap -eq [Windows.Networking.NetworkOperators.TetheringCapability]::Enabled
if ($Action -eq 'capability') {
    Out-Result @{ capability = $(if ($supported) { 'yes' } else { 'no' }); detail = "$cap" }
    exit 0
}
if (-not $supported) {
    if ($Action -eq 'status') { Out-Result @{ state = 'unsupported'; capability = 'no'; reason = "$cap" }; exit 0 }
    Fail "Mobile Hotspot tidak bisa dipakai di sini ($cap)."
}

$manager = $tm::CreateFromConnectionProfile($conn)

function Status {
    $config = $manager.GetCurrentAccessPointConfiguration()
    $state = switch ("$($manager.TetheringOperationalState)") {
        'On' { 'active' }
        'InTransition' { 'busy' }
        default { 'inactive' }
    }
    Out-Result @{
        state = $state
        capability = 'yes'
        ssid = $config.Ssid
        password = $config.Passphrase
        clients = $manager.ClientCount
        shared = $conn.ProfileName
    }
}

switch ($Action) {
    'status' { Status }
    'on' {
        $r = Await ($manager.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        if ("$($r.Status)" -ne 'Success') {
            $detail = if ($r.AdditionalErrorMessage) { $r.AdditionalErrorMessage } else { "$($r.Status)" }
            Fail "Hotspot gagal dinyalakan: $detail"
        }
        Status
    }
    'off' {
        $r = Await ($manager.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        if ("$($r.Status)" -ne 'Success') { Fail "Hotspot gagal dimatikan: $($r.Status)" }
        Status
    }
    'config' {
        # Kata sandi lewat stdin supaya tidak terlihat di daftar proses.
        $password = [Console]::In.ReadLine()
        if ([string]::IsNullOrEmpty($Ssid) -or [Text.Encoding]::UTF8.GetByteCount($Ssid) -gt 32) {
            Fail 'Nama Wi-Fi harus 1-32 byte.'
        }
        if ($password -notmatch '^[\x20-\x7e]{8,63}$') {
            Fail 'Kata sandi harus 8-63 karakter: huruf, angka, simbol, atau spasi.'
        }
        $config = $manager.GetCurrentAccessPointConfiguration()
        $config.Ssid = $Ssid
        $config.Passphrase = $password
        Await-Action ($manager.ConfigureAccessPointAsync($config))
        Status
    }
    default { Fail "Perintah tidak dikenal: $Action" }
}
