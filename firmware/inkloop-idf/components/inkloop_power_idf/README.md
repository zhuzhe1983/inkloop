# ESP-IDF deep-sleep adapter

`EspDeepSleepAdapter` consumes only `IWakePinCapabilities` and an injectable
ESP sleep function table. `EspBoardWakeCapabilities` is the bridge to the
selected `IBoardAdapter`; on M5 PaperColor C151 that resolves to active-low
GPIO 1 (voice), GPIO 10 (previous), and GPIO 9 (next), mask `0x602`.

The adapter rejects held/duplicate/invalid pins, validates timer conversion,
clears stale wake sources, configures the ESP-IDF v6 EXT1 `ANY_LOW` and timer
sources, and rolls configuration back on every returning failure path. It also
decodes timer, individual key, multi-key, and simultaneous-source wakes.

The system function table is intentionally separate so strict host tests can
exercise every path without calling `esp_deep_sleep_start()`. Product
composition creates `EspBoardWakeCapabilities(board_adapter())`, passes
`systemEspSleepFunctions()`, and invokes it only after all power blockers and
both pre-sleep snapshots are clear.

