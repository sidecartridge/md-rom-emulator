// units:
/* A frame's payload is MAX_PROTOCOL_PAYLOAD_SIZE bytes. The macro was
 * `2048 + 64` without parentheses, so payload[MAX_PROTOCOL_PAYLOAD_SIZE / 2]
 * was 2048 + 32 words, 4,160 bytes instead of 2,112: the firmware keeps four
 * frames in RAM, 8 KB they never used. */
#include "tprotocol.h"

#include "test.h"

int main(void) {
  CHECK_EQ(sizeof(((TransmissionProtocol*)0)->payload),
           MAX_PROTOCOL_PAYLOAD_SIZE);
  TEST_END();
}
