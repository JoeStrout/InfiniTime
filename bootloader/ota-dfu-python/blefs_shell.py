#!/usr/bin/env python3
"""
Interactive shell for InfiniTime's BLE filesystem, over bleak.

Runs anywhere bleak does: CoreBluetooth on macOS, BlueZ on Linux, WinRT on Windows.

    $ ./blefs_shell.py
    Found InfiniTime (C3764D0D-...)
    BLE FS version 4
    blefs> ls /images
    blefs> cp !fuji.bin /images/fuji.bin
    blefs> rm /infinitime-resources-1.16.0.zip
    blefs> exit

Plain paths refer to the remote (device) filesystem. A local path is prefixed with
`!`, as in ftp/sftp where `!` escapes to the local machine:

    cp !./fuji.bin /images/fuji.bin      upload
    cp /fonts/teko.bin !teko.bin         download
    cp !big.bin /images/                 trailing slash keeps the local basename

Protocol: doc/BLEFS.md, cross-checked against src/components/ble/FSService.h/.cpp.

Firmware quirks this works around -- all verified in the source, and all of them produce
misleading errors in other clients:

  - Success status is 0x01, not 0 (FSService.cpp:102, :246). Negative values are LittleFS
    error codes; see LFS_ERRORS below.
  - The WRITE header sets resp.status ONLY on success (FSService.cpp:172-176), and
    WRITE_DATA sets it ONLY on failure (FSService.cpp:185-198). WriteResponse has no
    default initialisers (FSService.h:120), so in each case the other path returns
    uninitialised stack memory. We therefore never trust status on data writes: uploads
    are tracked by our own byte count and then VERIFIED by re-listing the parent
    directory and comparing the file size.
  - Read and write chunks are not clamped to the connection MTU ("TODO add mtu somehow",
    FSService.cpp:111), so the client must size them or replies get truncated.
  - A directory listing ends with a terminator entry whose path_length is 0
    (FSService.cpp:292-296); it is not a file.
  - Everything here is gated behind Settings -> "Firmware & files" on the watch. When it
    is Disabled, every request is refused and the version characteristic reads 0, not 4.
"""

import argparse
import asyncio
import os
import shlex
import struct
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("bleak is not installed. Run: pip install bleak")


UUID_VERSION = "adaf0100-4669-6c65-5472-616e73666572"
UUID_TRANSFER = "adaf0200-4669-6c65-5472-616e73666572"

CMD_READ = 0x10
CMD_READ_DATA = 0x11
CMD_READ_PACING = 0x12
CMD_WRITE = 0x20
CMD_WRITE_PACING = 0x21
CMD_WRITE_DATA = 0x22
CMD_DELETE = 0x30
CMD_DELETE_STATUS = 0x31
CMD_MKDIR = 0x40
CMD_MKDIR_STATUS = 0x41
CMD_LISTDIR = 0x50
CMD_LISTDIR_ENTRY = 0x51

STATUS_OK = 0x01

READ_RESPONSE_HEADER = 16       # command, status, pad, chunkoff, totallen, chunklen
WRITE_RESPONSE_HEADER = 20      # command, status, pad, offset, modTime, freespace
WRITE_DATA_HEADER = 12          # command, status, pad, offset, dataSize
LISTDIR_RESPONSE_HEADER = 28

LFS_ERRORS = {
    0: "OK",
    -5: "IO — device operation failed",
    -84: "CORRUPT — filesystem is corrupted",
    -2: "NOENT — no such file or directory",
    -17: "EXIST — entry already exists",
    -20: "NOTDIR — entry is not a directory",
    -21: "ISDIR — entry is a directory",
    -39: "NOTEMPTY — directory is not empty",
    -9: "BADF — bad file number",
    -27: "FBIG — file is too large",
    -22: "INVAL — invalid parameter",
    -28: "NOSPC — no space left on device",
    -12: "NOMEM — no more memory available",
    -61: "NOATTR — no data/attr available",
    -36: "NAMETOOLONG — file name too long",
}

