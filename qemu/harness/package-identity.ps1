$ErrorActionPreference = 'Stop'

function ConvertFrom-XpAcceptanceInfIdentity {
    param([string]$Text)

    $driverVerMatches = [regex]::Matches(
        $Text, '(?im)^\s*DriverVer\s*=\s*([^,\r\n]+)\s*,\s*([^\r\n;]+)')
    if ($driverVerMatches.Count -ne 1) {
        throw "XPDM INF must contain exactly one DriverVer entry"
    }
    $catalogMatches = [regex]::Matches(
        $Text, '(?im)^\s*CatalogFile\s*=\s*([^\r\n;]+)')
    if ($catalogMatches.Count -ne 1) {
        throw "XPDM INF must contain exactly one CatalogFile entry"
    }
    $driverDate = $driverVerMatches[0].Groups[1].Value.Trim()
    $driverVersion = $driverVerMatches[0].Groups[2].Value.Trim()
    if ($driverDate -notmatch '^\d{2}/\d{2}/\d{4}$' -or
        $driverVersion -notmatch '^\d+(?:\.\d+){3}$') {
        throw "XPDM INF DriverVer has an unsupported format"
    }
    $catalogFile = $catalogMatches[0].Groups[1].Value.Trim()
    if ([IO.Path]::GetFileName($catalogFile) -ne $catalogFile) {
        throw "XPDM INF CatalogFile must be one file name"
    }
    return [pscustomobject][ordered]@{
        driverVer = "$driverDate,$driverVersion"
        driverDate = $driverDate
        driverVersion = $driverVersion
        catalogFile = $catalogFile
    }
}

