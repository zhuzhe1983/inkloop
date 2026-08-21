"""PlatformIO pre-build gate for packaged InkloopVoice prompt assets."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


def _asset_root() -> Path:
    # PlatformIO executes extra scripts with SCons `exec`, where `__file__` is
    # not guaranteed to exist. Resolve from the project root in that mode so
    # this remains an unconditional project-level pre-build gate.
    if "env" in globals():
        return (
            Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
            / "lib"
            / "InkloopVoice"
        ).resolve()
    return Path(__file__).resolve().parent.parent


def verify_prompt_assets(*_args, **_kwargs):
    root = _asset_root()
    manifest_path = root / "assets" / "prompts.v1.json"
    if not manifest_path.is_file():
        raise RuntimeError(f"missing prompt manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != "inkloop.prompt-manifest" or manifest.get("version") != 1:
        raise RuntimeError("unsupported InkloopVoice prompt manifest")
    entries = manifest.get("entries")
    if not isinstance(entries, list) or not entries:
        raise RuntimeError("InkloopVoice prompt manifest has no entries")
    for entry in entries:
        for path_key, hash_key in (("file", "sha256"), ("fallback", "fallbackSha256")):
            asset = (manifest_path.parent / entry[path_key]).resolve()
            if manifest_path.parent not in asset.parents or not asset.is_file():
                raise RuntimeError(f"missing prompt asset: {entry[path_key]}")
            payload = asset.read_bytes()
            if len(payload) < 44 or payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
                raise RuntimeError(f"invalid prompt WAV: {entry[path_key]}")
            if hashlib.sha256(payload).hexdigest() != entry[hash_key]:
                raise RuntimeError(f"prompt checksum mismatch: {entry[path_key]}")


try:
    Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injects Import.
except NameError:
    verify_prompt_assets()
else:
    # Run while PlatformIO evaluates the project extra script. This is before
    # dependency resolution/compilation, so a damaged package can never emit a
    # firmware image and the gate cannot be skipped by an incremental build.
    verify_prompt_assets()
