# License of the `asio/` component

The NAM Bridge ASIO driver (everything in this `asio/` directory) and its test host
`tests/asio_harness.cpp` are licensed under the **GNU General Public License, version 3**
(GPLv3): <https://www.gnu.org/licenses/gpl-3.0.html>.

They are built against Steinberg's ASIO SDK (2.3.4 or later), which Steinberg offers under
either its proprietary ASIO license or GPLv3. This project uses the **GPLv3 option**, so the
driver built from it is GPLv3 as well. The SDK itself is not part of this repository:
download it from Steinberg (steinberg.net/developers) and extract it to `deps/asiosdk`.

Other components keep their own licenses. NeuralAmpModelerCore (fetched into `deps/`) is
MIT-licensed, which is compatible with GPLv3.

ASIO is a trademark and software of Steinberg Media Technologies GmbH.