# Catalog membership is verified against the catalog bytes themselves, never
# the mutable system catalog database: each install payload's catalog digest
# (the same digest Inf2Cat embeds and WinVerifyTrust matches) must appear
# among the members of qxp.cat. A retained signed catalog therefore cannot
# bless a payload replaced after signing.
if (-not ('XpCatalogMembership' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class XpCatalogMembership
{
    [StructLayout(LayoutKind.Sequential)]
    struct CRYPT_ATTR_BLOB { public uint cbData; public IntPtr pbData; }

    [StructLayout(LayoutKind.Sequential)]
    struct CRYPT_ALGORITHM_IDENTIFIER
    {
        public IntPtr pszObjId;
        public CRYPT_ATTR_BLOB Parameters;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct CRYPT_ATTRIBUTE_TYPE_VALUE
    {
        public IntPtr pszObjId;
        public CRYPT_ATTR_BLOB Value;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SIP_INDIRECT_DATA
    {
        public CRYPT_ATTRIBUTE_TYPE_VALUE Data;
        public CRYPT_ALGORITHM_IDENTIFIER DigestAlgorithm;
        public CRYPT_ATTR_BLOB Digest;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct CRYPTCATMEMBER
    {
        public uint cbStruct;
        public IntPtr pwszReferenceTag;
        public IntPtr pwszFileName;
        public Guid gSubjectType;
        public uint fdwMemberFlags;
        public IntPtr pIndirectData;
        public uint dwCertVersion;
        public uint dwReserved;
        public IntPtr hReserved;
        public CRYPT_ATTR_BLOB sEncodedIndirectData;
        public CRYPT_ATTR_BLOB sEncodedMemberInfo;
    }

    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATOpen(string pwszFileName, uint dwFlags,
        IntPtr hProv, uint dwPublicVersion, uint dwEncodingType);

    [DllImport("wintrust.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CryptCATClose(IntPtr hCatalog);

    [DllImport("wintrust.dll", SetLastError = true)]
    static extern IntPtr CryptCATEnumerateMember(IntPtr hCatalog,
        IntPtr pPrevMember);

    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CryptCATAdminAcquireContext2(out IntPtr phCatAdmin,
        IntPtr pgSubsystem, string pwszHashAlgorithm,
        IntPtr pStrongHashPolicy, uint dwFlags);

    [DllImport("wintrust.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CryptCATAdminReleaseContext(IntPtr hCatAdmin,
        uint dwFlags);

    [DllImport("wintrust.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr hCatAdmin,
        IntPtr hFile, ref uint pcbHash, IntPtr pbHash, uint dwFlags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess,
        uint dwShareMode, IntPtr lpSecurityAttributes,
        uint dwCreationDisposition, uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CloseHandle(IntPtr hObject);

    const uint GENERIC_READ = 0x80000000;
    const uint FILE_SHARE_READ = 0x1;
    const uint OPEN_EXISTING = 3;
    static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

    public sealed class CatalogMembers
    {
        public string AlgorithmOid;
        public string[] Digests;
    }

    public static CatalogMembers GetMembers(string catalogPath)
    {
        var digests = new List<string>();
        string algorithmOid = null;
        IntPtr catalog = CryptCATOpen(catalogPath, 0, IntPtr.Zero, 0, 0);
        if (catalog == IntPtr.Zero)
        {
            throw new InvalidOperationException(
                "catalog could not be opened (error " +
                Marshal.GetLastWin32Error() + ")");
        }
        try
        {
            IntPtr previous = IntPtr.Zero;
            for (;;)
            {
                IntPtr memberPtr = CryptCATEnumerateMember(catalog, previous);
                if (memberPtr == IntPtr.Zero) break;
                previous = memberPtr;
                var member = (CRYPTCATMEMBER)Marshal.PtrToStructure(
                    memberPtr, typeof(CRYPTCATMEMBER));
                if (member.pIndirectData == IntPtr.Zero)
                {
                    throw new InvalidOperationException(
                        "catalog member has no indirect data");
                }
                var indirect = (SIP_INDIRECT_DATA)Marshal.PtrToStructure(
                    member.pIndirectData, typeof(SIP_INDIRECT_DATA));
                if (indirect.DigestAlgorithm.pszObjId == IntPtr.Zero ||
                    indirect.Digest.pbData == IntPtr.Zero ||
                    indirect.Digest.cbData == 0)
                {
                    throw new InvalidOperationException(
                        "catalog member has no digest");
                }
                var oid = Marshal.PtrToStringAnsi(
                    indirect.DigestAlgorithm.pszObjId);
                if (algorithmOid == null)
                {
                    algorithmOid = oid;
                }
                else if (algorithmOid != oid)
                {
                    throw new InvalidOperationException(
                        "catalog mixes member digest algorithms");
                }
                var digest = new byte[indirect.Digest.cbData];
                Marshal.Copy(indirect.Digest.pbData, digest, 0, digest.Length);
                digests.Add(BitConverter.ToString(digest).Replace("-", ""));
            }
        }
        finally
        {
            CryptCATClose(catalog);
        }
        var result = new CatalogMembers();
        result.AlgorithmOid = algorithmOid;
        result.Digests = digests.ToArray();
        return result;
    }

    static string AlgorithmNameForOid(string oid)
    {
        switch (oid)
        {
            case "1.3.14.3.2.26": return "SHA1";
            case "2.16.840.1.101.3.4.2.1": return "SHA256";
            case "2.16.840.1.101.3.4.2.2": return "SHA384";
            case "2.16.840.1.101.3.4.2.3": return "SHA512";
            default:
                throw new InvalidOperationException(
                    "unsupported catalog digest algorithm OID: " + oid);
        }
    }

    public static string ComputeMemberHash(string filePath, string algorithmOid)
    {
        IntPtr admin;
        if (!CryptCATAdminAcquireContext2(out admin, IntPtr.Zero,
                AlgorithmNameForOid(algorithmOid), IntPtr.Zero, 0))
        {
            throw new InvalidOperationException(
                "catalog hash context could not be acquired (error " +
                Marshal.GetLastWin32Error() + ")");
        }
        try
        {
            IntPtr file = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ,
                IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
            if (file == INVALID_HANDLE_VALUE)
            {
                throw new InvalidOperationException(
                    "member file could not be opened (error " +
                    Marshal.GetLastWin32Error() + ")");
            }
            try
            {
                uint size = 0;
                CryptCATAdminCalcHashFromFileHandle2(
                    admin, file, ref size, IntPtr.Zero, 0);
                if (size == 0)
                {
                    throw new InvalidOperationException(
                        "catalog digest size could not be determined");
                }
                IntPtr buffer = Marshal.AllocHGlobal((int)size);
                try
                {
                    if (!CryptCATAdminCalcHashFromFileHandle2(
                            admin, file, ref size, buffer, 0))
                    {
                        throw new InvalidOperationException(
                            "member catalog digest could not be computed " +
                            "(error " + Marshal.GetLastWin32Error() + ")");
                    }
                    var hash = new byte[size];
                    Marshal.Copy(buffer, hash, 0, (int)size);
                    return BitConverter.ToString(hash).Replace("-", "");
                }
                finally
                {
                    Marshal.FreeHGlobal(buffer);
                }
            }
            finally
            {
                CloseHandle(file);
            }
        }
        finally
        {
            CryptCATAdminReleaseContext(admin, 0);
        }
    }
}
'@
}

function Get-XpPackageCatalogCoverage {
    param(
        [string]$CatalogPath,
        [string[]]$MemberPaths
    )

    $members = [XpCatalogMembership]::GetMembers($CatalogPath)
    $uncovered = [Collections.Generic.List[string]]::new()
    if ($members.AlgorithmOid) {
        foreach ($path in @($MemberPaths)) {
            $digest = [XpCatalogMembership]::ComputeMemberHash(
                $path, [string]$members.AlgorithmOid)
            if ($members.Digests -notcontains $digest) {
                $uncovered.Add([IO.Path]::GetFileName($path))
            }
        }
    }
    return [pscustomobject][ordered]@{
        algorithmOid = $members.AlgorithmOid
        memberDigests = $members.Digests.Count
        uncovered = @($uncovered)
    }
}

function Get-XpPackagedTransportAbi {
    param([string]$Directory)

    $fullDirectory = [IO.Path]::GetFullPath($Directory)
    $versions = [Collections.Generic.List[uint32]]::new()
    foreach ($name in @('qxpmp.sys', 'qxpdisp.dll')) {
        $path = Join-Path $fullDirectory $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "transport ABI source is missing: $name"
        }
        $text = [Text.Encoding]::ASCII.GetString(
            [IO.File]::ReadAllBytes($path))
        $matches = [regex]::Matches(
            $text, 'QXP_D3D_TRANSPORT_ABI=(\d+)UL')
        if ($matches.Count -ne 1) {
            throw "$name must contain exactly one transport ABI marker"
        }
        $versions.Add([uint32]$matches[0].Groups[1].Value)
    }
    if ($versions[0] -ne $versions[1]) {
        throw "packaged display and miniport transport ABIs differ"
    }
    return $versions[0]
}

function Get-XpAcceptancePackageIdentity {
    param([string]$Directory)

    $fullDirectory = [IO.Path]::GetFullPath($Directory)
    $requiredFiles = @(
        'qxp.inf', 'qxp.cat', 'xpdriver-test.cer', 'qxpmp.sys',
        'qxpmap.sys', 'qxpdisp.dll', 'qxpicd.dll', 'qxpcoin2.dll'
    )
    $findings = [Collections.Generic.List[string]]::new()
    $fileRecords = [Collections.Generic.List[object]]::new()
    foreach ($name in $requiredFiles) {
        $path = Join-Path $fullDirectory $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $findings.Add("required package file is missing: $name")
            continue
        }
        $file = Get-Item -LiteralPath $path
        $fileRecords.Add([pscustomobject][ordered]@{
            name = $name
            size = $file.Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        })
    }
    # Membership is exact: an unexpected member rides the install ISO
    # without appearing in any hash the identity records.
    if (Test-Path -LiteralPath $fullDirectory -PathType Container) {
        foreach ($member in @(Get-ChildItem -LiteralPath $fullDirectory -Force)) {
            if ($member.PSIsContainer) {
                $findings.Add("unexpected package subdirectory: $($member.Name)")
            } elseif ($requiredFiles -notcontains $member.Name) {
                $findings.Add("unexpected package file: $($member.Name)")
            }
        }
    }

    $infIdentity = $null
    $infPath = Join-Path $fullDirectory 'qxp.inf'
    if (Test-Path -LiteralPath $infPath -PathType Leaf) {
        try {
            $infIdentity = ConvertFrom-XpAcceptanceInfIdentity `
                -Text (Get-Content -Raw -LiteralPath $infPath)
        } catch {
            $findings.Add($_.Exception.Message)
        }
    }

    $certificate = $null
    $certificatePath = Join-Path $fullDirectory 'xpdriver-test.cer'
    if (Test-Path -LiteralPath $certificatePath -PathType Leaf) {
        try {
            $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
                $certificatePath)
        } catch {
            $findings.Add("bundled certificate could not be decoded: $($_.Exception.Message)")
        }
    }

    $signature = $null
    $catalogPath = if ($infIdentity) {
        Join-Path $fullDirectory $infIdentity.catalogFile
    } else {
        Join-Path $fullDirectory 'qxp.cat'
    }
    if (Test-Path -LiteralPath $catalogPath -PathType Leaf) {
        try {
            $authenticode = Get-AuthenticodeSignature -LiteralPath $catalogPath
            $signature = [pscustomobject][ordered]@{
                status = $authenticode.Status.ToString()
                statusMessage = $authenticode.StatusMessage
                signerSubject = if ($authenticode.SignerCertificate) {
                    $authenticode.SignerCertificate.Subject
                } else { $null }
                signerThumbprint = if ($authenticode.SignerCertificate) {
                    $authenticode.SignerCertificate.Thumbprint.ToUpperInvariant()
                } else { $null }
            }
        } catch {
            $findings.Add("catalog signature could not be inspected: $($_.Exception.Message)")
        }
    }

    $certificateThumbprint = if ($certificate) {
        $certificate.Thumbprint.ToUpperInvariant()
    } else { $null }

    # The signed catalog must cover the payload bytes present now, not only
    # carry a valid signature: every install member's catalog digest has to
    # appear among the members of qxp.cat.
    $catalogCoverage = $null
    $catalogMemberNames = @(
        'qxp.inf', 'qxpmp.sys', 'qxpmap.sys', 'qxpdisp.dll',
        'qxpicd.dll', 'qxpcoin2.dll')
    if (Test-Path -LiteralPath $catalogPath -PathType Leaf) {
        $memberPaths = @($catalogMemberNames | ForEach-Object {
            Join-Path $fullDirectory $_
        } | Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        })
        try {
            $catalogCoverage = Get-XpPackageCatalogCoverage `
                -CatalogPath $catalogPath -MemberPaths $memberPaths
            if ($catalogCoverage.memberDigests -eq 0) {
                $findings.Add('catalog contains no members')
            }
            foreach ($uncovered in @($catalogCoverage.uncovered)) {
                $findings.Add(
                    "package member is not covered by the catalog: $uncovered")
            }
        } catch {
            $findings.Add(("catalog membership could not be verified: " +
                $_.Exception.Message))
        }
    }
    $catalogCoversPayload = $null -ne $catalogCoverage -and
        $catalogCoverage.memberDigests -gt 0 -and
        $catalogCoverage.uncovered.Count -eq 0

    $signerMatchesCertificate = $signature -and $certificateThumbprint -and
        $signature.signerThumbprint -eq $certificateThumbprint
    if ($signature -and -not $signature.signerThumbprint) {
        $findings.Add('catalog does not expose a signer certificate')
    } elseif ($signature -and -not $signerMatchesCertificate) {
        $findings.Add('catalog signer does not match the bundled certificate')
    }
    if ($signature -and
        $signature.status -notin @('Valid', 'UnknownError')) {
        $findings.Add("catalog signature status is $($signature.status)")
    }
    if ($signature -and $signature.status -eq 'UnknownError' -and
        $signature.statusMessage -notmatch '(?i)root certificate.*not trusted') {
        $findings.Add('catalog signature has an unexpected verification error')
    }
    if ($infIdentity -and $infIdentity.catalogFile -ne 'qxp.cat') {
        $findings.Add("unexpected XPDM catalog name: $($infIdentity.catalogFile)")
    }

    return [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-xpdm-package-identity'
        directory = $fullDirectory
        valid = $findings.Count -eq 0 -and
            $fileRecords.Count -eq $requiredFiles.Count -and
            $null -ne $infIdentity -and $signerMatchesCertificate -and
            $catalogCoversPayload
        driverVer = if ($infIdentity) { $infIdentity.driverVer } else { $null }
        driverDate = if ($infIdentity) { $infIdentity.driverDate } else { $null }
        driverVersion = if ($infIdentity) {
            $infIdentity.driverVersion
        } else { $null }
        catalogFile = if ($infIdentity) { $infIdentity.catalogFile } else { $null }
        certificate = if ($certificate) {
            [pscustomobject][ordered]@{
                subject = $certificate.Subject
                thumbprint = $certificateThumbprint
            }
        } else { $null }
        catalogSignature = $signature
        signerMatchesBundledCertificate = [bool]$signerMatchesCertificate
        catalogCoverage = $catalogCoverage
        files = @($fileRecords)
        findings = @($findings)
    }
}
