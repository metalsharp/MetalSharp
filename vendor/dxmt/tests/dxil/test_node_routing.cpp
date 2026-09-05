#include "d3d12_node_routing.hpp"
#include <iostream>
#include <stdexcept>
using namespace dxmt;
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
  try {
    const std::vector<NodeRoutingTarget> nodes = {{"entry",0},{"sparse_entry",0},
        {"array",0},{"array",1},{"array",2},{"array",3},{"sparse",65536}};
    const std::vector<NodeRoutingOutput> outputs = {
        {0,0,"array",0,4,true,false}, {1,0,"sparse",0,UINT32_MAX,true,true}};
    const auto routes = buildNodeOutputRoutes(nodes, outputs);
    require(routes && routes->size() == 8, "routing table expanded by declared sparse size");
    require((*routes)[0].target_node == 2 && (*routes)[3].target_node == 5,
            "dense array destinations lost");
    require((*routes)[4].array_index == UINT32_MAX && (*routes)[4].target_node == UINT32_MAX,
            "dense invalid route missing");
    require((*routes)[5].source_node == 1 && (*routes)[5].array_index == 0 &&
            (*routes)[5].target_node == UINT32_MAX, "sparse base descriptor lost");
    require((*routes)[6].array_index == 65536 && (*routes)[6].target_node == 6,
            "full-width sparse destination lost");
    require((*routes)[7].array_index == UINT32_MAX && (*routes)[7].target_node == UINT32_MAX,
            "sparse invalid route missing");
    auto duplicate = nodes; duplicate.push_back(nodes[2]);
    require(!buildNodeOutputRoutes(duplicate, outputs), "duplicate node identity accepted");
    auto missing = nodes; missing.erase(missing.begin() + 3);
    require(!buildNodeOutputRoutes(missing, outputs), "missing dense member accepted");
    auto invalid = outputs; invalid[1].allow_sparse = false;
    require(!buildNodeOutputRoutes(nodes, invalid), "unbounded nonsparse output accepted");
    invalid = outputs; invalid[0].array_size = 0;
    require(!buildNodeOutputRoutes(nodes, invalid), "zero array size accepted");
    invalid = outputs; invalid.push_back(outputs[0]);
    require(!buildNodeOutputRoutes(nodes, invalid), "duplicate output descriptor accepted");
    require(!buildNodeOutputRoutes(nodes, {{0,0,"absent",0,1,false,false}}),
            "missing required scalar target accepted");
    require(bool(buildNodeOutputRoutes(nodes, {{0,0,"absent",0,1,false,true}})),
            "optional scalar target rejected");
    const std::vector<NodeRoutingOutput> far_output = {{0,0,"far",0,UINT32_MAX,true,true}};
    require(bool(buildNodeOutputRoutes({{"entry",0},{"far",0xfffffdu}}, far_output)),
            "exact nonrecursive node budget rejected");
    require(!buildNodeOutputRoutes({{"entry",0},{"far",0xfffffeu}}, far_output),
            "aggregate sparse node budget overflow accepted");
    require(bool(buildNodeOutputRoutes({{"entry",0},{"far",0xfffffdu},{"far",0}}, far_output)),
            "same-name array span charged twice");
    require(bool(buildNodeOutputRoutes({{"entry",0},{"far",0},{"far",0xfffffdu}}, far_output)),
            "node budget depends on declaration order");
    require(bool(buildNodeOutputRoutes({{"entry",0,1},{"far",0xfffffcu,0}}, far_output)),
            "exact recursion-charged budget rejected");
    require(!buildNodeOutputRoutes({{"entry",0,2},{"far",0xfffffcu,0}}, far_output),
            "recursion slots omitted from budget");
    require(!buildNodeOutputRoutes({{"entry",0,0},{"far",0xfffffbu,1},{"far",0,2}}, far_output),
            "same-name recursion declarations not charged individually");
    const std::vector<NodeRoutingOutput> self_output = {{0,0,"self",0,1,false,false}};
    require(!buildNodeOutputRoutes({{"self",0,0}}, self_output),
            "self-edge without recursion declaration accepted");
    const auto self_routes = buildNodeOutputRoutes({{"self",0,3}}, self_output);
    require(self_routes && (*self_routes)[0].target_node == 0,
            "declared self-edge missing");
    const auto declared_only = buildNodeOutputRoutes({{"entry",0,3}}, {});
    require(declared_only && declared_only->empty(), "declaration invented a self-edge");
    std::cout << "PASS: dense/sparse routing construction and rejection rules\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
