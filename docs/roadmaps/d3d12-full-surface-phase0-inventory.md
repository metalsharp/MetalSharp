# Full-Surface Phase 0 Inventory

**State:** Phase 0 inventory complete; implementation and behavior gates remain open.
**Stable baseline:** Microsoft DirectX Agility SDK 1.619.5 (`D3D12SDKVersion=619`)
**Preview lane:** Agility SDK 1.721.3-preview (`D3D12SDKVersion=721`), opt-in only
**Generated:** 2026-09-06T14:52:33.976444+00:00
**Git HEAD:** `f090edeb06753423a6bb1e4bf35357a9b9d3d638`
**Runtime source files scanned:** 230
**Runtime source tree SHA-256:** `1a4358f214132009b2fd95b475024bc5baacfff80b4a7fd7b48a16e898be2e24`

## Interface census

- Interfaces: **145**
- Methods: **537**
- Methods with no textual source reference: **115**
- Textual source references are inventory clues only; they do not promote behavior.

## Static runtime findings

- Total findings: **2168**
- `abi_literal`: **40**
- `capability_literal`: **174**
- `empty_function_body`: **77**
- `placeholder_return`: **1076**
- `success_or_zero_return`: **748**
- `unsupported_return`: **53**

These findings are intentionally not suppressed. Later phases must attach a behavior probe and clear each legal-operation finding; expected invalid-input returns remain explicitly classified.

## Top findings

- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:240` — `return S_OK;`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12.cpp:314` — `void **dsr_device) = 0;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:514` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:521` — `return nullptr;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:532` — `return nullptr;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:605` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:625` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:691` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:704` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:710` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:718` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:727` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:729` — `UINT STDMETHODCALLTYPE GetDeviceState() override { return 0; }`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:753` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:801` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:841` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:857` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:862` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:869` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:903` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:930` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:947` — `return nullptr;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:964` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1092` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1098` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1106` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1108` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1115` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1119` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1129` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1168` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1229` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1272` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1323` — `return S_OK;`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1484` — `"version=%u subobject=%u type=%u -> E_NOTIMPL",`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1487` — `return E_NOTIMPL;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1693` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1704` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1709` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1784` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1891` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1899` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2132` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2162` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2212` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2234` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2261` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2284` — `return S_OK;`
- `empty_function_body` `vendor/dxmt/src/d3d12/d3d12.cpp:2435` — `_RSBlob(std::vector<uint8_t> &&data) : m_data(std::move(data)) {}`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2441` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2473` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2478` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2506` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2511` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2549` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2557` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2562` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2581` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2596` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2614` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2634` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2639` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2652` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2706` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2739` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2744` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2755` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2771` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2802` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2830` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2836` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2865` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2887` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2893` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2992` — `return S_OK;`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:3044` — `return DXGI_ERROR_UNSUPPORTED;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:3210` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:3378` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:3518` — `return TRUE;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_binding_completeness.hpp:51` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_allocator.cpp:30` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_allocator.cpp:81` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:102` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:121` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:137` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:145` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:420` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:436` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:441` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:446` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:540` — `return S_OK;`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:602` — `// back to an ordinary draw: that would make SetViewInstanceMask(0) render`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2256` — `(kD3D12RaytracingOmmArrayByteAlignment - 1)) != 0 \|\|`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2259` — `(kD3D12RaytracingOmmDescsByteAlignment - 1)) != 0 \|\|`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2261` — `(kD3D12RaytracingOmmDescsByteAlignment - 1)) != 0 \|\|`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2267` — `(kD3D12RaytracingOmmArrayByteAlignment - 1)) != 0 \|\|`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2269` — `(kD3D12RaytracingAccelerationStructureByteAlignment - 1)) != 0 \|\|`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2366` — `(kD3D12RaytracingAccelerationStructureByteAlignment - 1)) != 0 \|\|`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2368` — `(kD3D12RaytracingAccelerationStructureByteAlignment - 1)) != 0) {`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2431` — `CLTRACE("BuildRaytracingAccelerationStructure type=%u dest=0x%llx "`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2458` — `CLTRACE("EmitRaytracingPostbuildInfo type=%u dest=0x%llx source=0x%llx",`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2485` — `CLTRACE("CopyRaytracingAccelerationStructure mode=%u source=0x%llx "`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:80` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:253` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:255` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:487` — `return 0;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:541` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:553` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:557` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:621` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:629` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:657` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:666` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:674` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:684` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:691` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:699` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:706` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:722` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:724` — `return true;`

## Phase 0 artifacts

- `contracts/d3d12-full-surface-contract.json`
- `contracts/d3d12-full-surface-matrix.json`
- `contracts/d3d12-provider-contract.json`
- `contracts/d3d12-interface-census.json`
- `contracts/d3d12-no-op-policy.json`
- `contracts/d3d12-sdk-compatibility-matrix.json`
- `docs/roadmaps/d3d12-full-surface-phase0-inventory.md`

Phase 0 does not claim any runtime feature. It establishes the complete, versioned inventory and the evidence required before promotion.
