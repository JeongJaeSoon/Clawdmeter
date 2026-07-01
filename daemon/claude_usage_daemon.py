#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude and/or Codex usage via provider modules and writes a JSON
payload to the ESP32 "Clawdmeter" peripheral over a custom GATT service.
Uses bleak (CoreBluetooth backend on macOS).
"""

import argparse
import asyncio
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from providers import Usage
from providers.claude import ClaudeProvider
from providers.codex import CodexProvider

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_PATH = Path(__file__).with_name("config.toml")
VALID_PROVIDERS = {"claude", "codex", "both"}
DUAL_PROVIDER_KEYS = {"claude": "c", "codex": "x"}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---- Provider resolution --------------------------------------------------

def load_config_provider() -> str | None:
    """Read provider from daemon/config.toml if present."""
    try:
        raw = CONFIG_PATH.read_text()
    except (FileNotFoundError, OSError):
        return None
    for line in raw.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or not line.startswith("provider"):
            continue
        key, sep, value = line.partition("=")
        if sep and key.strip() == "provider":
            provider = value.strip().strip('"').strip("'").lower()
            if provider in VALID_PROVIDERS:
                return provider
            log(f"Ignoring invalid provider in config: {provider}")
    return None


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Clawdmeter BLE usage daemon")
    parser.add_argument(
        "--provider",
        choices=sorted(VALID_PROVIDERS),
        help="Usage provider. Overrides CLAWDMETER_PROVIDER and daemon/config.toml.",
    )
    return parser.parse_args(argv)


def resolve_provider_name(args: argparse.Namespace) -> tuple[str, str]:
    """Return (provider_name, source) using priority: flag > env > config > default."""
    if args.provider:
        return args.provider, "flag"
    env_val = os.environ.get("CLAWDMETER_PROVIDER", "").strip().lower()
    if env_val in VALID_PROVIDERS:
        return env_val, "environment"
    if env_val:
        log(f"Ignoring invalid CLAWDMETER_PROVIDER={env_val}")
    config_val = load_config_provider()
    if config_val:
        return config_val, "config"
    return "claude", "default"


def build_providers(name: str) -> list:
    """Instantiate the requested provider(s)."""
    if name == "both":
        return [ClaudeProvider(log), CodexProvider(log)]
    if name == "codex":
        return [CodexProvider(log)]
    return [ClaudeProvider(log)]


# ---- Payload helpers -------------------------------------------------------

def usage_to_payload(usage: Usage) -> dict:
    return {
        "s": usage.session_pct,
        "sr": usage.session_reset_min,
        "w": usage.weekly_pct,
        "wr": usage.weekly_reset_min,
        "st": usage.status,
        "ok": usage.ok,
    }


def failed_usage(status: str) -> Usage:
    return Usage(0, 0, 0, 0, status, False)


async def fetch_usage_payload(providers: list) -> dict | None:
    """Fetch usage from one or more providers and build the BLE payload."""
    if len(providers) == 1:
        usage = await providers[0].fetch_usage()
        if usage is None:
            return None
        payload = usage_to_payload(usage)
        payload["p"] = providers[0].name
        return payload

    # Dual mode: gather from all providers concurrently
    results = await asyncio.gather(
        *(p.fetch_usage() for p in providers),
        return_exceptions=True,
    )
    by_name: dict[str, Usage] = {}
    for provider, result in zip(providers, results):
        if isinstance(result, Exception):
            log(f"{provider.name} usage fetch failed: {result}")
            by_name[provider.name] = failed_usage(f"{provider.name}_error")
        elif result is None:
            by_name[provider.name] = failed_usage(f"{provider.name}_error")
        else:
            by_name[provider.name] = result

    # Build dual payload with "c" and "x" keys
    claude_usage = by_name.get("claude", failed_usage("claude_missing"))
    payload = usage_to_payload(claude_usage)
    payload["p"] = "both"
    for name, key in DUAL_PROVIDER_KEYS.items():
        payload[key] = usage_to_payload(
            by_name.get(name, failed_usage(f"{name}_missing"))
        )
    return payload


def read_config_dirs() -> list[Path]:
    """Claude config dirs to poll, from the `config_dirs` option (comma list).

    Defaults to [~/.claude] so existing single-plan setups are unchanged. ~ is
    expanded. Mirrors the Linux bash daemon's read_config_dirs.
    """
    raw = ""
    try:
        if CONFIG_PATH.exists():
            for line in CONFIG_PATH.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    raw = val.strip()
    except OSError:
        pass
    if not raw:
        return [DEFAULT_CONFIG_DIR]
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


# ---- Address cache ---------------------------------------------------------

def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID.
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


# ---- macOS CoreBluetooth recovery ------------------------------------------

_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    macOS auto-connects the firmware's HID link, so CoreBluetooth excludes
    it from BleakScanner results. Use retrieveConnectedPeripheralsWithServices_
    instead to find peripherals the OS already holds.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager
    peripherals = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )

    for p in peripherals:
        if p.name() and DEVICE_NAME.lower() in p.name().lower():
            addr = p.identifier().UUIDString()
            if skip_addr and addr == skip_addr:
                continue
            log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
            return BLEDevice(addr, p.name(), (p, manager))

    # Fallback: match by name without service UUID filter.
    peripherals = cm.retrieveConnectedPeripherals_()
    for p in peripherals:
        if p.name() and DEVICE_NAME.lower() in p.name().lower():
            addr = p.identifier().UUIDString()
            if skip_addr and addr == skip_addr:
                continue
            log(f"Found system-connected peripheral (no UUID filter): {p.name()!r} [{addr}]")
            return BLEDevice(addr, p.name(), (p, manager))

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Linux: use cached address or scan by name.
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
        for d in devices:
            if d.name == DEVICE_NAME and (skip_addr is None or d.address != skip_addr):
                log(f"Found via scan: {d.address}")
                return d
        return None

    # Linux: try cached address first, then scan.
    addr = load_cached_address()
    if addr:
        devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
        for d in devices:
            if d.address == addr and d.name == DEVICE_NAME:
                log(f"Found cached device: {d.address}")
                return d.address

    # Scan for device by name.
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            save_address(d.address)
            log(f"Found via scan: {d.address}")
            return d.address
    return None


# ---- Session ---------------------------------------------------------------

class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """Poll every configured config dir and return the active plan's payload.

    Returns None when no dir yields a usable payload this cycle. A single
    configured dir (the default) collapses to exactly the old single-poll path.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        payload = await poll_api(token)
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    return payloads[active]


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Bound it.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