HELP = """\
Paths refer to the remote (device) filesystem unless prefixed with ! (local), as in ftp/sftp.

  ls [-r] [PATH]        list a directory on the device (default /)
                          ls /images                        on the device
                          ls !                              local working directory
                          ls !../../build/src/resources     any local path
  cp SRC DST            copy a file; exactly one side must be local (!)
                          cp !fuji.bin /images/fuji.bin     upload
                          cp /fonts/teko.bin !teko.bin      download
                          cp !fuji.bin /images/             keep the local basename
  rm PATH               delete a file on the watch
  mkdir PATH            create a directory on the watch
  df                    show free space on the watch
  help                  this text
  exit                  quit (Ctrl-D also works)
"""


class FsError(Exception):
    pass


def describe_status(status):
    if status == STATUS_OK:
        return "OK"
    return LFS_ERRORS.get(status, f"unrecognised status {status}")


def is_local(path):
    return path.startswith("!")


def local_path(path):
    # A bare "!" means the current local directory, which makes `ls !` and
    # `cp /fonts/teko.bin !` do the obvious thing.
    return os.path.expanduser(path[1:]) or "."


def human(size):
    for unit in ("B", "KiB", "MiB"):
        if size < 1024 or unit == "MiB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size / 1:.0f} {unit}"
        size /= 1024
    return f"{size} B"


