"""Local-only to-do API, SQLite store and embedded MQTT 3.1.1 broker."""
import argparse
import asyncio
import contextlib
import hashlib
import json
import logging
import os
from pathlib import Path
import sqlite3
import uuid
import time
import secrets
import sys
import socket
from functools import partial
from logging.handlers import RotatingFileHandler
from aiohttp import BasicAuth
from urllib.parse import quote

from aiohttp import web
from amqtt.broker import Broker
from amqtt.client import MQTTClient

ROOT = Path(__file__).resolve().parent
TOPIC = "notepad/v1/devices/tablet-001/state"
PRESENCE = "notepad/v1/devices/tablet-001/presence"
COMMANDS = "notepad/v1/devices/tablet-001/commands"
RESULTS = "notepad/v1/devices/tablet-001/results"
MAX_ITEMS = 50
MAX_TITLE_BYTES = 120
SERVICE = web.AppKey("service", object)
LOG = logging.getLogger("notepad")


class ExclusiveBroker(Broker):
    """Prevent Windows from silently sharing MQTT listeners with another server."""

    async def _create_server_instance(self, listener_name, listener_type, address, port, ssl_context):
        if sys.platform != "win32" or listener_type != "tcp":
            return await super()._create_server_instance(
                listener_name, listener_type, address, port, ssl_context)
        # aMQTT sets SO_REUSEADDR, which permits overlapping TCP binds on Windows.
        # Supply an exclusively bound socket instead; asyncio owns it on success.
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            listener.setblocking(False)
            listener.bind((address or "0.0.0.0", port))
            return await asyncio.start_server(
                partial(self.stream_connected, listener_name=listener_name),
                sock=listener, ssl=ssl_context)
        except BaseException:
            listener.close()
            LOG.error("Cannot bind MQTT listener %s:%s exclusively; check for another running server.",
                      address, port)
            raise


def check_web_login(header, config):
    """Return a safe diagnostic reason; never return or log credential values."""
    if not header:
        return False, "credentials_missing"
    for encoding in ("utf-8", "latin1"):
        try:
            auth = BasicAuth.decode(header, encoding=encoding)
            user = config.get("web_user")
            password = config.get("web_password")
            if not isinstance(user, str) or not isinstance(password, str) or not user or not password:
                return False, "invalid_web_login_configuration"
            if (secrets.compare_digest(auth.login.encode("utf-8"), user.encode("utf-8")) and
                    secrets.compare_digest(auth.password.encode("utf-8"), password.encode("utf-8"))):
                return True, "accepted"
        except (ValueError, UnicodeError):
            continue
    return False, "credentials_rejected"


@web.middleware
async def request_log(request, handler):
    try:
        response = await handler(request)
    except web.HTTPException as exc:
        LOG.info("HTTP %s %r status=%d client=%r", request.method, request.path,
                 exc.status, request.remote)
        raise
    except Exception:
        LOG.exception("HTTP %s %r failed", request.method, request.path)
        raise
    LOG.info("HTTP %s %r status=%d client=%r", request.method, request.path,
             response.status, request.remote)
    return response


