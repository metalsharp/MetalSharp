# Full-Surface Phase 0 Inventory

**State:** Phase 0 inventory complete; implementation and behavior gates remain open.
**Stable baseline:** Microsoft DirectX Agility SDK 1.619.5 (`D3D12SDKVersion=619`)
**Preview lane:** Agility SDK 1.721.3-preview (`D3D12SDKVersion=721`), opt-in only
**Generated:** 2026-08-29T18:03:51.636603+00:00
**Git HEAD:** `318f03c3b7a74aafc4bed13db91f223813acdf1c`
**Runtime source files scanned:** 166
**Runtime source tree SHA-256:** `154ddc527fd21d6fbfb137f7f523bd66b13de49286d07809b6d5ea26d1d31793`

## Interface census

- Interfaces: **145**
- Methods: **537**
- Methods with no textual source reference: **222**
- Textual source references are inventory clues only; they do not promote behavior.

## Static runtime findings

- Total findings: **1361**
- `capability_literal`: **79**
- `empty_function_body`: **76**
- `placeholder_return`: **590**
- `success_or_zero_return`: **551**
- `unsupported_return`: **65**

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
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:236` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:297` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:325` — `return S_OK;`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:1745` — `CLTRACE("BuildRaytracingAccelerationStructure type=%u dest=0x%llx "`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:1772` — `CLTRACE("EmitRaytracingPostbuildInfo type=%u dest=0x%llx source=0x%llx",`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:1799` — `CLTRACE("CopyRaytracingAccelerationStructure mode=%u source=0x%llx "`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:124` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:126` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:351` — `return 0;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:405` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:417` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:421` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:503` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:512` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:610` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:632` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:635` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:638` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:642` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:646` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:650` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:654` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:656` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:666` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:670` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:674` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:678` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:682` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:686` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:690` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:692` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:699` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:715` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:716` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:726` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:729` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:738` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:760` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:770` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:835` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:847` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:858` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:864` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:873` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:894` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:907` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:913` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:922` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1190` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1196` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1205` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1213` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1286` — `return nullptr;`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1365` — `bool geom_mesh = false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1431` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1438` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1440` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:1444` — `return false;`

## Phase 0 artifacts

- `contracts/d3d12-full-surface-contract.json`
- `contracts/d3d12-full-surface-matrix.json`
- `contracts/d3d12-provider-contract.json`
- `contracts/d3d12-interface-census.json`
- `contracts/d3d12-no-op-policy.json`
- `contracts/d3d12-sdk-compatibility-matrix.json`
- `docs/roadmaps/d3d12-full-surface-phase0-inventory.md`

Phase 0 does not claim any runtime feature. It establishes the complete, versioned inventory and the evidence required before promotion.