class BleFs:
    def __init__(self, client, timeout, verbose):
        self.client = client
        self.timeout = timeout
        self.verbose = verbose
        self.notifications = asyncio.Queue()
        mtu = getattr(client, "mtu_size", 23) or 23
        self.mtu = mtu
        # The firmware does not clamp to the MTU, so we must (FSService.cpp:111).
        self.read_chunk = max(16, mtu - 3 - READ_RESPONSE_HEADER)
        self.write_chunk = max(16, mtu - 3 - WRITE_DATA_HEADER)

    def log(self, message):
        if self.verbose:
            print(f"  [fs] {message}", file=sys.stderr)

    def _on_notify(self, _characteristic, data):
        self.notifications.put_nowait(bytes(data))

    async def start(self):
        await self.client.start_notify(UUID_TRANSFER, self._on_notify)

    async def _response(self):
        try:
            return await asyncio.wait_for(self.notifications.get(), self.timeout)
        except asyncio.TimeoutError:
            raise FsError(
                f"no response within {self.timeout}s — check Settings -> "
                "'Firmware & files' is Enabled on the watch"
            )

    async def _send(self, payload):
        await self.client.write_gatt_char(UUID_TRANSFER, payload, response=True)

    async def version(self):
        raw = await self.client.read_gatt_char(UUID_VERSION)
        return struct.unpack("<H", raw[:2])[0]

    # -- listing -----------------------------------------------------------------

    async def listdir(self, path):
        encoded = path.encode()
        await self._send(struct.pack("<BBH", CMD_LISTDIR, 0, len(encoded)) + encoded)

        entries = []
        while True:
            data = await self._response()
            if len(data) < LISTDIR_RESPONSE_HEADER:
                raise FsError(f"short listdir response: {data.hex()}")
            (command, status, path_len, entry, total,
             flags, _modtime, size) = struct.unpack_from("<BbHIIIQI", data, 0)
            if command != CMD_LISTDIR_ENTRY:
                raise FsError(f"unexpected reply 0x{command:02x} to listdir")
            if status != STATUS_OK:
                raise FsError(f"{path}: {describe_status(status)}")
            if total == 0:
                return entries
            name = data[LISTDIR_RESPONSE_HEADER:LISTDIR_RESPONSE_HEADER + path_len].decode(
                errors="replace"
            )
            # Terminator entry, not a file (FSService.cpp:292-296).
            if path_len and name not in (".", ".."):
                entries.append({"name": name, "size": size, "is_dir": bool(flags & 1)})
            if entry >= total:
                return entries

    async def size_of(self, path):
        """Size of a file on the watch, or None if absent. Used to verify uploads."""
        parent, _, name = path.rpartition("/")
        for item in await self.listdir(parent or "/"):
            if item["name"] == name and not item["is_dir"]:
                return item["size"]
        return None

    # -- reading -----------------------------------------------------------------

    async def read_file(self, path, on_progress=None):
        encoded = path.encode()
        await self._send(
            struct.pack("<BBHII", CMD_READ, 0, len(encoded), 0, self.read_chunk) + encoded
        )

        content = bytearray()
        while True:
            data = await self._response()
            if len(data) < READ_RESPONSE_HEADER:
                raise FsError(f"short read response: {data.hex()}")
            command, status, _pad, offset, totallen, chunklen = struct.unpack_from(
                "<BbHIII", data, 0
            )
            if command != CMD_READ_DATA:
                raise FsError(f"unexpected reply 0x{command:02x} to read")
            if status != STATUS_OK:
                raise FsError(f"{path}: {describe_status(status)}")

            chunk = data[READ_RESPONSE_HEADER:READ_RESPONSE_HEADER + chunklen]
            if len(chunk) != chunklen:
                raise FsError(
                    f"truncated chunk: header claimed {chunklen} bytes, got {len(chunk)}"
                )
            content.extend(chunk)
            if on_progress:
                on_progress(len(content), totallen)

            if len(content) >= totallen or chunklen == 0:
                break

            await self._send(
                struct.pack(
                    "<BBHII", CMD_READ_PACING, STATUS_OK, 0, len(content),
                    min(self.read_chunk, totallen - len(content)),
                )
            )
        return bytes(content)

    # -- writing -----------------------------------------------------------------

    async def write_file(self, path, content, on_progress=None):
        encoded = path.encode()
        header = struct.pack(
            "<BBHIQI", CMD_WRITE, 0, len(encoded), 0, 0, len(content)
        ) + encoded
        await self._send(header)

        data = await self._response()
        if len(data) < WRITE_RESPONSE_HEADER:
            raise FsError(f"short write response: {data.hex()}")
        command, status, _pad, _offset, _modtime, _free = struct.unpack_from(
            "<BbHIQI", data, 0
        )
        if command != CMD_WRITE_PACING:
            raise FsError(f"unexpected reply 0x{command:02x} to write")
        # This one IS meaningful: the header handler assigns status only on success,
        # so anything other than 0x01 means FileOpen failed (and the value is garbage).
        if status != STATUS_OK:
            raise FsError(
                f"could not create {path}: {describe_status(status)}. "
                "Does the parent directory exist? (mkdir it first)"
            )

        sent = 0
        while sent < len(content):
            chunk = content[sent:sent + self.write_chunk]
            await self._send(
                struct.pack("<BBHII", CMD_WRITE_DATA, STATUS_OK, 0, sent, len(chunk))
                + chunk
            )
            # The reply's status is uninitialised on success (FSService.cpp:196-198),
            # so it is deliberately not checked. We just need the round trip for pacing.
            await self._response()
            sent += len(chunk)
            if on_progress:
                on_progress(sent, len(content))

        # Because we cannot trust the status bytes, confirm the result independently.
        actual = await self.size_of(path)
        if actual is None:
            raise FsError(f"upload finished but {path} is not in the directory listing")
        if actual != len(content):
            raise FsError(f"size mismatch: watch has {actual} bytes, sent {len(content)}")
        return actual

    async def delete(self, path):
        encoded = path.encode()
        await self._send(struct.pack("<BBH", CMD_DELETE, 0, len(encoded)) + encoded)
        data = await self._response()
        command, status = struct.unpack_from("<Bb", data, 0)
        if command != CMD_DELETE_STATUS:
            raise FsError(f"unexpected reply 0x{command:02x} to delete")
        if status != STATUS_OK:
            raise FsError(f"{path}: {describe_status(status)}")

    async def mkdir(self, path):
        encoded = path.encode()
        await self._send(
            struct.pack("<BBHIQ", CMD_MKDIR, 0, len(encoded), 0, 0) + encoded
        )
        data = await self._response()
        command, status = struct.unpack_from("<Bb", data, 0)
        if command != CMD_MKDIR_STATUS:
            raise FsError(f"unexpected reply 0x{command:02x} to mkdir")
        if status != STATUS_OK:
            raise FsError(f"{path}: {describe_status(status)}")

    async def freespace(self):
        """Free bytes, learned from a zero-length write probe to a scratch path."""
        encoded = b"/.blefs_probe"
        await self._send(
            struct.pack("<BBHIQI", CMD_WRITE, 0, len(encoded), 0, 0, 0) + encoded
        )
        data = await self._response()
        _cmd, _status, _pad, _off, _mt, free = struct.unpack_from("<BbHIQI", data, 0)
        try:
            await self.delete("/.blefs_probe")
        except FsError:
            pass
        return free