def configure_logs(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    formatter = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    console = logging.StreamHandler()
    saved = RotatingFileHandler(path, maxBytes=1_000_000, backupCount=3, encoding="utf-8")
    for handler in (console, saved):
        handler.setFormatter(formatter)
    logging.basicConfig(level=logging.WARNING, handlers=[console, saved], force=True)
    LOG.setLevel(logging.INFO)
    def unhandled(kind, value, trace):
        if not issubclass(kind, KeyboardInterrupt):
            LOG.critical("Server stopped with an unhandled error", exc_info=(kind, value, trace))
    sys.excepthook = unhandled
    LOG.info("Server starting pid=%d; logs=%s", os.getpid(), path.resolve())


class Invalid(ValueError):
    pass


class Conflict(ValueError):
    pass


def title_value(value):
    if not isinstance(value, str):
        raise Invalid("Enter a task title.")
    value = value.strip()
    if not value or len(value.encode("utf-8")) > MAX_TITLE_BYTES:
        raise Invalid("Use a title between 1 and 120 UTF-8 bytes.")
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise Invalid("Task titles must be a single line of text.")
    return value


class Store:
    def __init__(self, path):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
            INSERT OR IGNORE INTO meta VALUES ('revision', 0);
            CREATE TABLE IF NOT EXISTS items (
                id TEXT PRIMARY KEY, title TEXT NOT NULL,
                completed INTEGER NOT NULL DEFAULT 0, revision INTEGER NOT NULL,
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            );
            CREATE TABLE IF NOT EXISTS requests (
                id TEXT PRIMARY KEY, title TEXT NOT NULL, item_id TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS command_receipts (
                command_id TEXT PRIMARY KEY, payload_hash TEXT NOT NULL,
                result_json TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source TEXT NOT NULL, action TEXT NOT NULL, item_id TEXT,
                title_before TEXT, title_after TEXT,
                before_json TEXT, after_json TEXT,
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                revision INTEGER NOT NULL, outcome TEXT NOT NULL,
                command_id TEXT
            );
        """)

    def snapshot(self):
        rows = self.db.execute("SELECT * FROM items ORDER BY created_at,id").fetchall()
        return {"schema_version": 1, "device_id": "tablet-001", "list_id": "inbox",
                "revision": self.db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0],
                "items": [dict(row) | {"completed": bool(row["completed"])} for row in rows]}

    def next_revision(self):
        self.db.execute("UPDATE meta SET value=value+1 WHERE key='revision'")
        return self.db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0]

    @staticmethod
    def _item_data(item):
        if item is None:
            return None
        return {"title": item["title"], "completed": bool(item["completed"])}

    def _audit(self, source, action, item_id, before, after, revision, outcome="applied", command_id=None):
        self.db.execute("""INSERT INTO history
            (source,action,item_id,title_before,title_after,before_json,after_json,revision,outcome,command_id)
            VALUES(?,?,?,?,?,?,?,?,?,?)""",
            (source, action, item_id,
             before["title"] if before else None, after["title"] if after else None,
             json.dumps(self._item_data(before), ensure_ascii=False, separators=(",", ":")) if before else None,
             json.dumps(self._item_data(after), ensure_ascii=False, separators=(",", ":")) if after else None,
             revision, outcome, command_id))

    def history(self, item_id=None, limit=100):
        limit = max(1, min(int(limit), 100))
        if item_id is None:
            rows = self.db.execute("SELECT * FROM history ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        else:
            rows = self.db.execute("SELECT * FROM history WHERE item_id=? ORDER BY id DESC LIMIT ?",
                                   (item_id, limit)).fetchall()
        return [{"id": row["id"], "source": row["source"], "action": row["action"],
                 "item_id": row["item_id"], "title_before": row["title_before"],
                 "title_after": row["title_after"],
                 "before": json.loads(row["before_json"]) if row["before_json"] else None,
                 "after": json.loads(row["after_json"]) if row["after_json"] else None,
                 "created_at": row["created_at"], "revision": row["revision"],
                 "outcome": row["outcome"], "command_id": row["command_id"]} for row in rows]

    def create(self, body, source="web", command_id=None):
        try:
            with self.db:
                return self._create(body, source, command_id)
        except (Invalid, Conflict) as exc:
            if source == "web":
                request_id = body.get("request_id") if isinstance(body, dict) else None
                try:
                    item_id = str(uuid.UUID(request_id))
                except (ValueError, TypeError, AttributeError):
                    item_id = None
                current = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone() if item_id else None
                attempted = {"title": body.get("title"), "completed": False} if isinstance(body, dict) and isinstance(body.get("title"), str) else current
                with self.db:
                    self._audit(source, "create", item_id, current, attempted, self.current_revision(),
                                "conflict" if isinstance(exc, Conflict) else "rejected")
            raise

    def _create(self, body, source="web", command_id=None):
        title = title_value(body.get("title"))
        request_id = body.get("request_id")
        try:
            request_id = str(uuid.UUID(request_id))
        except (ValueError, TypeError, AttributeError):
            raise Invalid("request_id must be a UUID.") from None
        previous = self.db.execute("SELECT * FROM requests WHERE id=?", (request_id,)).fetchone()
        if previous:
            if previous["title"] != title:
                raise Conflict("This request ID was already used for a different task.")
            return self.snapshot()
        if self.db.execute("SELECT count(*) FROM items").fetchone()[0] >= MAX_ITEMS:
            raise Invalid("This prototype supports up to 50 tasks. Remove a task first.")
        # The tablet uses its request UUID as the item UUID so it can
        # reconcile a create if the MQTT acknowledgement is lost.
        item_id = request_id
        revision = self.next_revision()
        self.db.execute("INSERT INTO items(id,title,revision) VALUES(?,?,?)", (item_id, title, revision))
        self.db.execute("INSERT INTO requests VALUES(?,?,?)", (request_id, title, item_id))
        after = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone()
        self._audit(source, "create", item_id, None, after, revision, command_id=command_id)
        return self.snapshot()

    def change(self, item_id, body, delete=False, source="web", command_id=None):
        try:
            with self.db:
                return self._change(item_id, body, delete, source, command_id)
        except (Invalid, Conflict, KeyError) as exc:
            if source == "web":
                current = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone()
                attempted = current
                if current and isinstance(body, dict) and not delete:
                    attempted = {"title": body.get("title", current["title"]),
                                 "completed": body.get("completed", bool(current["completed"]))}
                    if not isinstance(attempted["title"], str) or type(attempted["completed"]) is not bool:
                        attempted = current
                with self.db:
                    self._audit(source, "delete" if delete else "update", item_id,
                                current, attempted, self.current_revision(),
                                "conflict" if isinstance(exc, Conflict) else "rejected")
            raise

    def _change(self, item_id, body, delete=False, source="web", command_id=None):
        expected = body.get("base_revision")
        if type(expected) is not int:
            raise Invalid("base_revision must be an integer.")
        item = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone()
        if not item:
            raise KeyError("Task not found.")
        if item["revision"] != expected:
            raise Conflict("This task changed in another window. Refresh and try again.")
        if delete:
            revision = self.next_revision()
            self._audit(source, "delete", item_id, item, None, revision, command_id=command_id)
            self.db.execute("DELETE FROM items WHERE id=?", (item_id,))
        else:
            if not ({"title", "completed"} & body.keys()):
                raise Invalid("Provide title or completed.")
            title = title_value(body.get("title", item["title"]))
            completed = body.get("completed", bool(item["completed"]))
            if type(completed) is not bool:
                raise Invalid("completed must be true or false.")
            if title != item["title"] or completed != bool(item["completed"]):
                revision = self.next_revision()
                self.db.execute("UPDATE items SET title=?,completed=?,revision=? WHERE id=?",
                                (title, int(completed), revision, item_id))
            else:
                revision = self.db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0]
            after = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone()
            self._audit(source, "update", item_id, item, after, revision, command_id=command_id)
        return self.snapshot()

    def apply_tablet_command(self, body):
        """Apply once and persist the command result with its audit entry."""
        command_id = body.get("command_id") if isinstance(body, dict) else None
        try:
            command_id = str(uuid.UUID(command_id))
        except (ValueError, TypeError, AttributeError):
            return {"command_id": str(command_id or ""), "status": "rejected", "revision": self.current_revision()}
        canonical = json.dumps(body, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        payload_hash = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
        with self.db:
            existing = self.db.execute("SELECT payload_hash,result_json FROM command_receipts WHERE command_id=?",
                                       (command_id,)).fetchone()
            if existing:
                if existing["payload_hash"] == payload_hash:
                    return json.loads(existing["result_json"])
                return {"command_id": command_id, "status": "rejected", "revision": self.current_revision()}

            op = body.get("op")
            action = "create" if op == "create" else "update"
            item_id = body.get("request_id") if op == "create" else body.get("id")
            try:
                if op == "create":
                    if set(body) != {"op", "title", "request_id", "command_id"}:
                        raise Invalid("Malformed create command.")
                    self._create(body, source="tablet", command_id=command_id)
                elif op == "update":
                    if set(body) - {"op", "id", "base_revision", "completed", "title", "command_id"} or not {"op", "id", "base_revision", "command_id"} <= set(body) or not ({"completed", "title"} & set(body)):
                        raise Invalid("Malformed update command.")
                    if not isinstance(item_id, str):
                        raise Invalid("Task id is required.")
                    self._change(item_id, body, source="tablet", command_id=command_id)
                else:
                    raise Invalid("Unknown tablet command.")
                result = {"command_id": command_id, "status": "applied", "revision": self.current_revision()}
            except Conflict:
                result = {"command_id": command_id, "status": "conflict", "revision": self.current_revision()}
                current = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone() if isinstance(item_id, str) else None
                attempted = self._tablet_attempt(current, body)
                self._audit("tablet", action, item_id if isinstance(item_id, str) else None,
                            current, attempted, result["revision"], "conflict", command_id)
            except (Invalid, KeyError):
                result = {"command_id": command_id, "status": "rejected", "revision": self.current_revision()}
                current = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone() if isinstance(item_id, str) else None
                attempted = self._tablet_attempt(current, body)
                if action == "create" and isinstance(body.get("title"), str):
                    attempted = {"title": body["title"], "completed": False}
                self._audit("tablet", action, item_id if isinstance(item_id, str) else None,
                            current, attempted, result["revision"], "rejected", command_id)
            self.db.execute("INSERT INTO command_receipts VALUES(?,?,?)",
                            (command_id, payload_hash, json.dumps(result, separators=(",", ":"))))
            return result

    @staticmethod
    def _tablet_attempt(current, body):
        if current is None:
            return None
        attempted = {"title": body.get("title", current["title"]),
                     "completed": body.get("completed", bool(current["completed"]))}
        if not isinstance(attempted["title"], str) or type(attempted["completed"]) is not bool:
            return current
        return attempted

    def current_revision(self):
        return self.db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0]

    def close(self):
        self.db.close()


class Service:
    def __init__(self, database, mqtt_port, lan_config=None):
        self.database = database
        self.mqtt_port = mqtt_port
        self.store = None
        self.lock = asyncio.Lock()
        self.changed = asyncio.Event()
        self.published_revision = -1
        self.mqtt_ready = False
        self.device_seen = 0
        self.device_revision = -1
        self.lan_config = lan_config
        plugins = {"amqtt.plugins.authentication.AnonymousAuthPlugin": {"allow_anonymous": True}}
        self.connect_uri = f"mqtt://127.0.0.1:{mqtt_port}/"
        if lan_config and lan_config.get("authentication_enabled", True):
            plugins = {
                "amqtt.plugins.authentication.FileAuthPlugin": {"password_file": lan_config["password_file"]},
                "lan_acl.TabletACL": {"service_user": lan_config["service_user"], "device_user": lan_config["device_user"]},
            }
            self.connect_uri = (f'mqtt://{quote(lan_config["service_user"], safe="")}:'
                                f'{quote(lan_config["service_password"], safe="")}@127.0.0.1:{mqtt_port}/')
        listeners = {"default": {"type": "tcp", "bind": f'{lan_config["broker_host"] if lan_config else "127.0.0.1"}:{mqtt_port}', "max_connections": 20}}
        if lan_config and lan_config["broker_host"] not in {"127.0.0.1", "0.0.0.0"}:
            listeners["local"] = {"type": "tcp", "bind": f"127.0.0.1:{mqtt_port}", "max_connections": 20}
        self.broker = ExclusiveBroker({
            "listeners": listeners,
            "plugins": plugins,
        })

    async def observe_device(self):
        while True:
            client = MQTTClient(client_id="notepad-presence-observer", config={"auto_reconnect": False})
            try:
                await client.connect(self.connect_uri)
                await client.subscribe([(PRESENCE, 1), (COMMANDS, 1)])
                while True:
                    message = await client.deliver_message()
                    try:
                        data = json.loads(message.data)
                        if not isinstance(data, dict):
                            continue
                        if message.topic == COMMANDS:
                            async with self.lock:
                                if "command_id" in data:
                                    result = self.store.apply_tablet_command(data)
                                    await client.publish(RESULTS,
                                        json.dumps(result, separators=(",", ":")).encode(),
                                        qos=1, retain=False)
                                    self.changed.set()
                                else:
                                    # Preserve the original command shape for older firmware.
                                    try:
                                        if data.get("op") == "create":
                                            self.store.create(data, source="tablet")
                                        else:
                                            self.store.change(data.get("id"), data, source="tablet")
                                        self.changed.set()
                                    except (Invalid, Conflict, KeyError):
                                        LOG.warning("Legacy tablet change rejected")
                            continue
                        self.device_seen = time.monotonic() if data.get("online") is True else 0
                        revision = data.get("revision", -1)
                        if type(revision) is int:
                            self.device_revision = revision
                    except (ValueError, UnicodeError):
                        pass
            except asyncio.CancelledError:
                raise
            except Exception:
                self.device_seen = 0
                LOG.warning("Device status connection lost; retrying")
                await asyncio.sleep(2)
            finally:
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(client.disconnect(), 2)

    async def publisher(self):
        """Republish SQLite state after restarts; retry latest state on failure."""
        while True:
            client = MQTTClient(client_id="notepad-state-publisher", config={"auto_reconnect": False})
            try:
                await client.connect(self.connect_uri)
                self.changed.set()
                while True:
                    await self.changed.wait()
                    self.changed.clear()
                    async with self.lock:
                        state = self.store.snapshot()
                    data = json.dumps(state, ensure_ascii=False, separators=(",", ":")).encode()
                    await asyncio.wait_for(client.publish(TOPIC, data, qos=1, retain=True), 5)
                    self.published_revision = state["revision"]
                    self.mqtt_ready = True
                    # Re-publish periodically as well as on edits to check the connection.
                    try:
                        await asyncio.wait_for(self.changed.wait(), 10)
                    except asyncio.TimeoutError:
                        self.changed.set()
            except asyncio.CancelledError:
                raise
            except Exception:
                self.mqtt_ready = False
                LOG.exception("MQTT publish failed; task data is saved; retrying")
                await asyncio.sleep(2)
            finally:
                self.mqtt_ready = False
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(client.disconnect(), 2)


@web.middleware
async def local_only(request, handler):
    # Only configured LAN/loopback hosts and same-origin mutations are accepted.
    config = request.app[SERVICE].lan_config
    allowed = {"127.0.0.1", "localhost"}
    if config:
        allowed.add(config["broker_host"])
    if request.host.split(":")[0].lower() not in allowed:
        LOG.warning("Request blocked: unknown HTTP Host (check the configured broker_host)")
        raise web.HTTPForbidden(text="Unknown server address")
    if config and config.get("authentication_enabled", True) and config.get("web_password"):
        valid, reason = check_web_login(request.headers.get("Authorization"), config)
        if not valid:
            LOG.warning("Web login failed: %s; client=%r", reason, request.remote)
            raise web.HTTPUnauthorized(headers={"WWW-Authenticate": 'Basic realm="Note tablet", charset="UTF-8"'})
    if request.method not in {"GET", "HEAD"}:
        if request.headers.get("Origin") not in {None, f"http://{request.host}"}:
            raise web.HTTPForbidden(text="Same-origin requests only")
        if request.content_type != "application/json":
            raise web.HTTPUnsupportedMediaType(text="Use application/json")
    try:
        response = await handler(request)
    except (Invalid, json.JSONDecodeError) as exc:
        response = web.json_response({"error": str(exc)}, status=400)
    except Conflict as exc:
        response = web.json_response({"error": str(exc)}, status=409)
    except KeyError:
        response = web.json_response({"error": "Task not found."}, status=404)
    response.headers["Cache-Control"] = "no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'"
    return response


async def read_state(request):
    s = request.app[SERVICE]
    async with s.lock:
        state = s.store.snapshot()
    state["transport"] = {"mqtt_connected": s.mqtt_ready, "published_revision": s.published_revision,
                          "topic": TOPIC, "mqtt_port": s.mqtt_port,
                          "device_connected": s.device_seen > 0 and time.monotonic() - s.device_seen < 45,
                          "device_revision": s.device_revision,
                          "broker_host": s.lan_config["broker_host"] if s.lan_config else "127.0.0.1"}
    return web.json_response(state)


async def read_history(request):
    s = request.app[SERVICE]
    item_id = request.query.get("item_id")
    async with s.lock:
        events = s.store.history(item_id=item_id)
    return web.json_response({"events": events})


async def mutate(request):
    body = await request.json()
    if not isinstance(body, dict):
        raise Invalid("Request must be a JSON object.")
    s = request.app[SERVICE]
    async with s.lock:
        if request.method == "POST":
            state = s.store.create(body)
        else:
            state = s.store.change(request.match_info["item_id"], body, request.method == "DELETE")
        s.changed.set()
    return web.json_response(state)


def make_app(database=ROOT / "data" / "tasks.sqlite3", mqtt_port=1883, lan_config=None):
    app = web.Application(middlewares=[request_log, local_only], client_max_size=4096)

    async def lifecycle(app):
        s = Service(database, mqtt_port, lan_config)
        app[SERVICE] = s
        s.store = Store(database)
        publisher = None
        observer = None
        started = False
        try:
            await s.broker.start()
            started = True
            publisher = asyncio.create_task(s.publisher())
            observer = asyncio.create_task(s.observe_device())
            LOG.info("MQTT: mqtt://127.0.0.1:%d | topic: %s", mqtt_port, TOPIC)
            yield
        finally:
            if observer:
                observer.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await observer
            if publisher:
                publisher.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await publisher
            if started:
                await s.broker.shutdown()
            s.store.close()

    async def index(request):
        return web.FileResponse(ROOT / "static" / "index.html")

    async def asset(request):
        name = request.match_info["name"]
        if name not in {"app.js", "style.css"}:
            raise web.HTTPNotFound()
        return web.FileResponse(ROOT / "static" / name)

    app.cleanup_ctx.append(lifecycle)
    app.router.add_get("/", index)
    app.router.add_get("/static/{name}", asset)
    app.router.add_get("/api/state", read_state)
    app.router.add_get("/api/history", read_history)
    app.router.add_post("/api/items", mutate)
    app.router.add_patch("/api/items/{item_id}", mutate)
    app.router.add_delete("/api/items/{item_id}", mutate)
    return app


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--database", type=Path, default=ROOT / "data" / "tasks.sqlite3")
    parser.add_argument("--lan", action="store_true", help="Enable authenticated LAN MQTT using data/connection.json")
    parser.add_argument("--log-file", type=Path, default=ROOT / "data" / "server.log")
    args = parser.parse_args()
    configure_logs(args.log_file)
    config = json.loads((ROOT / "data" / "connection.json").read_text()) if args.lan else None
    if config and config.get("authentication_enabled", True) and not config.get("web_password"):
        LOG.error("LAN configuration has no webpage password; run setup_lan.py")
        raise SystemExit("Run setup_lan.py again to provision the phone-page login.")
    host = config["broker_host"] if config else "localhost"
    if config:
        LOG.info("Webpage and MQTT authentication: %s",
                 "enabled" if config.get("authentication_enabled", True) else "disabled")
        LOG.info("Loaded login configuration from %s (modified %s); restart after editing",
                 ROOT / "data" / "connection.json",
                 time.strftime("%Y-%m-%d %H:%M:%S", time.localtime((ROOT / "data" / "connection.json").stat().st_mtime)))
    LOG.info("Task page http://%s:%d | MQTT port %d | database %s", host, args.port, args.mqtt_port, args.database)
    print(f"Open http://{host}:{args.port} — Ctrl+C stops both services.", flush=True)
    web.run_app(make_app(args.database, args.mqtt_port, config),
                host="0.0.0.0" if config else "127.0.0.1", port=args.port, access_log=None)
