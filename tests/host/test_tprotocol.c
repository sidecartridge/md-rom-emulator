// units: rp/src/tprotocol.c
/* The ROM3 command parser (rp/src/include/tprotocol.h): a keystroke frame as
 * the ST sends it, a frame with a bad checksum, and a frame cut short by a
 * silence longer than the parser waits. */
#include "tprotocol.h"

#include "test.h"

host_timer_hw_t host_timer;

static int commands;
static int checksumErrors;
static uint16_t lastCommand;
static uint16_t lastSize;
static uint16_t lastPayload[4];

static void onCommand(const TransmissionProtocol *p) {
  commands++;
  lastCommand = p->command_id;
  lastSize = p->payload_size;
  memcpy(lastPayload, p->payload, sizeof(lastPayload));
}

static void onChecksumError(const TransmissionProtocol *p) {
  (void)p;
  checksumErrors++;
}

// Each word as the ST sends it: a few microseconds apart.
static void sendWords(const uint16_t *words, size_t count) {
  for (size_t i = 0; i < count; i++) {
    host_timer.timerawl += 3;
    tprotocol_parse(words[i], onCommand, onChecksumError);
  }
}

// header, command, payload size, token (2 words), key (2 words), checksum
static void keystrokeFrame(uint16_t frame[8], char key) {
  const uint16_t body[] = {0x0001, 8, 0x1234, 0x5678, (uint16_t)key, 0};
  frame[0] = PROTOCOL_HEADER;
  uint16_t sum = 0;
  for (size_t i = 0; i < 6; i++) {
    frame[i + 1] = body[i];
    sum += body[i];
  }
  frame[7] = sum;
}

int main(void) {
  uint16_t frame[8];

  keystrokeFrame(frame, 'b');
  sendWords(frame, 8);
  CHECK_EQ(commands, 1);
  CHECK_EQ(lastCommand, 1);
  CHECK_EQ(lastSize, 8);
  CHECK_EQ(lastPayload[0], 0x1234);
  CHECK_EQ(lastPayload[1], 0x5678);
  CHECK_EQ(lastPayload[2], 'b');
  CHECK_EQ(checksumErrors, 0);

  keystrokeFrame(frame, 'c');
  frame[7] ^= 1;
  sendWords(frame, 8);
  CHECK_EQ(commands, 1);
  CHECK_EQ(checksumErrors, 1);

  // Half a frame, then a silence longer than the parser waits: the next
  // frame is read from its header, not as the rest of the first.
  keystrokeFrame(frame, 'd');
  sendWords(frame, 4);
  host_timer.timerawl += PROTOCOL_READ_RESTART_MICROSECONDS + 1;
  keystrokeFrame(frame, 'e');
  sendWords(frame, 8);
  CHECK_EQ(commands, 2);
  CHECK_EQ(lastPayload[2], 'e');

  TEST_END();
}
