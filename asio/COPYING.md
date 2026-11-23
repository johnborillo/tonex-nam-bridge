# License of the `asio/` component

The whole tonex-nam-bridge project is licensed under the **GNU General Public License,
version 3** ([LICENSE](../LICENSE)). For the ASIO driver in this folder (and its test host
`tests/asio_harness.cpp`) that's also a requirement, not just a choice.

The driver is built against Steinberg's ASIO SDK (2.3.4 or later), which Steinberg offers under
either its proprietary ASIO license or GPLv3. This project uses the **GPLv3 option**, so the
driver built from it (`NAMBridgeASIO.dll`) is GPLv3 as well, and anyone redistributing it must
follow GPLv3. The SDK itself is not part of this repository: download it from Steinberg
(steinberg.net/developers) and extract it to `deps/asiosdk`.

Third-party components keep their own licenses. NeuralAmpModelerCore and libASPL (fetched into
`deps/`) are MIT-licensed, which is compatible with GPLv3.

ASIO is a trademark and software of Steinberg Media Technologies GmbH.