# ---- macOS encryption / unpair helpers -------------------------------------

def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch."""
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout."""
    try:
        out = subprocess.run(
            ["blueutil", *args],
            check=True, capture_output=True, text=True, timeout=BLUEUTIL_TIMEOUT,
        )
        return out.stdout.strip()
    except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        log(f"blueutil failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`.
    The daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`.
    """
    paired = _blueutil("--paired")
    if not paired or int(paired) != 1:
        log("No paired devices found for unpair")
        return False

    devices_str = _blueutil("-l")
    if not devices_str:
        return False

    for line in devices_str.splitlines():
        parts = line.rsplit("\t", 2)
        if len(parts) < 3:
            continue
        name = parts[0].strip()
        if DEVICE_NAME.lower() in name.lower():
            addr = parts[1].strip()
            log(f"Unpairing {name} ({addr})...")
            ok = _blueutil("-f", addr)
            if ok is not None:
                log("Unpaired successfully")
                return True
            else:
                log("Unpair failed")
                return False

    log(f"No paired device matching '{DEVICE_NAME}' found")
    return False


# ---- Connection loop -------------------------------------------------------

async def connect_and_run(target, stop_event: asyncio.Event, providers: list) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect — unbounded await wedges on half-open peripherals.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                payload = await poll_active_payload()
                if payload is None:
                    log("No usable config dir this cycle")
                elif await session.write_payload(payload):
                    last_poll = time.time()
                    used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


# ---- Main ------------------------------------------------------------------

async def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    provider_name, provider_source = resolve_provider_name(args)
    providers = build_providers(provider_name)

    log(f"=== Clawdmeter Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Provider: {provider_name} ({provider_source})")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event, providers)
        if not ok:
            if sys.platform == "darwin":
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
