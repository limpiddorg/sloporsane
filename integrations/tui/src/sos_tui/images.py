"""Immutable image snapshots; paths and clipboard are never re-read on send."""

import asyncio
import base64
from dataclasses import dataclass
import os
from pathlib import Path
import shlex
import shutil
import sys
import tempfile

from .paste_blocks import PathPaste

MAX_IMAGE_BYTES = 5 * 1024 * 1024
MAX_TOTAL_BYTES = 10 * 1024 * 1024
MAX_IMAGES = 8


@dataclass(frozen=True, eq=False)
class ImagePaste:
    pixels: bytes
    mime: str
    name: str
    origin: str
    width: int
    height: int

    @property
    def label(self) -> str:
        return "[Pasted Image]" if self.origin == "clipboard" else f"[Image • {self.origin}]"

    @property
    def text(self) -> str:
        return self.label

    @property
    def details(self) -> str:
        return f"{self.name}\n{self.mime} • {self.width}×{self.height}\n{len(self.pixels):,} bytes\n{self.origin}\nCaptured at attachment time."

    def payload(self) -> dict:
        return {"mime": self.mime, "data": base64.b64encode(self.pixels).decode("ascii"),
                "name": self.name, "origin": self.origin}


def snapshot(pixels: bytes, name: str, origin: str) -> ImagePaste:
    if not pixels or len(pixels) > MAX_IMAGE_BYTES:
        raise ValueError("Image must contain at most 5 MiB")
    if pixels.startswith(b"\x89PNG\r\n\x1a\n"):
        mime = "image/png"
    elif pixels.startswith(b"\xff\xd8\xff"):
        mime = "image/jpeg"
    elif pixels.startswith(b"RIFF") and pixels[8:12] == b"WEBP":
        mime = "image/webp"
    else:
        raise ValueError("Only PNG, JPEG and WebP images are supported")
    width, height = dimensions(pixels, mime)
    if not width or not height or max(width, height) > 16384 or width * height > 40000000:
        raise ValueError("Image exceeds 16384 pixels per side or 40 megapixels, or has an invalid header")
    return ImagePaste(pixels, mime, name, origin, width, height)


def dimensions(data: bytes, mime: str) -> tuple[int, int]:
    def number(start, count, order="big"):
        if start + count > len(data):
            raise ValueError("Truncated image header")
        return int.from_bytes(data[start:start + count], order)
    if mime == "image/png" and data[12:16] == b"IHDR":
        return number(16, 4), number(20, 4)
    if mime == "image/jpeg":
        p = 2
        while p + 4 <= len(data) and data[p] == 255:
            while p < len(data) and data[p] == 255:
                p += 1
            if p >= len(data):
                break
            marker = data[p]
            p += 1
            if marker in (0xD9, 0xDA):
                break
            length = number(p, 2)
            if length < 2 or p + length > len(data):
                break
            if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                return number(p + 5, 2), number(p + 3, 2)
            p += length
    if mime == "image/webp" and number(4, 4, "little") + 8 == len(data):
        kind = data[12:16]
        if kind == b"VP8X":
            return number(24, 3, "little") + 1, number(27, 3, "little") + 1
        if kind == b"VP8L" and data[20:21] == b"\x2f":
            bits = number(21, 4, "little")
            return (bits & 0x3fff) + 1, ((bits >> 14) & 0x3fff) + 1
        if kind == b"VP8 " and data[23:26] == b"\x9d\x01\x2a":
            return number(26, 2, "little") & 0x3fff, number(28, 2, "little") & 0x3fff
    raise ValueError("Invalid image dimensions")


def pasted_paths(text: str) -> tuple[ImagePaste | PathPaste, ...]:
    """Recognize an entire paste of existing files/directories, not ordinary prose."""
    value = text.strip()
    if not value or "\x00" in value:
        return ()
    try:
        direct = Path(value).expanduser()
        try:
            paths = [direct] if direct.is_file() or direct.is_dir() else [Path(p).expanduser() for p in shlex.split(value)]
        except ValueError:
            return ()  # Unbalanced quotes in ordinary pasted prose are not paths.
        if not paths or any(not (p.is_file() or p.is_dir()) for p in paths):
            return ()
        image_paths = [p for p in paths if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp")]
        if len(image_paths) > MAX_IMAGES:
            raise ValueError("At most 8 images may be attached to a message")
        attachments = []
        for path in paths:
            if path in image_paths:
                with path.open("rb") as stream:
                    attachments.append(snapshot(stream.read(MAX_IMAGE_BYTES + 1), path.name, str(path.absolute())))
            else:
                attachments.append(PathPaste(str(path.absolute())))
        check_budget([item for item in attachments if isinstance(item, ImagePaste)])
        return tuple(attachments)
    except (OSError, RuntimeError):  # Unknown ~user in ordinary pasted text.
        return ()


def check_budget(images) -> None:
    if len(images) > MAX_IMAGES or sum(len(image.pixels) for image in images) > MAX_TOTAL_BYTES:
        raise ValueError("Attach at most 8 images, 5 MiB each and 10 MiB total")


async def _capture(command: list[str]) -> bytes:
    process = await asyncio.create_subprocess_exec(*command, stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
    try:
        data = await asyncio.wait_for(process.stdout.read(MAX_IMAGE_BYTES + 1), 3)
        # read() may return a partial pipe chunk. Continue up to the hard bound.
        while len(data) <= MAX_IMAGE_BYTES:
            more = await asyncio.wait_for(process.stdout.read(MAX_IMAGE_BYTES + 1 - len(data)), 3)
            if not more:
                break
            data += more
        if len(data) > MAX_IMAGE_BYTES:
            raise ValueError("Clipboard image exceeds 5 MiB")
        await asyncio.wait_for(process.wait(), 3)
        return data if process.returncode == 0 else b""
    finally:
        if process.returncode is None:
            process.kill()
            await process.wait()


async def clipboard_image() -> ImagePaste:
    if os.environ.get("SSH_CONNECTION") or os.environ.get("SSH_TTY"):
        raise ValueError("Native clipboard images are unavailable over SSH. Attach a local image path instead.")
    if sys.platform == "darwin" and shutil.which("osascript"):
        with tempfile.TemporaryDirectory(prefix="sos-image-") as directory:
            path = Path(directory) / "clipboard.png"
            script = ('on run argv\nset target to POSIX file (item 1 of argv)\n'
                      'set imageData to the clipboard as «class PNGf»\n'
                      'set output to open for access target with write permission\n'
                      'try\nwrite imageData to output\nclose access output\n'
                      'on error\nclose access output\nend try\nend run')
            await _capture(["osascript", "-e", script, str(path)])
            if path.exists():
                with path.open("rb") as stream:
                    return snapshot(stream.read(MAX_IMAGE_BYTES + 1), "clipboard.png", "clipboard")
    elif sys.platform == "linux":
        for mime in ("image/png", "image/jpeg", "image/webp"):
            if os.environ.get("WAYLAND_DISPLAY") and shutil.which("wl-paste"):
                command = ["wl-paste", "--no-newline", "--type", mime]
            elif os.environ.get("DISPLAY") and shutil.which("xclip"):
                command = ["xclip", "-selection", "clipboard", "-out", "-target", mime]
            else:
                break
            pixels = await _capture(command)
            if pixels:
                return snapshot(pixels, "clipboard." + mime.split("/")[1], "clipboard")
    raise ValueError("No supported clipboard image found. Paste an image path or install native clipboard support.")
