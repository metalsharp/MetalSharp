# Subnautica 2 Failure Index

Generated from local logs and shader sidecars without launching the game.

## Totals

- `barrier`: 0
- `dxil_msl_compile_failed`: 10
- `fullscreen_diag`: 0
- `metal_pso_error`: 57
- `present`: 2511
- `readback_nonzero`: 24
- `readback_zero`: 32
- `rtv`: 64
- `ue_no_window`: 0
- `shader_sidecars`: 15119

## Latest Logs

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779836424.log`

- counts: `{}`

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779834908.log`

- counts: `{"present": 16, "readback_nonzero": 1, "readback_zero": 4, "rtv": 1}`
- tail evidence:
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=12 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=1 present=[11@slot6+0:fmt31]`

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779834668.log`

- counts: `{"present": 11, "readback_zero": 3, "rtv": 2}`
- tail evidence:
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  VKD3D final RTV slot=0 handle=0x5178238912 rtv_fmt=24 rtv_dim=4 res=0x836dca10 dim=3 fmt=24 size=898x503x1 mips=1 samples=1 tex=140190944847408 tex_id=3931 tex_array=1 buf=0 gpu=0x35184474259456 bytes=0 swapchain=1 bb=0`

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779834360.log`

- counts: `{"present": 11, "readback_nonzero": 3, "readback_zero": 1, "rtv": 2}`
- tail evidence:
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  VKD3D final RTV slot=0 handle=0x5014656416 rtv_fmt=24 rtv_dim=4 res=0x130aec440 dim=3 fmt=24 size=774x435x1 mips=1 samples=1 tex=140471035111072 tex_id=3457 tex_array=1 buf=0 gpu=0x35184464756736 bytes=0 swapchain=1 bb=0`

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779834094.log`

- counts: `{"present": 8, "readback_nonzero": 1, "readback_zero": 5, "rtv": 1}`
- tail evidence:
  - `info:  VKD3D swapchain readback count=30 fmt=24 sample=724x418 nonzero_pixels=0 nonzero_bytes=0 max_byte=0 checksum=0x17675501c860ab03`
  - `info:  VKD3D swapchain render readback capture=16 backbuffer=0 fmt=24 sample=724x418 nonzero_pixels=0 nonzero_bytes=0 max_byte=0 checksum=0x17675501c860ab03`
  - `info:  VKD3D final RTV slot=0 handle=0x5111882064 rtv_fmt=24 rtv_dim=4 res=0x13093e4e0 dim=3 fmt=24 size=724x418x1 mips=1 samples=1 tex=140493007026416 tex_id=2661 tex_array=1 buf=0 gpu=0x35184447193088 bytes=0 swapchain=1 bb=1`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt39,12@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=14 layouts=9 slot_mask=0x1c0 has_input_signature=1 reflection_order=3 present=[11@slot6+0:fmt31,12@slot8+0:fmt36,13@slot7+4:fmt31]`
  - `info:  D3D12 stage_in vertex descriptor attrs=17 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=6 present=[11@slot6+0:fmt31,12@slot6+16:fmt29,13@slot6+24:fmt29,14@slot6+32:fmt31,15@slot6+36:fmt31,16@slot6+40:fmt37]`
  - `info:  VKD3D swapchain render readback capture=30 backbuffer=0 fmt=24 sample=774x435 nonzero_pixels=83737 nonzero_bytes=334948 max_byte=229 checksum=0x492ca6bb19ff4ab4`

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779833779.log`

