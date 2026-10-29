# Interactive USB simulation

This firmware adds a USB-serial test mode for the controller. It uses the same
`ffheater::Controller` as normal operation, while substituting a virtual clock
and serially injected sensor readings. It is not a desktop-only mock.

## Safety

Disconnect the optocoupler/control wiring to the heater remote before uploading
or starting a simulation. GPIO pins still produce real 200 ms HIGH pulses.

Simulation can only start while normal automatic control is inactive and no
button pulse or sensor configuration transaction is pending. It remains active
until restart; this prevents accidental return to ESP-NOW-driven control in the
middle of a test.

## Serial commands

Open PlatformIO's serial monitor at 115200 baud and send one command per line.

```
SIM START
SIM TEMP 8 8
SIM TEMP 8 8
SIM AUTO START
SIM ADVANCE 60
SIM TEMP 8 14
SIM ADVANCE 60
SIM STATUS
SIM AUTO STOP
SIM RESTART
```

`SIM TEMP <room> <duct>` injects one sensor packet at the current virtual time.
Use `nan` for either temperature to model a failed measurement. Two finite
packets are required before that sensor becomes healthy, exactly as in normal
operation.

`SIM ADVANCE <seconds>` advances virtual time without injecting a packet. This
is intentional: advancing 150 seconds after the last packet triggers the
production communication-timeout decision. The command accepts 0 to 86400
seconds per invocation.

`SIM AUTO START` and `SIM AUTO STOP` start or stop the same automatic-control
state machine used in normal operation. `SIM RESTART` restarts the ESP32; after
the reboot normal ESP-NOW sensor control resumes.

## Expected log evidence

The serial monitor shows virtual timestamps for injected values, control
decisions, and the GPIO action directly beside `triggerButton()`:

```
[SIM 00:00:00] control decision: ON
[SIM 00:00:00] GPIO25 HIGH: 電源ON
[SIM 00:00:00] GPIO25 LOW (200ms pulse complete): 電源ON
```

The low transition occurs 200 ms of real ESP32 time after HIGH. A simulation
timestamp remains unchanged unless virtual time is advanced, so the log also
states the real 200 ms pulse completion explicitly.
