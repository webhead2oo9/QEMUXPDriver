$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-iso.ps1')

function Assert-AcceptanceIsoTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$root = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-acceptance-iso-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $root)
try {
    $iso = Join-Path $root 'prepared.iso'
    [IO.File]::WriteAllBytes($iso, [byte[]](1, 2, 3, 4, 5))
    $identity = Get-XpAcceptanceIsoIdentity -Path $iso
    Assert-AcceptanceIsoTest ($identity.length -eq 5) `
        'prepared ISO identity length is wrong'
    $verified = Assert-XpPreparedAcceptanceIso -Path $iso `
        -ExpectedSha256 $identity.sha256
    Assert-AcceptanceIsoTest `
        ($verified.sha256 -ceq $identity.sha256) `
        'unchanged prepared ISO did not verify'
    $isoSource = Join-Path $root 'iso-source'
    $isoPackage = Join-Path $isoSource 'XPDM'
    [void](New-Item -ItemType Directory -Path $isoPackage)
    $packageMembers = @(
        'qxp.inf', 'qxp.cat', 'xpdriver-test.cer', 'qxpmp.sys',
        'qxpmap.sys', 'qxpdisp.dll', 'qxpicd.dll', 'qxpcoin2.dll')
    foreach ($name in $packageMembers) {
        [IO.File]::WriteAllText(
            (Join-Path $isoPackage $name), "fixture-$name")
    }
    $packageIso = Join-Path $root 'package.iso'
    $isoBuilder = Join-Path (
        Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) `
        'guest\acceptance\make_iso.py'
    $buildOutput = @(& py.exe -3 $isoBuilder $isoSource $packageIso 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "synthetic acceptance ISO build failed: $buildOutput"
    }
    $packageFiles = @(Get-XpAcceptanceIsoPackageFiles -Path $packageIso)
    Assert-AcceptanceIsoTest ($packageFiles.Count -eq 8) `
        'acceptance ISO package inspector omitted members'
    foreach ($record in $packageFiles) {
        $sourceFile = Join-Path $isoPackage ([string]$record.name)
        Assert-AcceptanceIsoTest (
            [string]$record.sha256 -ceq
                (Get-FileHash -LiteralPath $sourceFile `
                    -Algorithm SHA256).Hash) `
            "acceptance ISO package hash differs for $($record.name)"
    }

    foreach ($missingMember in @('qxpdisp.dll', 'qxpmap.sys')) {
        Remove-Item -LiteralPath (
            Join-Path $isoPackage $missingMember) -Force
        [IO.File]::WriteAllText(
            (Join-Path $isoPackage 'unrelated-tool.exe'),
            'not-a-package-member')
        $missingMemberIso = Join-Path $root "missing-$missingMember.iso"
        $buildOutput = @(
            & py.exe -3 $isoBuilder $isoSource $missingMemberIso 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "synthetic missing-member ISO build failed: $buildOutput"
        }
        $missingMemberRejected = $false
        try {
            [void](Get-XpAcceptanceIsoPackageFiles -Path $missingMemberIso)
        } catch {
            $missingMemberRejected = $_.Exception.Message -like
                "*lacks exact XPDM package members*'$missingMember'*"
        }
        Assert-AcceptanceIsoTest $missingMemberRejected `
            "acceptance ISO missing $missingMember was accepted"
        [IO.File]::WriteAllText(
            (Join-Path $isoPackage $missingMember), "fixture-$missingMember")
    }


    $privateIso = Join-Path $root 'private prepared.iso'
    $privateIsoArgument =
        [IO.Path]::GetFullPath($privateIso).Replace('\', '/')
    $driveContent =
        'qemu -hda disk.qcow2 -drive file=../../old.iso,format=raw,media=cdrom,index=2,id=acceptcd -nic none'
    $rewrittenDrive = Set-XpContentAcceptanceIsoPath `
        -ContentText $driveContent -IsoPath $privateIso
    Assert-AcceptanceIsoTest `
        ($rewrittenDrive.Contains(
            ('-drive "file={0},format=raw,media=cdrom,index=2,id=acceptcd"' -f
                $privateIsoArgument)) -and
         $rewrittenDrive.Contains('-hda disk.qcow2') -and
         $rewrittenDrive.Contains('-nic none') -and
         -not $rewrittenDrive.Contains('../../old.iso')) `
        'modern -drive CD-ROM path was not rewritten exactly'
    $quotedDriveContent =
        'qemu -drive if=none,file=data.img,format=raw -drive "id=acceptcd,media=cdrom,file=old.iso,format=raw"'
    $rewrittenQuotedDrive = Set-XpContentAcceptanceIsoPath `
        -ContentText $quotedDriveContent -IsoPath $privateIso
    Assert-AcceptanceIsoTest `
        ($rewrittenQuotedDrive.Contains(
            ('-drive "id=acceptcd,media=cdrom,file={0},format=raw"' -f
                $privateIsoArgument)) -and
         $rewrittenQuotedDrive.Contains(
            '-drive if=none,file=data.img,format=raw')) `
        'quoted/order-varied CD-ROM drive rewrite changed another drive'
    $dollarIso = Join-Path $root 'private $1 $& prepared.iso'
    $dollarIsoArgument =
        [IO.Path]::GetFullPath($dollarIso).Replace('\', '/')
    $rewrittenLegacy = Set-XpContentAcceptanceIsoPath `
        -ContentText 'qemu -cdrom old.iso -boot c' -IsoPath $dollarIso
    Assert-AcceptanceIsoTest `
        ($rewrittenLegacy -eq
            ('qemu -cdrom "{0}" -boot c' -f $dollarIsoArgument)) `
        'legacy -cdrom path was not inserted literally'
    $decoyContent =
        'qemu -name "quoted -drive file=decoy.iso,media=cdrom" -fake-drive file=fake.iso,media=cdrom -DRIVE file=real.iso,MEDIA=CDROM,id=acceptcd'
    $rewrittenDecoy = Set-XpContentAcceptanceIsoPath `
        -ContentText $decoyContent -IsoPath $privateIso
    Assert-AcceptanceIsoTest `
        ($rewrittenDecoy.Contains(
            '"quoted -drive file=decoy.iso,media=cdrom"') -and
         $rewrittenDecoy.Contains(
            '-fake-drive file=fake.iso,media=cdrom') -and
         $rewrittenDecoy.Contains(
            ('-DRIVE "file={0},MEDIA=CDROM,id=acceptcd"' -f
                $privateIsoArgument))) `
        'exact-token rewrite accepted or changed a decoy option'
    foreach ($invalidContent in @(
            'qemu -hda disk.qcow2',
            'qemu -drive media=cdrom,format=raw',
            'qemu -cdrom a.iso -drive file=b.iso,media=cdrom',
            'qemu -fake-drive file=a.iso,media=cdrom',
            'qemu -name "quoted -drive file=a.iso,media=cdrom"',
            'qemu -drive file=a.iso,media=cdrom,media=cdrom',
            'qemu -drive file=a.iso,file=b.iso,media=cdrom')) {
        $invalidRejected = $false
        try {
            [void](Set-XpContentAcceptanceIsoPath `
                -ContentText $invalidContent -IsoPath $privateIso)
        } catch {
            $invalidRejected = $_.Exception.Message -like
                'Prepared-ISO*'
        }
        Assert-AcceptanceIsoTest $invalidRejected `
            "invalid/ambiguous CD-ROM content was accepted: $invalidContent"
    }

    [IO.File]::AppendAllText($iso, 'mutation')
    $mutationRejected = $false
    try {
        [void](Assert-XpPreparedAcceptanceIso -Path $iso `
            -ExpectedSha256 $identity.sha256)
    } catch {
        $mutationRejected = $_.Exception.Message -like `
            '*Prepared acceptance ISO hash mismatch*'
    }
    Assert-AcceptanceIsoTest $mutationRejected `
        'post-hash prepared ISO mutation was not rejected'

    $missingRejected = $false
    try {
        [void](Get-XpAcceptanceIsoIdentity `
            -Path (Join-Path $root 'missing.iso'))
    } catch {
        $missingRejected = $_.Exception.Message -like `
            '*Acceptance ISO is missing*'
    }
    Assert-AcceptanceIsoTest $missingRejected `
        'missing prepared ISO was not rejected'

    Write-Output 'acceptance ISO identity tests passed'
} finally {
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