- counts: `{"present": 7, "readback_nonzero": 5, "readback_zero": 2, "rtv": 3}`
- tail evidence:
  - `info:  VKD3D final RTV slot=0 handle=0x644237264 rtv_fmt=24 rtv_dim=4 res=0x2674d0f0 dim=3 fmt=24 size=1280x720x1 mips=1 samples=1 tex=140682569925600 tex_id=125 tex_array=1 buf=0 gpu=0x35184428580864 bytes=0 swapchain=1 bb=0`
  - `info:  VKD3D swapchain render readback capture=16 backbuffer=0 fmt=24 sample=1280x720 nonzero_pixels=24157 nonzero_bytes=93920 max_byte=255 checksum=0xc52bfdadf38d8a12`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  VKD3D final RTV slot=0 handle=0x644237264 rtv_fmt=24 rtv_dim=4 res=0x2674d0f0 dim=3 fmt=24 size=1280x720x1 mips=1 samples=1 tex=140682569925600 tex_id=125 tex_array=1 buf=0 gpu=0x35184428580864 bytes=0 swapchain=1 bb=0`
  - `info:  VKD3D swapchain render readback capture=30 backbuffer=1 fmt=24 sample=1280x720 nonzero_pixels=18 nonzero_bytes=72 max_byte=253 checksum=0x7f774cd99bea747`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=17 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=6 present=[11@slot6+0:fmt31,12@slot6+16:fmt29,13@slot6+24:fmt29,14@slot6+32:fmt31,15@slot6+36:fmt31,16@slot6+40:fmt37]`

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779833306.log`

- counts: `{"present": 9, "readback_nonzero": 3, "readback_zero": 3, "rtv": 1}`
- tail evidence:
  - `info:  VKD3D present blit count=30 idx=0 src=140412589774112 drawable=140412844978416 size=1280x720`
  - `info:  VKD3D swapchain readback count=30 fmt=24 sample=1280x720 nonzero_pixels=18 nonzero_bytes=70 max_byte=249 checksum=0x5486e4dc2a3f1957`
  - `info:  VKD3D final RTV slot=0 handle=0x644761296 rtv_fmt=24 rtv_dim=4 res=0x1287fdb70 dim=3 fmt=24 size=1280x720x1 mips=1 samples=1 tex=140412589774112 tex_id=125 tex_array=1 buf=0 gpu=0x35184428580864 bytes=0 swapchain=1 bb=0`
  - `info:  VKD3D swapchain render readback capture=16 backbuffer=0 fmt=24 sample=1280x720 nonzero_pixels=18 nonzero_bytes=70 max_byte=249 checksum=0x5486e4dc2a3f1957`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=13 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt29,12@slot6+16:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`
  - `info:  D3D12 stage_in vertex descriptor attrs=1 layouts=7 slot_mask=0x40 has_input_signature=1 reflection_order=0 present=[0@slot6+0:fmt29]`

### `/Users/alexmondello/.metalsharp/compatdata/1962700/logs/launch-1779832975.log`

- counts: `{"present": 157}`
- tail evidence:
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`
  - `info:  D3D12 stage_in vertex descriptor attrs=25 layouts=8 slot_mask=0xc0 has_input_signature=1 reflection_order=2 present=[11@slot6+0:fmt31,24@slot7+0:fmt36]`

## Latest Shader Sidecars

- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-b37687dfe5427f9c-286c403cfd59850f.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/286c403cfd59850f.dxil_report.txt`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/1cdbcdf625b4e874.dxil_report.txt`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-92814e5940d2986e-68f8da1b8922f375.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/68f8da1b8922f375.dxil_report.txt`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-1624db321d2130ad-2b5d194c3804708d.json`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-e8cb8780900c50c5-0b600dff5efc7726.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/0b600dff5efc7726.dxil_report.txt`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/2b5d194c3804708d.dxil_report.txt`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-1624db321d2130ad-f0779d696d0aad0b.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/f0779d696d0aad0b.dxil_report.txt`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-b37687dfe5427f9c-b17d9b7d97ec1568.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/b17d9b7d97ec1568.dxil_report.txt`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-e2c199cb67d1f610-0b600dff5efc7726.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/1624db321d2130ad.dxil_report.txt`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-b37687dfe5427f9c-48e304f00a75150e.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/b37687dfe5427f9c.dxil_report.txt`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/48e304f00a75150e.dxil_report.txt`
- `pso-*.json` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/pso-render-b37687dfe5427f9c-37c154adb4864f99.json`
- `*.dxil_report.txt` `/Users/alexmondello/.metalsharp/shader-cache/vkd3d/1962700/shaders/37c154adb4864f99.dxil_report.txt`
