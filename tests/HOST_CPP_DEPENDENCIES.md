# Host C++ test dependencies

The PaperColor primitive harness compiles production C++ headers with the host
compiler. It uses ArduinoJson 7.4.3, matching the exact PlatformIO dependency in
`firmware/m5-papercolor/platformio.ini`.

The harness does **not** read the ignored `firmware/m5-papercolor/.pio` cache.
Its auditable declaration is `tests/fixtures/host-cpp-dependencies.json`,
which pins:

- the upstream version and peeled Git tag commit;
- the official upstream release URL;
- the committed source archive's exact byte length and SHA-256 digest;
- the extracted package metadata and source-file count; and
- the upstream MIT license identifier and license-file digest.

`tests/vendor/arduinojson-7.4.3-src.tar.gz` is the content-addressed reviewed
source package used only by the host harness. The helper verifies the archive
before extraction, parses the verified tar bytes itself, and permits only
regular files and directories under the reviewed member roots. Symbolic and
hard links, devices, FIFOs, sockets, extension records, duplicate paths, and
other special member types are rejected before `tar -x` runs. Extraction reads
the same verified bytes through stdin, after which the helper verifies the
package and license metadata and exposes `src/` from a random OS temporary
directory. The directory is removed after the compile. No network or ignored
`.pio` tree can satisfy or influence the test.

Run the focused clean dependency gate with:

```sh
node --test \
  tests/esp-idf-reproducible-build.test.mjs \
  tests/papercolor-firmware-primitives.test.mjs
```

The focused host compile is self-contained and performs no network access.
PlatformIO separately resolves the same exact version for the legacy Arduino
firmware build.

Version 7.4.3 is the upstream security release that fixes the numeric
string-to-float buffer overrun disclosed in ArduinoJson issue 2220. Pinning the
patched release keeps both the host harness and the legacy network JSON parser
off the affected 7.4.2 code path.
