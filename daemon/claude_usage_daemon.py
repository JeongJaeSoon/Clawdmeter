#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import argparse
import json
import os
import re
import signal
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from providers import Provider, Usage
from providers.claude import ClaudeProvider
from providers.codex import CodexProvider

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0

SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_PATH = Path(__file__).with_name("config.toml")
VALID_PROVIDERS = {"claude", "codex", "both"}
DUAL_PROVIDER_KEYS = {"claude": "c", "codex": "x"}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
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


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


def usage_to_payload(usage: Usage) -> dict:
    return {
        "s": usage.session_pct,
        "sr": usage.session_reset_min,
        "w": usage.weekly_pct,
        "wr": usage.weekly_reset_min,
        "st": usage.status,
        "ok": usage.ok,
    }


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected device, or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


def failed_usage(status: str) -> Usage:
    return Usage(0, 0, 0, 0, status, False)


def single_provider_payload(provider: Provider, usage: Usage) -> dict:
    payload = usage_to_payload(usage)
    payload["p"] = provider.name
    return payload


def dual_provider_payload(usages: dict[str, Usage]) -> dict:
    claude_usage = usages.get("claude", failed_usage("claude_missing"))
    payload = usage_to_payload(claude_usage)
    payload["p"] = "both"
    for name, key in DUAL_PROVIDER_KEYS.items():
        payload[key] = usage_to_payload(usages.get(name, failed_usage(f"{name}_missing")))
    return payload


def load_config_provider() -> str | None:
    try:
        raw = CONFIG_PATH.read_text()
    except FileNotFoundError:
        return None
    except OSError as e:
        log(f"Config read failed: {e}")
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


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Clawdmeter BLE usage daemon")
    parser.add_argument(
        "--provider",
        choices=sorted(VALID_PROVIDERS),
        help="Usage provider. Overrides CLAWDMETER_PROVIDER and daemon/config.toml.",
    )
    return parser.parse_args(argv)


def resolve_provider_name(args: argparse.Namespace) -> tuple[str, str]:
    if args.provider:
        return args.provider, "flag"
    env_provider = os.environ.get("CLAWDMETER_PROVIDER", "").strip().lower()
    if env_provider:
        if env_provider in VALID_PROVIDERS:
            return env_provider, "environment"
        log(f"Ignoring invalid CLAWDMETER_PROVIDER={env_provider}")
    config_provider = load_config_provider()
    if config_provider:
        return config_provider, "config"
    return "claude", "default"


def build_providers(name: str) -> list[Provider]:
    if name == "both":
        return [ClaudeProvider(log), CodexProvider(log)]
    if name == "codex":
        return [CodexProvider(log)]
    return [ClaudeProvider(log)]


async def fetch_usage_payload(providers: list[Provider]) -> dict | None:
    if len(providers) == 1:
        usage = await providers[0].fetch_usage()
        if usage is None:
            return None
        return single_provider_payload(providers[0], usage)

    results = await asyncio.gather(
        *(provider.fetch_usage() for provider in providers),
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

    return dual_provider_payload(by_name)


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(
    target,
    stop_event: asyncio.Event,
    providers: list[Provider],
) -> bool:
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
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
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
                payload = await fetch_usage_payload(providers)
                if payload is not None and await session.write_payload(payload):
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


async def main(argv: list[str] | None = None) -> None:
    args = parse_args(sys.argv[1:] if argv is None else argv)
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

    log("=== Clawdmeter Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Provider: {provider_name} ({provider_source})")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
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
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
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