def progress(done, total):
    if not total:
        return
    filled = int(30 * done / total)
    print(f"\r  [{'#' * filled}{'-' * (30 - filled)}] {done}/{total} B", end="", flush=True)


class Shell:
    def __init__(self, fs):
        self.fs = fs

    async def run_line(self, line):
        try:
            parts = shlex.split(line)
        except ValueError as exc:
            print(f"parse error: {exc}")
            return True
        if not parts:
            return True

        command, args = parts[0], parts[1:]
        handlers = {
            "ls": self.cmd_ls, "cp": self.cmd_cp, "rm": self.cmd_rm,
            "mkdir": self.cmd_mkdir, "df": self.cmd_df, "help": self.cmd_help,
        }
        if command in ("exit", "quit"):
            return False
        if command not in handlers:
            print(f"unknown command {command!r}; try 'help'")
            return True
        try:
            await handlers[command](args)
        except FsError as exc:
            print(f"error: {exc}")
        except OSError as exc:
            print(f"local error: {exc}")
        return True

    async def cmd_help(self, _args):
        print(HELP, end="")

    async def cmd_ls(self, args):
        recursive = "-r" in args
        paths = [a for a in args if not a.startswith("-")]
        path = paths[0] if paths else "/"
        if is_local(path):
            self._ls_local(local_path(path), recursive, 0)
        else:
            await self._ls(path, recursive, 0)

    @staticmethod
    def _print_entries(entries, depth):
        for item in sorted(entries, key=lambda e: (not e["is_dir"], e["name"])):
            size = "        -" if item["is_dir"] else f"{item['size']:>9}"
            print(f"{size}  {'  ' * (depth + 1)}{item['name']}{'/' if item['is_dir'] else ''}")
            yield item

    async def _ls(self, path, recursive, depth):
        entries = await self.fs.listdir(path)
        if depth == 0:
            print(path)
        for item in self._print_entries(entries, depth):
            if recursive and item["is_dir"]:
                await self._ls(f"{path.rstrip('/')}/{item['name']}", recursive, depth + 1)

    def _ls_local(self, path, recursive, depth):
        """Same output as _ls, but for the machine running this script."""
        if os.path.isfile(path):
            print(f"{os.path.getsize(path):>9}  {path}")
            return

        entries = []
        with os.scandir(path) as scan:
            for item in scan:
                try:
                    is_dir = item.is_dir()
                    size = 0 if is_dir else item.stat().st_size
                except OSError:
                    # Broken symlink, or something we cannot stat. Still worth listing.
                    is_dir, size = False, 0
                entries.append({"name": item.name, "size": size, "is_dir": is_dir})

        if depth == 0:
            print(os.path.abspath(path))
        for item in self._print_entries(entries, depth):
            if recursive and item["is_dir"]:
                self._ls_local(os.path.join(path, item["name"]), recursive, depth + 1)

    async def cmd_cp(self, args):
        if len(args) != 2:
            raise FsError("usage: cp SRC DST  (exactly one side prefixed with !)")
        src, dst = args
        if is_local(src) == is_local(dst):
            raise FsError(
                "exactly one of SRC and DST must be local (!). "
                "Copying watch-to-watch or local-to-local is not supported."
            )

        if is_local(src):  # upload
            source = local_path(src)
            target = dst
            if target.endswith("/"):
                target += os.path.basename(source)
            with open(source, "rb") as handle:
                content = handle.read()
            print(f"{source} -> {target} ({len(content)} B)")
            written = await self.fs.write_file(target, content, progress)
            print(f"\n  verified {written} B on the watch")
        else:  # download
            source = src
            target = local_path(dst)
            if target.endswith("/") or os.path.isdir(target):
                target = os.path.join(target, os.path.basename(source))
            print(f"{source} -> {target}")
            content = await self.fs.read_file(source, progress)
            with open(target, "wb") as handle:
                handle.write(content)
            print(f"\n  wrote {len(content)} B locally")

    async def cmd_rm(self, args):
        if len(args) != 1:
            raise FsError("usage: rm PATH")
        if is_local(args[0]):
            raise FsError("rm only deletes on the watch; use your own shell for local files")
        await self.fs.delete(args[0])
        print(f"deleted {args[0]}")

    async def cmd_mkdir(self, args):
        if len(args) != 1:
            raise FsError("usage: mkdir PATH")
        if is_local(args[0]):
            raise FsError("mkdir only creates directories on the watch")
        await self.fs.mkdir(args[0])
        print(f"created {args[0]}")

    async def cmd_df(self, _args):
        free = await self.fs.freespace()
        print(f"{free} bytes free ({free / 1024:.1f} KiB)")


