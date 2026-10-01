// units:
/* A defect the parser still has: MAX_PROTOCOL_PAYLOAD_SIZE is `2048 + 64`
 * without parentheses, so a frame's payload[MAX_PROTOCOL_PAYLOAD_SIZE / 2]
 * is 2048 + 32 words, 4,160 bytes instead of 2,112. The firmware keeps four
 * frames in RAM, 8 KB they never use. Fails until the macro is in
 * parentheses. */
#include "tprotocol.h"

#include "test.h"

int main(void) {
  CHECK_EQ(sizeof(((TransmissionProtocol*)0)->payload),
           MAX_PROTOCOL_PAYLOAD_SIZE);
  TEST_END();
}
