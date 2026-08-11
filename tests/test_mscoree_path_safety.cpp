#include "mscoree_path.h"

#include <cassert>
#include <cstring>
#include <string>

int main() {
    constexpr size_t path_size = 1024;
    const std::string max_length_path(path_size - 1, 'a');

    char exe_path[path_size];
    std::memset(exe_path, 'x', sizeof(exe_path));
    mscoree_terminate_path(exe_path, sizeof(exe_path));
    assert(exe_path[path_size - 1] == '\0');

    char exe_dir[path_size];
    mscoree_copy_path(exe_dir, sizeof(exe_dir), max_length_path.c_str());
    assert(exe_dir[path_size - 1] == '\0');
    assert(std::strlen(exe_dir) == path_size - 1);
    assert(std::strrchr(exe_dir, '\\') == nullptr);

    char config_file[path_size];
    assert(!mscoree_build_config_path(config_file, sizeof(config_file), max_length_path.c_str()));
    assert(config_file[path_size - 1] == '\0');

    constexpr const char* normal_path = "C:\\games\\managed.exe";
    assert(mscoree_build_config_path(config_file, sizeof(config_file), normal_path));
    assert(std::strcmp(config_file, "C:\\games\\managed.exe.config") == 0);

    return 0;
}
