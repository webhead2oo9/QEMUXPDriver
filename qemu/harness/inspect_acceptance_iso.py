#!/usr/bin/env python3
"""Report the exact signed XPDM package embedded in an acceptance ISO."""

import hashlib
import io
import json
import sys
from pathlib import Path


WORKSPACE = Path(__file__).resolve().parents[2]
PACKAGE_MEMBERS = (
    "qxp.inf",
    "qxp.cat",
    "xpdriver-test.cer",
    "qxpmp.sys",
    "qxpmap.sys",
    "qxpdisp.dll",
    "qxpicd.dll",
    "qxpcoin2.dll",
)
sys.path.insert(0, str(WORKSPACE / ".tools" / "pycdlib"))

try:
    import pycdlib
except ImportError as error:
    raise SystemExit(
        "pycdlib 1.14.0 is required under .tools/pycdlib"
    ) from error


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: inspect_acceptance_iso.py ACCEPTANCE_ISO")
    iso_path = Path(sys.argv[1]).resolve()
    if not iso_path.is_file():
        raise SystemExit(f"acceptance ISO is missing: {iso_path}")

    image = pycdlib.PyCdlib()
    image.open(str(iso_path))
    try:
        entries = list(image.walk(joliet_path="/XPDM"))
        if len(entries) != 1:
            raise SystemExit("acceptance ISO XPDM directory is missing or nested")
        _, directories, files = entries[0]
        actual = {name.casefold(): name for name in files}
        expected = {name.casefold(): name for name in PACKAGE_MEMBERS}
        missing = set(expected) - set(actual)
        if directories or missing:
            raise SystemExit(
                "acceptance ISO lacks exact XPDM package members: "
                f"missing={sorted(missing)} "
                f"directories={sorted(directories)}"
            )


        records = []
        for name in PACKAGE_MEMBERS:
            payload = io.BytesIO()
            image.get_file_from_iso_fp(
                payload, joliet_path=f"/XPDM/{actual[name.casefold()]}"
            )
            data = payload.getvalue()
            records.append(
                {
                    "name": name,
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest().upper(),
                }
            )
    finally:
        image.close()

    print(json.dumps(records, separators=(",", ":"), sort_keys=True))


if __name__ == "__main__":
    main()
