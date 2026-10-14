import unittest
from aiohttp import BasicAuth
from server import check_web_login
from server import local_only, SERVICE
from aiohttp import web
from types import SimpleNamespace

class LoginTests(unittest.TestCase):
    def test_missing_wrong_and_correct_credentials(self):
        config = {"web_user": "owner", "web_password": "test-only-password"}
        self.assertEqual(check_web_login(None, config), (False, "credentials_missing"))
        self.assertFalse(check_web_login("malformed", config)[0])
        self.assertFalse(check_web_login(BasicAuth("owner", "wrong").encode(), config)[0])
        self.assertTrue(check_web_login(BasicAuth("owner", "test-only-password").encode(), config)[0])
        self.assertFalse(check_web_login(BasicAuth("owner", "test-only-password").encode(), {})[0])

    def test_unicode_login_and_diagnostics_do_not_expose_secrets(self):
        config = {"web_user": "propriétaire", "web_password": "café-秘密"}
        valid, reason = check_web_login(BasicAuth(config["web_user"], config["web_password"], encoding="utf-8").encode(), config)
        self.assertTrue(valid)
        self.assertNotIn(config["web_password"], reason)

class AnonymousModeTests(unittest.TestCase):
    def test_web_accepts_configuration_without_credentials(self):
        config = {"broker_host": "127.0.0.1", "authentication_enabled": False}
        request = SimpleNamespace(host="localhost", method="GET",
                                  app={SERVICE: SimpleNamespace(lan_config=config)})
        async def handler(request):
            return web.Response(text="OK")
        # This middleware/handler has no I/O: exercise it without opening sockets.
        with self.assertRaises(StopIteration) as result:
            local_only(request, handler).send(None)
        self.assertEqual(result.exception.value.status, 200)

if __name__ == "__main__":
    unittest.main()
