"""Topic permissions for the server and its single read-only tablet."""
from dataclasses import dataclass
from amqtt.contexts import Action
from amqtt.plugins.base import BaseTopicPlugin

STATE = "notepad/v1/devices/tablet-001/state"
PRESENCE = "notepad/v1/devices/tablet-001/presence"


class TabletACL(BaseTopicPlugin):
    @dataclass
    class Config:
        service_user: str = "notepad-service"
        device_user: str = "tablet-001"

    async def topic_filtering(self, *, session=None, topic=None, action=None):
        user = session.username if session else None
        if action == Action.PUBLISH:
            return ((user == self.config.service_user and topic == STATE) or
                    (user == self.config.device_user and topic == PRESENCE))
        if action in {Action.SUBSCRIBE, Action.RECEIVE}:
            return ((user == self.config.service_user and topic == PRESENCE) or
                    (user == self.config.device_user and topic == STATE))
        return False
