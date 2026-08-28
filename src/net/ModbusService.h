// Modbus TCP — a flat register map, because that is all a PLC or a building
// management head-end wants to see.
//
// Nothing here is discoverable or self-describing; that is the point. A
// Siemens or Beckhoff controller reads holding register 2 and gets the target
// temperature in tenths of a degree, forever, with no JSON parser involved.
//
// Holding registers (function 3 to read, 6/16 to write):
//
//   0   power              0 off, 1 on                          read/write
//   1   mode               0 auto, 1 cool, 2 heat, 3 dry, 4 fan  read/write
//   2   target temperature tenths of a degree, e.g. 235 = 23.5   read/write
//   3   fan speed          0 auto, 1 min … 6 max                 read/write
//   4   vertical swing     0 off, 1 auto                         read/write
//   5   command            write 1 to retransmit the state       write
//
//   10  state revision     increments on every change            read
//   11  runtime            hours the unit has run                read
//   12  starts             how many times it has been switched on read
//
// Writes are applied through the command bus like anything else, so a PLC is
// gated by the client switches and clamped to the unit's limits.
#pragma once

#include <ArduinoJson.h>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace net {

class ModbusService {
 public:
  void begin();
  void loop();
  void reconfigure();

  void onStateChanged(const ac::State &state, src::Source source);

  void statusJson(JsonObject out) const;

 private:
  void publish(const ac::State &state);

  bool started_ = false;
  // Set while writing our own registers, so a value we publish is not read
  // back as if a PLC had written it.
  bool publishing_ = false;
  uint32_t writes_ = 0;
};

extern ModbusService modbusService;

}  // namespace net
