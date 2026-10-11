# Gateway.Grpc 0.1.0

Project-wide documentation is maintained in the separate **Docs Git repository**. Before changing this module, explicitly read [Docs/AGENTS.md](../Docs/AGENTS.md), [architecture](../Docs/ARCHITECTURE.md), [module/build policy](../Docs/MODULE_STRUCTURE.md), [local sources](../Docs/LOCAL_SOURCES.md), and [code style](../Docs/CODE_STYLE.md) with its [owner export](../Docs/CodeStyle.xml). The shared instructions are not discovered automatically across repository boundaries; do not create a competing AGENTS.md in the parent workspace. Historical reports and reproduction guides are indexed under [Docs/Tasks/](../Docs/Tasks/README.md), with run receipts under [Docs/Evidence/](../Docs/Evidence/README.md).


Generic injected-provider C++ gRPC server; no Sea Power, C#, host ABI, loader or P/Invoke dependency. Inject an IProvider and logger, keep them alive for the server, and Start/Stop on the host I/O worker. Loopback only.

The provider publishes `Gateway.Api::ProviderObservation`: an order-free `World.Api::ObserverSnapshot` and a separate per-observation `Orders.Api::OrderAvailability` overlay. `GetView` returns only that observer snapshot; `DiscoverOrders` returns scoped availability coverage and typed selectors. Snapshot observations, discovery and requests use `Observations.Api::ObservationId` scoped by `ObserverId` and session; identity never grants control. No public response includes authoritative `UnitId`, simulation truth or truth/contact correlations. The mock exercises false/ambiguous observation handles without exposing its internal authority fixture.

Production library lives in src/. Gateway.Grpc.Mock is an interface target in mock/ for the header-only domain mock. app/mock-host.exe uses the same server, listens at its supplied loopback endpoint and exits after 120 s. test/gateway-tests.exe covers observer isolation/current authority, unknown/partial/complete-empty discovery, malformed requests, duplicate/session/protected/retention and real gRPC connectivity. Route replace/append behavior and route-head-only synthetic movement remain operational; mock displacement is not game evidence. Contact measurement time remains unknown, rather than copying snapshot capture time.

## Build structure

Root CMakeLists.txt initializes/composes only. Shared policy/resolution lives in the independent Build.CMake repository, fetched as ncbuild. Production libraries are defined in src/, mocks in mock/, tests in test/, applications in app/. Namespace paths and existing target names are preserved.

Enter a VS 2026 x64 developer shell, set standard VCPKG_ROOT, and use CMake 4.2+ and Ninja:

```powershell
cmake --preset windows-msvc-release -DFETCHCONTENT_SOURCE_DIR_NCBUILD=/absolute/path/to/Build.CMake
cmake --build o/windows-msvc-release --parallel 8
ctest --test-dir o/windows-msvc-release --output-on-failure --no-tests=error
```

The bootstrap option is unnecessary once Build.CMake is available at its declared Git URL. Optional ignored Build.Local.cmake registers canonical module URLs with NavalCommandSetDependencyPath(URL, absoluteCheckout); it is read once at the top level before declaration and applies transitively. Without a registration the declared Git URL and main reference are used. There is no workspace-root environment variable or implicit sibling lookup. External dependencies remain source-built vcpkg manifest packages. An already provisioned prefix can use standard -DVCPKG_INSTALLED_DIR=... -DVCPKG_MANIFEST_INSTALL=OFF cache options. See the separate [Docs repository build policy](../Docs/MODULE_STRUCTURE.md) for current commands and limits; dated measurements belong to the [task archive](../Docs/Tasks/README.md).

Presets contain configurePresets only, windows-msvc-debug/release and linux-clang-debug/release where applicable. Output is o/${presetName}. C++26 policy is centralized; MSVC uses /std:c++latest for available features. No full C++26 conformance claim. Build outputs, Build.Local.cmake and CMakeUserPresets.json are ignored. No proprietary game binaries are included.

Windows target and host dependencies use `x64-windows-static`, matching `/MT` in Release and `/MTd` in Debug. Reconfigure with `--fresh` and relink when migrating earlier dynamic-CRT artifacts.
