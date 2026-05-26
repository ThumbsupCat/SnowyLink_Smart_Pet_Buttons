#include <stdint.h>

typedef enum {
    MSG_BUTTON_PRESS = 0x01,
    MSG_BOOT         = 0x02,
} msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t button_id;
    uint16_t battery_mv;
    uint32_t sequence;
    uint8_t reserved[4];
} snowy_msg_t;

static const uint8_t MASTER_MAC[6] = {0x3C, 0x0F, 0x02, 0xDA, 0x72, 0xF4};
// 3C:0F:02:DA:72:F4