#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "service.h"
#include "display.h"

static btstack_packet_callback_registration_t event_registration;
static const uint8_t advertisement[] = {
    2, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    10, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'P', 'i', 'c', 'o', 'W', '-', 'B', 'L', 'E'
};

static void packet_handler(uint8_t type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void) channel;
    (void) size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE: {
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            bd_addr_t unused_address = {0};
            gap_advertisements_set_params(0x0030, 0x0060, 0, 0,
                                          unused_address, 0x07, 0);
            gap_advertisements_set_data(sizeof(advertisement), (uint8_t *) advertisement);
            gap_advertisements_enable(1);
            printf("BLE ready: scan for PicoW-BLE\n");
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            gap_advertisements_enable(1);
            printf("Disconnected; advertising again\n");
            break;
        default:
            break;
    }
}

int main(void) {
    stdio_init_all();
    // Draw once before starting the radio. No slow drawing in BLE callbacks.
    display_hello_world();
    if (cyw43_arch_init()) {
        printf("CYW43 initialization failed\n");
        return 1;
    }
    l2cap_init();
    sm_init();
    att_server_init(profile_data, NULL, NULL);
    event_registration.callback = packet_handler;
    hci_add_event_handler(&event_registration);
    hci_power_control(HCI_POWER_ON);
    // Service the SDK's polling async context, including Bluetooth events.
    while (true) {
        display_hello_world();
        // async_context_poll(cyw43_arch_async_context());
        // async_context_wait_for_work_until(cyw43_arch_async_context(), at_the_end_of_time);
    }
}
