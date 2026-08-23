import contextlib
import importlib.util
import io
import json
import struct
import sys
import threading
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "papercolor_portal_physical_acceptance.py"
SPEC = importlib.util.spec_from_file_location("portal_acceptance", SCRIPT)
assert SPEC and SPEC.loader
portal = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = portal
SPEC.loader.exec_module(portal)


class PortalFixture:
    password = "saved-wifi-password"
    session = "session_abcdefghijklmnopqrstuvwxyz"
    csrf = "csrf_abcdefghijklmnopqrstuvwxyz"

    def __init__(self):
        self.led = 40
        self.refreshes = 7
        self.events = []
        self.preview = portal.diagnostic_png(400, 600)
        self.items = [
            {
                "id": "original-asset",
                "title": "Original",
                "origin": "inkloop",
                "bytes": len(self.preview),
                "current": True,
                "factoryAsset": False,
                "renderStrategy": "official-quality",
            }
        ]

    def state(self):
        return {
            "firmwareVersion": "0.4.0-beta.test",
            "deviceName": "M5 PaperColor",
            "displayWidth": 400,
            "displayHeight": 600,
            "wifiOnline": True,
            "storageReady": True,
            "storageFreeBytes": 1024,
            "storageTotalBytes": 2048,
            "displayBusy": False,
            "displayTiming": {
                "completedRefreshes": self.refreshes,
                "loadDecodeMs": 1,
                "conversionMs": 2,
                "panelRefreshMs": 3,
                "totalMs": 6,
            },
            "runtimeTelemetry": {},
            "myAi": {"state": "active", "pairingCode": "", "bindingUrl": ""},
            "capabilities": {
                "microphone": True,
                "speaker": True,
                "duplexAudio": True,
                "rgbPixels": 2,
                "removableStorage": True,
                "renderStrategies": [
                    {"id": "official-quality", "displayName": "Official"},
                    {"id": "solid-graphic", "displayName": "Solid"},
                ],
            },
            "settings": {
                "volume": 55,
                "voiceAssistanceEnabled": True,
                "ledMaximumBrightness": self.led,
            },
        }


