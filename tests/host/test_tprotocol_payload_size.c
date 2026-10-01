// units: rp/src/tprotocol.c
/* The parser drops a frame whose payload size is larger than its buffer
 * (MAX_PROTOCOL_PAYLOAD_SIZE) instead of writing past it. Any program that
 * reads ROM3 sequentially sends one: its reads are $ABCD, $ABCE, $ABCF, a
 * header, a command and a size of 43,983 bytes. Under the address sanitizer a
 * write past the buffer fails the test. */
#include "tprotocol.h"

#include "test.h"

host_timer_hw_t host_timer;

static void onCommand(const TransmissionProtocol *p) { (void)p; }
static void onChecksumError(const TransmissionProtocol *p) { (void)p; }

int main(void) {
  uint16_t word = PROTOCOL_HEADER;
  // A sequential read of ROM3 from $FB0000 + $ABCD: every word is the next.
  for (int i = 0; i < 30000; i++) {
    host_timer.timerawl += 1;
    tprotocol_parse(word++, onCommand, onChecksumError);
  }
  CHECK(1);
  TEST_END();
}
