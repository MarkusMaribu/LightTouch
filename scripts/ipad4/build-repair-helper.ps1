param(
    [Parameter(Mandatory)] [string] $Engine,
    [Parameter(Mandatory)] [string] $Sdk,
    [Parameter(Mandatory)] [string] $Ld64,
    [Parameter(Mandatory)] [string] $Ldid,
    [Parameter(Mandatory)] [string] $Output
)

# The Apple linker and its dependent DLLs must be on PATH.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$enginePath = (Resolve-Path $Engine).Path
$sdkPath = (Resolve-Path $Sdk).Path
$linker = (Get-Command $Ld64 -ErrorAction Stop).Source
$signer = (Get-Command $Ldid -ErrorAction Stop).Source
$outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$workPath = Join-Path ([IO.Path]::GetTempPath()) ("ipad-audio-" + [Guid]::NewGuid())
$work = (New-Item -ItemType Directory -Path $workPath).FullName
try {
    $cflags = @('-target', 'armv7-apple-ios6.0', '-marm', '-Os', '-fno-stack-protector',
                '-D_FORTIFY_SOURCE=0', '-isysroot', $sdkPath)
    clang @cflags -c "$enginePath/contrib/armv6-toolchain/crt1old.c" -o "$work/start.o"
    clang @cflags -c "$PSScriptRoot/repair_audio.c" -o "$work/repair.o"
    & $linker -arch armv7 -execute -ios_version_min 6.0 -no_pie -no_function_starts `
        -no_data_in_code_info -no_uuid -syslibroot $sdkPath -L "$sdkPath/usr/lib" `
        -lSystem -e _start -o "$work/repair_audio" "$work/start.o" "$work/repair.o"
    Push-Location $work
    try {
        & $signer -Hsha1 "-S$enginePath/contrib/it-restore/entitlements.plist" repair_audio
    } finally {
        Pop-Location
    }
    Copy-Item -LiteralPath "$work/repair_audio" -Destination $outputPath
} finally {
    foreach ($file in @('start.o', 'repair.o', 'repair_audio')) {
        $path = Join-Path $work $file
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    Remove-Item -LiteralPath $work
}