async def find_device(name, address, scan_time):
    if address:
        device = await BleakScanner.find_device_by_address(address, timeout=scan_time)
        if device is None:
            raise FsError(f"no device with address/UUID {address}")
        return device
    print(f"Scanning for {name!r}...", file=sys.stderr)
    device = await BleakScanner.find_device_by_filter(
        lambda d, _adv: bool(d.name) and name.lower() in d.name.lower(),
        timeout=scan_time,
    )
    if device is None:
        raise FsError(
            f"no device named {name!r}. Make sure the watch is awake, in range, and not "
            "connected to a phone or browser (BLE allows one central at a time)."
        )
    return device


async def main_async(args):
    device = await find_device(args.name, args.address, args.scan_time)
    print(f"Found {device.name} ({device.address})")

    async with BleakClient(device) as client:
        if client.services.get_characteristic(UUID_TRANSFER) is None:
            raise FsError("no BLE FS transfer characteristic — is this an InfiniTime watch?")

        fs = BleFs(client, args.timeout, args.verbose)
        await fs.start()

        version = await fs.version()
        if version == 0:
            raise FsError(
                "BLE FS version reads 0, which means file access is denied. "
                "On the watch: Settings -> 'Firmware & files' -> Enabled."
            )
        print(f"BLE FS version {version}  (MTU {fs.mtu}: "
              f"{fs.read_chunk} B reads, {fs.write_chunk} B writes)")

        shell = Shell(fs)

        if args.command:
            for line in args.command:
                print(f"blefs> {line}")
                await shell.run_line(line)
            return

        print("Type 'help' for commands, 'exit' to quit.")
        loop = asyncio.get_running_loop()
        while True:
            try:
                line = await loop.run_in_executor(None, input, "blefs> ")
            except EOFError:
                print()
                break
            if not await shell.run_line(line):
                break
        print("Bye.")


def main():
    parser = argparse.ArgumentParser(
        description="Interactive shell for InfiniTime's BLE filesystem.",
    )
    parser.add_argument("-n", "--name", default="InfiniTime", help="advertised name to match")
    parser.add_argument("-a", "--address", default=None, help="specific address/UUID")
    parser.add_argument("--scan-time", type=float, default=10.0)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("-c", "--command", action="append",
                        help="run a command and exit; repeatable")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    try:
        asyncio.run(main_async(args))
    except FsError as exc:
        sys.exit(f"Error: {exc}")
    except KeyboardInterrupt:
        sys.exit("\nInterrupted.")


if __name__ == "__main__":
    main()
