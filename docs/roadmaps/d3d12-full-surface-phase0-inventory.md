# Full-Surface Phase 0 Inventory

**State:** Phase 0 inventory complete; implementation and behavior gates remain open.
**Stable baseline:** Microsoft DirectX Agility SDK 1.619.5 (`D3D12SDKVersion=619`)
**Preview lane:** Agility SDK 1.721.3-preview (`D3D12SDKVersion=721`), opt-in only
**Generated:** 2026-08-30T05:24:45.205935+00:00
**Git HEAD:** `c3a988c554ef2fa246d32f846e0a96eb5c4156da`
**Runtime source files scanned:** 166
**Runtime source tree SHA-256:** `07b84995113735edf7673f579cc7ced1595b50d7c60e8d50bda663b108942dd6`

## Interface census

- Interfaces: **145**
- Methods: **537**
- Methods with no textual source reference: **222**
- Textual source references are inventory clues only; they do not promote behavior.

## Static runtime findings

- Total findings: **1437**
- `capability_literal`: **92**
- `empty_function_body`: **76**
- `placeholder_return`: **634**
- `success_or_zero_return`: **569**
- `unsupported_return`: **66**

These findings are intentionally not suppressed. Later phases must attach a behavior probe and clear each legal-operation finding; expected invalid-input returns remain explicitly classified.

## Top findings

- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:312` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:319` — `return nullptr;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:330` — `return nullptr;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:403` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:423` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:447` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:463` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:468` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:475` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:509` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:536` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:553` — `return nullptr;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:570` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:651` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:676` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:686` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:704` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:732` — `return S_OK;`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:741` — `"version=%u subobjects=%u -> E_NOTIMPL",`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:743` — `return E_NOTIMPL;`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:750` — `"E_NOTIMPL",`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:754` — `return E_NOTIMPL;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:765` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:797` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:842` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:864` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:891` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:914` — `return S_OK;`
- `empty_function_body` `vendor/dxmt/src/d3d12/d3d12.cpp:1065` — `_RSBlob(std::vector<uint8_t> &&data) : m_data(std::move(data)) {}`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1071` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1103` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1108` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1136` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1141` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1179` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1187` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1192` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1211` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1226` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1244` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1264` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1269` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1282` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1336` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1369` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1374` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1385` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1401` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1432` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1460` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1466` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1495` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1517` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1523` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1622` — `return S_OK;`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1674` — `return DXGI_ERROR_UNSUPPORTED;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1840` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1984` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2087` — `return TRUE;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_binding_completeness.hpp:51` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_allocator.cpp:30` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_allocator.cpp:81` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:268` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:329` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:361` — `return S_OK;`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2029` — `CLTRACE("BuildRaytracingAccelerationStructure type=%u dest=0x%llx "`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2056` — `CLTRACE("EmitRaytracingPostbuildInfo type=%u dest=0x%llx source=0x%llx",`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2083` — `CLTRACE("CopyRaytracingAccelerationStructure mode=%u source=0x%llx "`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:137` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:139` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:364` — `return 0;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:418` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:430` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:434` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:516` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:525` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:623` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:656` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:659` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:662` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:666` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:670` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:674` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:678` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:680` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:690` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:694` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:698` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:702` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:706` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:710` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:714` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:716` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:723` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:739` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:740` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:750` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:753` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:762` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:784` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:794` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:859` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:871` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:882` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:888` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:897` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:918` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:931` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:937` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:946` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1229` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1235` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1244` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1252` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1325` — `return nullptr;`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1404` — `bool geom_mesh = false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1470` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1477` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1479` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1483` — `return false;`

## Phase 0 artifacts

- `contracts/d3d12-full-surface-contract.json`
- `contracts/d3d12-full-surface-matrix.json`
- `contracts/d3d12-provider-contract.json`
- `contracts/d3d12-interface-census.json`
- `contracts/d3d12-no-op-policy.json`
- `contracts/d3d12-sdk-compatibility-matrix.json`
- `docs/roadmaps/d3d12-full-surface-phase0-inventory.md`

Phase 0 does not claim any runtime feature. It establishes the complete, versioned inventory and the evidence required before promotion.
