# Gateway.Grpc 0.1.0
Generic gRPC library. No Sea Power, C#, host ABI, P/Invoke or loader references.

Construct Server with an injected Gateway.Api IProvider and logger on an I/O worker; provider must outlive server. Start accepts only loopback; Stop shuts down/waits on the host worker. Source is identical when linked into mock-host.exe or SeaPower.Mod's private native DLL.

Set VCPKG_ROOT, NC_VCPKG_INSTALLED_DIR, NC_CMAKE; ./scripts/Build-Native.ps1 builds/tests independently. CPM fetches exact API Git commits in cmake/Modules.cmake. Override repository URL base with NC_REPOSITORY_BASE; do not edit fetched source copies. vcpkg manifest pins external source builds. Our targets request C++26 (clang-cl 19.1.5 /clang:-std=c++2c with MSVC library), with an essential std::expected check; full C++26 library coverage is not claimed.

out/native/mock-host.exe 127.0.0.1:50052 hosts the same service using a domain mock; simulation displacement is clearly tagged mock_displacement. It exits after 120 s. Connect separately with Gateway.Grpc.Api's naval-cli.exe --endpoint 127.0.0.1:50052 --smoke. Source-backed gtest covers domain/wire mapping, duplicate/reconnect/session semantics, bounded retention, protected state and real gRPC connectivity.


