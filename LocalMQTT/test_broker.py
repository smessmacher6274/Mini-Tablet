import asyncio
from pathlib import Path
import socket
import sys
import unittest

from amqtt.session import Session
from server import Service


class BrokerTests(unittest.IsolatedAsyncioTestCase):
    async def test_anonymous_config_without_credentials(self):
        service = Service(Path("unused.sqlite3"), 1883,
                          {"broker_host": "0.0.0.0", "authentication_enabled": False})
        self.assertEqual(service.connect_uri, "mqtt://127.0.0.1:1883/")
        self.assertEqual(len(service.broker.listeners_config), 1)
        self.assertEqual(service.broker.config.plugins, {
            "amqtt.plugins.authentication.AnonymousAuthPlugin": {"allow_anonymous": True}})
        self.assertTrue(await service.broker._authenticate(Session(), None))
        await service.broker.plugins_manager.close()

    @unittest.skipUnless(sys.platform == "win32", "Windows exclusive binding")
    async def test_duplicate_listener_is_rejected(self):
        service = Service(Path("unused.sqlite3"), 1883)
        with socket.socket() as occupied:
            # Reproduce the reuse flag enabled by the older aMQTT server.
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            port = occupied.getsockname()[1]
            with self.assertRaises(OSError):
                await service.broker._create_server_instance("default", "tcp", "127.0.0.1", port, None)
        # Once the first listener closes, the same endpoint can start normally.
        listener = await service.broker._create_server_instance("default", "tcp", "127.0.0.1", port, None)
        listener.close()
        await listener.wait_closed()
        await service.broker.plugins_manager.close()


if __name__ == "__main__":
    unittest.main()
