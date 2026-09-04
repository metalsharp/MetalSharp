# Full-Surface Phase 0 Inventory

**State:** Phase 0 inventory complete; implementation and behavior gates remain open.
**Stable baseline:** Microsoft DirectX Agility SDK 1.619.5 (`D3D12SDKVersion=619`)
**Preview lane:** Agility SDK 1.721.3-preview (`D3D12SDKVersion=721`), opt-in only
**Generated:** 2026-09-04T13:46:46.245463+00:00
**Git HEAD:** `511bc77f7817f308d4d88e0831304850ee0c4a57`
**Runtime source files scanned:** 172
**Runtime source tree SHA-256:** `ea0069aba7d374951c5d56c484b0ce93cd0c382f94e61866f27811c77530e987`

## Interface census

- Interfaces: **145**
- Methods: **537**
- Methods with no textual source reference: **153**
- Textual source references are inventory clues only; they do not promote behavior.

## Static runtime findings

- Total findings: **1800**
- `capability_literal`: **143**
- `empty_function_body`: **77**
- `placeholder_return`: **830**
- `success_or_zero_return`: **698**
- `unsupported_return`: **52**

These findings are intentionally not suppressed. Later phases must attach a behavior probe and clear each legal-operation finding; expected invalid-input returns remain explicitly classified.

## Top findings

- `capability_literal` `vendor/dxmt/src/d3d12/d3d12.cpp:169` — `void **dsr_device) = 0;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:369` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:376` — `return nullptr;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:387` — `return nullptr;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:460` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:480` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:513` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:561` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:601` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:617` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:622` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:629` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:663` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:690` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:707` — `return nullptr;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:724` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:852` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:858` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:866` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:868` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:875` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:879` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:889` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:928` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:989` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1032` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1083` — `return S_OK;`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1244` — `"version=%u subobject=%u type=%u -> E_NOTIMPL",`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1247` — `return E_NOTIMPL;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1453` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1464` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1469` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1544` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1651` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1659` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1892` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1922` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1972` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:1994` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2021` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2044` — `return S_OK;`
- `empty_function_body` `vendor/dxmt/src/d3d12/d3d12.cpp:2195` — `_RSBlob(std::vector<uint8_t> &&data) : m_data(std::move(data)) {}`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2201` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2233` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2238` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2266` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2271` — `return S_OK;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2309` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2317` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2322` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2341` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2356` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2374` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2394` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2399` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2412` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2466` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2499` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2504` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2515` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2531` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2562` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2590` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2596` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2625` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2647` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2653` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2752` — `return S_OK;`
- `unsupported_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2804` — `return DXGI_ERROR_UNSUPPORTED;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:2970` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:3114` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12.cpp:3233` — `return TRUE;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_binding_completeness.hpp:51` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_allocator.cpp:30` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_allocator.cpp:81` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:270` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:275` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:280` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:285` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:346` — `return S_OK;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:378` — `return S_OK;`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:440` — `// back to an ordinary draw: that would make SetViewInstanceMask(0) render`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2141` — `CLTRACE("BuildRaytracingAccelerationStructure type=%u dest=0x%llx "`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2168` — `CLTRACE("EmitRaytracingPostbuildInfo type=%u dest=0x%llx source=0x%llx",`
- `capability_literal` `vendor/dxmt/src/d3d12/d3d12_command_list.cpp:2195` — `CLTRACE("CopyRaytracingAccelerationStructure mode=%u source=0x%llx "`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:147` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:149` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:381` — `return 0;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:435` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:447` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:451` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:515` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:523` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:551` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:560` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:568` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:578` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:585` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:593` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:600` — `return false;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:616` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:618` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:645` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:654` — `return 0;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:752` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:785` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:788` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:791` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:795` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:799` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:803` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:807` — `return true;`
- `placeholder_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:809` — `return false;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:819` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:823` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:827` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:831` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:835` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:839` — `return true;`
- `success_or_zero_return` `vendor/dxmt/src/d3d12/d3d12_command_queue.cpp:843` — `return true;`

## Phase 0 artifacts

- `contracts/d3d12-full-surface-contract.json`
- `contracts/d3d12-full-surface-matrix.json`
- `contracts/d3d12-provider-contract.json`
- `contracts/d3d12-interface-census.json`
- `contracts/d3d12-no-op-policy.json`
- `contracts/d3d12-sdk-compatibility-matrix.json`
- `docs/roadmaps/d3d12-full-surface-phase0-inventory.md`

Phase 0 does not claim any runtime feature. It establishes the complete, versioned inventory and the evidence required before promotion.
