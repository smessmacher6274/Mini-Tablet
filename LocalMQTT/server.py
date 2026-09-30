"""Local-only to-do API, SQLite store and embedded MQTT 3.1.1 broker."""
import argparse
import asyncio
import contextlib
import json
import logging
from pathlib import Path
import sqlite3
import uuid
import time
from urllib.parse import quote

from aiohttp import web
from amqtt.broker import Broker
from amqtt.client import MQTTClient

ROOT = Path(__file__).resolve().parent
TOPIC = "notepad/v1/devices/tablet-001/state"
PRESENCE = "notepad/v1/devices/tablet-001/presence"
MAX_ITEMS = 50
MAX_TITLE_BYTES = 120
SERVICE = web.AppKey("service", object)
LOG = logging.getLogger("notepad")


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
        """)

    def snapshot(self):
        rows = self.db.execute("SELECT * FROM items ORDER BY created_at,id").fetchall()
        return {"schema_version": 1, "device_id": "tablet-001", "list_id": "inbox",
                "revision": self.db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0],
                "items": [dict(row) | {"completed": bool(row["completed"])} for row in rows]}

    def next_revision(self):
        self.db.execute("UPDATE meta SET value=value+1 WHERE key='revision'")
        return self.db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0]

    def create(self, body):
        title = title_value(body.get("title"))
        request_id = body.get("request_id")
        try:
            uuid.UUID(request_id)
        except (ValueError, TypeError, AttributeError):
            raise Invalid("request_id must be a UUID.") from None
        with self.db:
            previous = self.db.execute("SELECT * FROM requests WHERE id=?", (request_id,)).fetchone()
            if previous:
                if previous["title"] != title:
                    raise Conflict("This request ID was already used for a different task.")
                return self.snapshot()
            if self.db.execute("SELECT count(*) FROM items").fetchone()[0] >= MAX_ITEMS:
                raise Invalid("This prototype supports up to 50 tasks. Remove a task first.")
            item_id = str(uuid.uuid4())
            revision = self.next_revision()
            self.db.execute("INSERT INTO items(id,title,revision) VALUES(?,?,?)", (item_id, title, revision))
            self.db.execute("INSERT INTO requests VALUES(?,?,?)", (request_id, title, item_id))
        return self.snapshot()

    def change(self, item_id, body, delete=False):
        expected = body.get("base_revision")
        if type(expected) is not int:
            raise Invalid("base_revision must be an integer.")
        with self.db:
            item = self.db.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone()
            if not item:
                raise KeyError("Task not found.")
            if item["revision"] != expected:
                raise Conflict("This task changed in another window. Refresh and try again.")
            if delete:
                self.next_revision()
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
        return self.snapshot()

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
        if lan_config:
            plugins = {
                "amqtt.plugins.authentication.FileAuthPlugin": {"password_file": lan_config["password_file"]},
                "lan_acl.TabletACL": {"service_user": lan_config["service_user"], "device_user": lan_config["device_user"]},
            }
            self.connect_uri = (f'mqtt://{quote(lan_config["service_user"], safe="")}:'
                                f'{quote(lan_config["service_password"], safe="")}@127.0.0.1:{mqtt_port}/')
        listeners = {"default": {"type": "tcp", "bind": f'{lan_config["broker_host"] if lan_config else "127.0.0.1"}:{mqtt_port}', "max_connections": 20}}
        if lan_config and lan_config["broker_host"] != "127.0.0.1":
            listeners["local"] = {"type": "tcp", "bind": f"127.0.0.1:{mqtt_port}", "max_connections": 20}
        self.broker = Broker({
            "listeners": listeners,
            "plugins": plugins,
        })

    async def observe_device(self):
        while True:
            client = MQTTClient(client_id="notepad-presence-observer", config={"auto_reconnect": False})
            try:
                await client.connect(self.connect_uri)
                await client.subscribe([(PRESENCE, 1)])
                while True:
                    message = await client.deliver_message()
                    try:
                        data = json.loads(message.data)
                        if not isinstance(data, dict):
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
    # Restrict HTTP Host/Origin as well as the listener to localhost.
    if request.host.split(":")[0].lower() not in {"127.0.0.1", "localhost"}:
        raise web.HTTPForbidden(text="Localhost only")
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
    app = web.Application(middlewares=[local_only], client_max_size=4096)

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
    args = parser.parse_args()
    logging.basicConfig(level=logging.WARNING)
    LOG.setLevel(logging.INFO)
    print(f"Open http://localhost:{args.port} — Ctrl+C stops both services.", flush=True)
    config = json.loads((ROOT / "data" / "connection.json").read_text()) if args.lan else None
    web.run_app(make_app(args.database, args.mqtt_port, config), host="127.0.0.1", port=args.port)