def handler_for(fixture):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            return

        def send_bytes(self, status, payload, content_type="application/json", cookie=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            if cookie:
                self.send_header("Set-Cookie", cookie)
            self.end_headers()
            self.wfile.write(payload)

        def send_json(self, status, value, cookie=None):
            self.send_bytes(
                status,
                json.dumps(value, separators=(",", ":")).encode(),
                cookie=cookie,
            )

        def body(self):
            return self.rfile.read(int(self.headers.get("Content-Length", "0")))

        def authenticated(self, mutation=False):
            if f"inkloop_session={fixture.session}" not in self.headers.get("Cookie", ""):
                self.send_json(401, {"ok": False, "error": "session_required"})
                return False
            if mutation and self.headers.get("X-Inkloop-CSRF") != fixture.csrf:
                self.send_json(403, {"ok": False, "error": "origin_or_csrf_rejected"})
                return False
            return True

        def queued(self, command):
            fixture.events.append(command)
            self.send_json(
                202,
                {
                    "ok": True,
                    "state": "queued",
                    "requestId": len(fixture.events),
                    "command": command,
                },
            )

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == "/health":
                self.send_json(200, {"ok": True, "service": "inkloop-portal"})
                return
            if not self.authenticated():
                return
            query = urllib.parse.parse_qs(parsed.query)
            if parsed.path == "/api/state":
                self.send_json(200, {"ok": True, "state": fixture.state()})
            elif parsed.path == "/api/album":
                self.send_json(
                    200,
                    {
                        "ok": True,
                        "page": {
                            "cursor": query.get("cursor", [""])[0],
                            "nextCursor": "",
                            "limit": 16,
                            "totalItems": len(fixture.items),
                            "revision": len(fixture.events),
                        },
                        "items": fixture.items,
                    },
                )
            elif parsed.path == "/api/chat":
                self.send_json(
                    200,
                    {
                        "ok": True,
                        "retention": "local",
                        "audioIncluded": False,
                        "page": {"nextAfter": 1, "hasMore": False},
                        "messages": [{"sequence": 1, "role": "assistant", "text": "hello"}],
                    },
                )
            elif parsed.path == "/api/album/preview":
                asset_id = query.get("asset_id", [""])[0]
                if not any(item["id"] == asset_id for item in fixture.items):
                    self.send_json(404, {"ok": False, "error": "asset_missing"})
                else:
                    self.send_bytes(200, fixture.preview, "image/png")
            else:
                self.send_json(404, {"ok": False, "error": "route_not_found"})

        def do_POST(self):
            parsed = urllib.parse.urlsplit(self.path)
            if self.headers.get("Origin") != f"http://127.0.0.1:{self.server.server_port}":
                self.send_json(403, {"ok": False, "error": "origin_rejected"})
                return
            body = self.body()
            if parsed.path == "/api/session":
                fields = urllib.parse.parse_qs(body.decode())
                if fields.get("nonce") != [fixture.password]:
                    self.send_json(401, {"ok": False, "error": "invalid_access_code"})
                    return
                self.send_json(
                    200,
                    {"ok": True, "csrfToken": fixture.csrf, "expiresAt": 900},
                    f"inkloop_session={fixture.session}; Path=/; HttpOnly; SameSite=Strict",
                )
                return
            if not self.authenticated(mutation=True):
                return
            if parsed.path == "/api/album/upload":
                self.assert_png(body)
                title = urllib.parse.parse_qs(parsed.query).get("title", [""])[0]
                fixture.items.append(
                    {
                        "id": "temporary-asset",
                        "title": title,
                        "origin": "upload",
                        "bytes": len(body),
                        "current": False,
                        "factoryAsset": False,
                        "renderStrategy": "official-quality",
                    }
                )
                self.queued("COMMIT_ALBUM_UPLOAD")
                return
            fields = urllib.parse.parse_qs(body.decode())
            if parsed.path == "/api/audio/preview":
                self.queued("PREVIEW_VOLUME")
            elif parsed.path == "/api/settings":
                fixture.led = int(fields["led_brightness"][0])
                self.queued("UPDATE_SETTINGS")
            elif parsed.path == "/api/album/display":
                asset_id = fields["asset_id"][0]
                for item in fixture.items:
                    item["current"] = item["id"] == asset_id
                fixture.refreshes += 1
                self.queued("DISPLAY_ALBUM_ITEM")
            elif parsed.path == "/api/album/render":
                asset_id = fields["asset_id"][0]
                for item in fixture.items:
                    if item["id"] == asset_id:
                        item["renderStrategy"] = fields["render_strategy"][0]
                        if item["current"]:
                            fixture.refreshes += 1
                self.queued("SET_ALBUM_RENDER_STRATEGY")
            elif parsed.path == "/api/album/delete":
                asset_id = fields["asset_id"][0]
                fixture.items[:] = [item for item in fixture.items if item["id"] != asset_id]
                self.queued("DELETE_ALBUM_ITEM")
            else:
                self.send_json(404, {"ok": False, "error": "route_not_found"})

        def assert_png(self, body):
            if not body.startswith(portal.PNG_SIGNATURE):
                raise AssertionError("upload was not PNG")

    return Handler


class PortalAcceptanceTests(unittest.TestCase):
    def test_password_is_interactive_only_and_responses_are_bounded(self):
        source = SCRIPT.read_text()
        self.assertIn("getpass.getpass", source)
        self.assertNotIn('add_argument("--password', source)
        self.assertIn("MAX_JSON_BYTES", source)
        self.assertIn("MAX_PREVIEW_BYTES", source)

    def test_local_base_url_is_fail_closed(self):
        self.assertEqual(portal.local_base_url("http://inkloop.local/"), "http://inkloop.local")
        self.assertEqual(
            portal.local_base_url("http://192.168.4.1:8080"),
            "http://192.168.4.1:8080",
        )
        for value in (
            "https://inkloop.local",
            "http://example.com",
            "http://user:password@inkloop.local",
            "http://inkloop.local/settings",
        ):
            with self.assertRaises(Exception):
                portal.local_base_url(value)

    def test_generated_png_has_exact_dimensions(self):
        value = portal.diagnostic_png(400, 600)
        self.assertTrue(value.startswith(portal.PNG_SIGNATURE))
        length = struct.unpack(">I", value[8:12])[0]
        self.assertEqual(value[12:16], b"IHDR")
        self.assertEqual(struct.unpack(">II", value[16:24]), (400, 600))
        self.assertEqual(length, 13)
        self.assertLess(len(value), 1_500_000)

    def test_full_flow_restores_state_and_deletes_temporary_asset(self):
        fixture = PortalFixture()
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(fixture))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output):
                portal.run_acceptance(
                    portal.PortalClient(f"http://127.0.0.1:{server.server_port}", 2),
                    fixture.password,
                    portal.AcceptanceConfig(2, 0.01, False),
                )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.assertIn("PORTAL PHYSICAL PIPELINE PASS", output.getvalue())
        self.assertEqual(fixture.led, 40)
        self.assertEqual([item["id"] for item in fixture.items], ["original-asset"])
        self.assertTrue(fixture.items[0]["current"])
        self.assertEqual(
            fixture.events,
            [
                "PREVIEW_VOLUME",
                "UPDATE_SETTINGS",
                "COMMIT_ALBUM_UPLOAD",
                "DISPLAY_ALBUM_ITEM",
                "SET_ALBUM_RENDER_STRATEGY",
                "DISPLAY_ALBUM_ITEM",
                "DELETE_ALBUM_ITEM",
                "UPDATE_SETTINGS",
            ],
        )


if __name__ == "__main__":
    unittest.main()
