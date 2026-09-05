d3d12_vertex_contract_test = executable('test_d3d12_vertex_contract',
  [
    'dxil/test_d3d12_vertex_contract.cpp',
  ],
  include_directories : [ dxmt_include_path, dxmt_native_include_path ],
  native : true,
)

test('d3d12_vertex_contract', d3d12_vertex_contract_test)

d3d12_node_gpu_input_test = executable('test_node_gpu_input',
  [
    'dxil/test_node_gpu_input.cpp',
  ],
  include_directories : [ dxmt_include_path, dxmt_native_include_path ],
  native : true,
)

test('node_gpu_input', d3d12_node_gpu_input_test)

subdir('d3d12_game')
if fs.is_file('tests/dx11/cube.obj') and fs.is_file('tests/dx11/test.png')
  subdir('dx11')
else
  message('Skipping dx11 samples: cube.obj/test.png assets are not present')
endif
