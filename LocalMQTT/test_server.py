import asyncio
import json
from pathlib import Path
import socket
import tempfile
import unittest
import uuid

from aiohttp.test_utils import TestClient, TestServer
from aiohttp import BasicAuth
from amqtt.client import MQTTClient
from server import make_app, Store, TOPIC
from server import PRESENCE, COMMANDS, RESULTS
from setup_lan import provision
from urllib.parse import quote


class ServerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.database = Path(self.temp.name) / "tasks.db"
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.mqtt_port = sock.getsockname()[1]
        await self.start()

    async def start(self):
        self.client = TestClient(TestServer(make_app(self.database, self.mqtt_port)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        self.temp.cleanup()

    async def state(self):
        response = await self.client.get("/api/state")
        self.assertEqual(response.status, 200)
        return await response.json()

    async def add(self, title="Buy milk", request_id=None):
        response = await self.client.post("/api/items", json={"title": title, "request_id": request_id or str(uuid.uuid4())})
        self.assertEqual(response.status, 200, await response.text())
        return await response.json()

    async def published(self, revision):
        for _ in range(100):
            state = await self.state()
            if state["transport"]["published_revision"] == revision:
                return
            await asyncio.sleep(.05)
        self.fail("Snapshot was not published")

    async def retained(self):
        subscriber = MQTTClient(client_id="test-" + uuid.uuid4().hex, config={"auto_reconnect": False})
        try:
            await subscriber.connect(f"mqtt://127.0.0.1:{self.mqtt_port}/")
            await subscriber.subscribe([(TOPIC, 1)])
            message = await asyncio.wait_for(subscriber.deliver_message(), 5)
            return json.loads(message.data)
        finally:
            await subscriber.disconnect()

    async def test_crud_duplicate_conflict_and_retained_delivery(self):
        request_id = str(uuid.uuid4())
        first = await self.add(request_id=request_id)
        again = await self.add(request_id=request_id)
        self.assertEqual(first, again)
        item = first["items"][0]
        response = await self.client.patch(f'/api/items/{item["id"]}', json={"completed": True, "base_revision": item["revision"]})
        self.assertEqual(response.status, 200)
        changed = await response.json()
        current = changed["items"][0]
        self.assertTrue(current["completed"])
        stale = await self.client.patch(f'/api/items/{item["id"]}', json={"title": "Stale", "base_revision": item["revision"]})
        self.assertEqual(stale.status, 409)
        await self.published(changed["revision"])
        self.assertEqual(await self.retained(), changed)
        response = await self.client.patch(f'/api/items/{item["id"]}', json={"title": "Buy bread", "base_revision": current["revision"]})
        edited = await response.json()
        current = edited["items"][0]
        self.assertEqual(current["title"], "Buy bread")
        response = await self.client.delete(f'/api/items/{item["id"]}', json={"base_revision": current["revision"]})
        self.assertEqual(response.status, 200)
        deleted = await response.json()
        self.assertEqual(deleted["items"], [])
        await self.published(deleted["revision"])
        self.assertEqual((await self.retained())["items"], [])
        # Retrying the original add must not resurrect a deleted item.
        self.assertEqual((await self.add(request_id=request_id))["items"], [])

    async def test_restart_restores_database_and_retained_snapshot(self):
        state = await self.add("Survive a restart")
        await self.published(state["revision"])
        await self.client.close()
        await self.start()
        await self.published(state["revision"])
        self.assertEqual(await self.retained(), state)

    async def test_mqtt_create_is_idempotent_and_can_be_completed(self):
        request_id = str(uuid.uuid4())
        title = 'Café \\\\ "📝"'
        command = {"op": "create", "title": title, "request_id": request_id}
        device = MQTTClient(client_id="test-create-" + uuid.uuid4().hex,
                            config={"auto_reconnect": False})
        try:
            await device.connect(f"mqtt://127.0.0.1:{self.mqtt_port}/")
            await device.publish(COMMANDS, json.dumps(command, ensure_ascii=False).encode(), qos=1)
            for _ in range(100):
                state = await self.state()
                if state["items"]:
                    break
                await asyncio.sleep(.05)
            self.assertEqual(len(state["items"]), 1)
            item = state["items"][0]
            self.assertEqual(item["id"], request_id)
            self.assertEqual(item["title"], title)

            # Replaying the same create request after a lost acknowledgement
            # must not create another task or advance the saved revision.
            await device.publish(COMMANDS, json.dumps(command, ensure_ascii=False).encode(), qos=1)
            await asyncio.sleep(.15)
            repeated = await self.state()
            self.assertEqual(len(repeated["items"]), 1)
            self.assertEqual(repeated["revision"], state["revision"])

            await device.publish(COMMANDS, json.dumps({
                "id": item["id"], "base_revision": item["revision"], "completed": True,
            }).encode(), qos=1)
            for _ in range(100):
                completed = await self.state()
                if completed["items"][0]["completed"]:
                    break
                await asyncio.sleep(.05)
            self.assertTrue(completed["items"][0]["completed"])
        finally:
            await device.disconnect()

    async def test_command_results_dedup_history_and_stale_conflict(self):
        device = MQTTClient(client_id="test-results-" + uuid.uuid4().hex,
                            config={"auto_reconnect": False})
        try:
            await device.connect(f"mqtt://127.0.0.1:{self.mqtt_port}/")
            self.assertEqual(await device.subscribe([(RESULTS, 1)]), [1])

            async def command_result(command):
                await device.publish(COMMANDS, json.dumps(command).encode(), qos=1)
                message = await asyncio.wait_for(device.deliver_message(), 5)
                self.assertEqual(message.topic, RESULTS)
                return json.loads(message.data)

            create = {"op": "create", "title": "Result task", "request_id": str(uuid.uuid4()),
                      "command_id": str(uuid.uuid4())}
            created_result = await command_result(create)
            self.assertEqual(created_result["status"], "applied")
            await self.published(created_result["revision"])
            created_state = await self.state()
            item = created_state["items"][0]

            replay_result = await command_result(create)
            self.assertEqual(replay_result, created_result)
            replay_state = await self.state()
            self.assertEqual(replay_state["revision"], created_state["revision"])

            update = {"op": "update", "id": item["id"], "base_revision": item["revision"],
                      "completed": True, "command_id": str(uuid.uuid4())}
            update_result = await command_result(update)
            self.assertEqual(update_result["status"], "applied")
            await self.published(update_result["revision"])
            updated_state = await self.state()
            self.assertTrue(updated_state["items"][0]["completed"])

            conflict = {"op": "update", "id": item["id"], "base_revision": item["revision"],
                        "completed": False, "command_id": str(uuid.uuid4())}
            conflict_result = await command_result(conflict)
            self.assertEqual(conflict_result["status"], "conflict")
            self.assertEqual(conflict_result["revision"], updated_state["revision"])
            after_conflict = await self.state()
            self.assertEqual(after_conflict, updated_state)

            history_response = await self.client.get("/api/history")
            self.assertEqual(history_response.status, 200)
            events = (await history_response.json())["events"]
            self.assertEqual([event["outcome"] for event in events], ["conflict", "applied", "applied"])
            self.assertEqual([event["action"] for event in events], ["update", "update", "create"])
            self.assertTrue(all(event["source"] == "tablet" for event in events))
            self.assertEqual(events[0]["command_id"], conflict["command_id"])
            self.assertEqual(events[2]["command_id"], create["command_id"])
        finally:
            await device.disconnect()

    async def test_input_limits_and_origin_protection(self):
        for title in ["", "  ", "x" * 121, "\n", "line\nbreak", "🙂" * 31]:
            response = await self.client.post("/api/items", json={"title": title, "request_id": str(uuid.uuid4())})
            self.assertEqual(response.status, 400)
        response = await self.client.post("/api/items", json=[1,2])
        self.assertEqual(response.status, 400)
        response = await self.client.post("/api/items", json={"title":"No", "request_id":str(uuid.uuid4())}, headers={"Origin":"https://example.com"})
        self.assertEqual(response.status, 403)
        self.assertEqual((await self.state())["items"], [])
        for n in range(50):
            await self.add(f"Task {n}")
        response = await self.client.post("/api/items", json={"title":"Full", "request_id":str(uuid.uuid4())})
        self.assertEqual(response.status, 400)
        self.assertEqual(len((await self.state())["items"]), 50)


class LanTests(unittest.IsolatedAsyncioTestCase):
    async def test_credentials_snapshot_presence_and_acl(self):
        with tempfile.TemporaryDirectory() as directory:
            config = provision("127.0.0.1", Path(directory))
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
            client = TestClient(TestServer(make_app(Path(directory) / "tasks.db", port, config)),
                                auth=BasicAuth(config["web_user"], config["web_password"]))
            await client.start_server()
            device = MQTTClient(client_id="test-tablet", config={"auto_reconnect": False})
            denied = MQTTClient(client_id="test-denied", config={"auto_reconnect": False})
            try:
                with self.assertRaises(Exception):
                    await asyncio.wait_for(denied.connect(f"mqtt://tablet-001:wrong@127.0.0.1:{port}/"), 3)
                await device.connect(f'mqtt://{config["device_user"]}:{quote(config["device_password"])}@127.0.0.1:{port}/')
                granted = await device.subscribe([(TOPIC, 1)])
                self.assertEqual(granted, [1])
                packet = await asyncio.wait_for(device.deliver_message(), 5)
                self.assertEqual(json.loads(packet.data)["items"], [])
                # Other namespaces and writing server state are not device permissions.
                self.assertEqual(await device.subscribe([("unrelated/#", 1)]), [128])
                await device.publish(PRESENCE, b'{"online":true,"revision":0}', qos=1, retain=True)
                for _ in range(60):
                    state = await (await client.get("/api/state")).json()
                    if state["transport"]["device_connected"]:
                        break
                    await asyncio.sleep(.05)
                self.assertTrue(state["transport"]["device_connected"])
                self.assertEqual(state["transport"]["device_revision"], 0)
                await device.publish(PRESENCE, b'{"online":false}', qos=1, retain=True)
                await asyncio.sleep(.1)
                state = await (await client.get("/api/state")).json()
                self.assertFalse(state["transport"]["device_connected"])
                # Tablet completion goes into SQLite and back to HTTP/MQTT.
                response = await client.post("/api/items", json={"title": "Phone task", "request_id": str(uuid.uuid4())})
                state = await response.json()
                item = state["items"][0]
                command = json.dumps({"id": item["id"], "base_revision": item["revision"], "completed": True}).encode()
                await device.publish(COMMANDS, command, qos=1)
                for _ in range(60):
                    state = await (await client.get("/api/state")).json()
                    if state["items"][0]["completed"]:
                        break
                    await asyncio.sleep(.05)
                self.assertTrue(state["items"][0]["completed"])
                revision = state["revision"]
                await device.publish(COMMANDS, command, qos=1)
                await asyncio.sleep(.15)
                state = await (await client.get("/api/state")).json()
                self.assertEqual(state["revision"], revision)
                # The phone page refuses anonymous requests in LAN mode.
                async with client.session.get(client.make_url("/api/state"),
                                               auth=BasicAuth("owner", "wrong")) as response:
                    self.assertEqual(response.status, 401)
            finally:
                await device.disconnect()
                await client.close()


class TabletCommandStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.database = Path(self.temp.name) / "tasks.db"
        self.store = Store(self.database)

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def test_receipt_and_audit_survive_reopen_and_conflict_is_deduplicated(self):
        create = {"op": "create", "title": "Tablet task", "request_id": str(uuid.uuid4()),
                  "command_id": str(uuid.uuid4())}
        created = self.store.apply_tablet_command(create)
        self.assertEqual(created["status"], "applied")
        self.store.close()
        self.store = Store(self.database)
        self.assertEqual(self.store.apply_tablet_command(create), created)
        item = self.store.snapshot()["items"][0]

        stale = {"op": "update", "id": item["id"], "base_revision": item["revision"] - 1,
                 "completed": True, "command_id": str(uuid.uuid4())}
        first_conflict = self.store.apply_tablet_command(stale)
        self.assertEqual(first_conflict["status"], "conflict")
        self.store.close()
        self.store = Store(self.database)
        self.assertEqual(self.store.apply_tablet_command(stale), first_conflict)
        self.assertFalse(self.store.snapshot()["items"][0]["completed"])
        events = self.store.history()
        self.assertEqual([e["outcome"] for e in events].count("conflict"), 1)
        self.assertEqual(events[0]["command_id"], stale["command_id"])
        self.assertEqual({event["source"] for event in events}, {"tablet"})

        # A command ID cannot be reused with a different payload or re-applied.
        changed_payload = dict(create, title="Different title")
        rejected_reuse = self.store.apply_tablet_command(changed_payload)
        self.assertEqual(rejected_reuse["status"], "rejected")
        self.assertEqual(self.store.snapshot()["items"][0]["title"], "Tablet task")

    def test_web_and_tablet_actions_have_distinct_history_origins(self):
        web_request_id = str(uuid.uuid4())
        self.store.create({"title": "Web task", "request_id": web_request_id})
        item = self.store.snapshot()["items"][0]
        self.store.change(item["id"], {"base_revision": item["revision"], "completed": True})
        events = self.store.history()
        self.assertEqual([event["action"] for event in events], ["update", "create"])
        self.assertEqual({event["source"] for event in events}, {"web"})
        self.assertEqual(events[0]["before"], {"title": "Web task", "completed": False})
        self.assertEqual(events[0]["after"], {"title": "Web task", "completed": True})


if __name__ == "__main__":
    unittest.main(verbosity=2)
