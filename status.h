/* Helper-to-deck status records. The deck runs a streaming helper with
 * the write end of a pipe on fd 3; the helper sends one record about ten
 * times a second, only in slack before its next chip write, and drops a
 * record rather than wait for pipe space. ELKS pipes hold 80 bytes and
 * write a record of this size whole or not at all.
 *
 *  0-1   'S' 'D'
 *  2     channels with a known level (bit per channel)
 *  3     channels whose level comes from a hardware envelope
 *  4-7   position in the current pass, 44.1 kHz samples, little-endian
 *  8-11  pass length (the VGM total), 44.1 kHz samples
 *  12-17 current level per channel
 *  18-23 highest level since the previous record
 */
#ifndef SIGNALDECK_STATUS_H
#define SIGNALDECK_STATUS_H

#define STATUS_FD 3
#define STATUS_SIZE 24
#define STATUS_CHANNELS 6

#endif
