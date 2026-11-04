#!/usr/bin/env python3
"""Check the XPDM package, shared mapping service, and escape/ICD contract."""

import re
from pathlib import Path


HERE = Path(__file__).resolve().parent
inf = (HERE / "qxp.inf").read_text(encoding="utf-8")
header = (HERE / "qxp_escape.h").read_text(encoding="utf-8")
normalized = re.sub(r"\s+", "", inf).lower()


def macro(name: str) -> int:
    match = re.search(rf"^#define\s+{name}\s+([^\s]+)", header, re.M)
    if not match:
        raise SystemExit(f"missing header macro {name}")
    return int(match.group(1).rstrip("UuLl"), 0)


def section_lines(name: str) -> list[str]:
    matches = re.findall(
        rf"(?ims)^\[{re.escape(name)}\]\s*(.*?)(?=^\[|\Z)", inf
    )
    if len(matches) != 1:
        raise SystemExit(f"INF must contain exactly one {name} section")
    return [
        re.sub(r"\s+", "", line.split(";", 1)[0]).lower()
        for line in matches[0].splitlines()
        if line.split(";", 1)[0].strip()
    ]


required = {
    "display class": "class=display",
    "display class GUID": "classguid={4d36e968-e325-11ce-bfc1-08002be10318}",
    "catalog": "catalogfile=qxp.cat",
    "third-party XPDM feature score": "featurescore=fc",
    "NVIDIA GeForce4 Ti 4600 PCI ID":
        r"pci\ven_10de&dev_0250&subsys_11001af4",
    "QEMU std-vga PCI ID (legacy migration match)":
        r"pci\ven_1234&dev_1111",
    "miniport service": "addservice=qxpmp,0x00000002,qxp.service",
    "PnP demand-start miniport": "starttype=3",
    "miniport destination": "qxp.miniport=12",
    "display destination": "qxp.display=11",
    "ICD destination": "qxp.icd=11",
    "co-installer destination": "qxp.coinstaller=11",
    "display binding": "installeddisplaydrivers,%reg_multi_sz%,qxpdisp",
    "full acceleration": "acceleration.level,%reg_dword%,0",
    "miniport file": "qxpmp.sys",
    "display file": "qxpdisp.dll",
    "ICD file": "qxpicd.dll",
    "co-installer file": "qxpcoin2.dll",
    "co-installer registration":
        'coinstallers32,%reg_multi_sz%,"qxpcoin2.dll,qxpcoinstaller"',
}
for label, value in required.items():
    if value not in normalized:
        raise SystemExit(f"INF missing {label}: {value}")

map_directives = {
    "DestinationDirs": ("qxp.map=12",),
    "SourceDisksFiles": ("qxpmap.sys=1",),
    "QXP.Install": (
        "copyfiles=qxp.miniport,qxp.display,qxp.icd,qxp.map",
    ),
}
for section, directives in map_directives.items():
    lines = section_lines(section)
    for directive in directives:
        key = directive.split("=", 1)[0] + "="
        if [line for line in lines if line.startswith(key)] != [directive]:
            raise SystemExit(f"INF {section} must contain only {directive}")

if section_lines("QXP.Map") != ["qxpmap.sys,,,0x00000020"]:
    raise SystemExit(
        "INF QXP.Map must copy qxpmap.sys with COPYFLG_NO_VERSION_DIALOG"
    )
services = [
    re.sub(r"\s+", "", entry).lower()
    for entry in re.findall(r"(?im)^\s*AddService\s*=\s*([^;\r\n]+)", inf)
]
if services != ["qxpmp,0x00000002,qxp.service"] or section_lines(
    "QXP.Install.Services"
) != ["addservice=qxpmp,0x00000002,qxp.service"]:
    raise SystemExit(
        "INF must declare only associated qxpmp; the co-installer owns qxpmap"
    )

coinstaller_section = re.search(
    r"(?ims)^\[QXP\.Install\.CoInstallers\]\s*(.*?)(?=^\[|\Z)", inf
)
if not coinstaller_section:
    raise SystemExit("INF missing QXP.Install.CoInstallers")
normalized_coinstaller = re.sub(
    r"\s+", "", coinstaller_section.group(1)
).lower()
for directive in (
    "copyfiles=qxp.coinstaller", "addreg=qxp.coinstaller.addreg"
):
    if directive not in normalized_coinstaller:
        raise SystemExit(f"INF co-installer section missing {directive}")

software_settings = re.search(
    r"(?ims)^\[QXP\.Install\.SoftwareSettings\]\s*(.*?)(?=^\[|\Z)", inf
)
if not software_settings:
    raise SystemExit("INF missing QXP.Install.SoftwareSettings")
software_addreg = re.findall(
    r"(?im)^\s*AddReg\s*=\s*([^;\r\n]+)", software_settings.group(1)
)
if [entry.strip().lower() for entry in software_addreg] != [
    "qxp.devicesettings", "qxp.opengl"
]:
    raise SystemExit(
        "INF SoftwareSettings must install QXP.DeviceSettings then QXP.OpenGL"
    )

device_settings = re.search(
    r"(?ims)^\[QXP\.DeviceSettings\]\s*(.*?)(?=^\[|\Z)", inf
)
if not device_settings:
    raise SystemExit("INF missing QXP.DeviceSettings")

opengl_settings = re.search(
    r"(?ims)^\[QXP\.OpenGL\]\s*(.*?)(?=^\[|\Z)", inf
)
if not opengl_settings:
    raise SystemExit("INF missing QXP.OpenGL")
normalized_opengl_settings = re.sub(
    r"\s+", "", opengl_settings.group(1)
).lower()

driver_version = re.search(
    r"(?im)^\s*DriverVer\s*=\s*[^,\r\n]+,\s*(\d+)\.(\d+)\.(\d+)\.(\d+)\s*$",
    inf,
)
if not driver_version or any(
    int(part) > 65535 for part in driver_version.groups()
) or not any(int(part) for part in driver_version.groups()):
    raise SystemExit("INF DriverVer must have a nonzero four-WORD version")

key = r'"software\microsoft\windowsnt\currentversion\opengldrivers\qxpicd"'
registry = {
    "Dll": '"dll",%reg_sz%,"qxpicd.dll"',
    "Version": f'"version",%reg_dword%,{macro("QXP_ICD_INTERFACE_VERSION")}',
    "DriverVersion":
        f'"driverversion",%reg_dword%,{macro("QXP_ICD_DRIVER_VERSION")}',
    "Flags": '"flags",%reg_dword%,1',
}
for label, tail in registry.items():
    if f"hklm,{key},{tail}" not in normalized_opengl_settings:
        raise SystemExit(
            f"INF QXP.OpenGL has missing/mismatched OpenGL {label}"
        )

print("qxp.inf: PCI binding, files, services, display, and ICD tuple are consistent")
