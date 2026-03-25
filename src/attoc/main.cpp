#include "serial.h"
#include "inference.h"
#include "type_utils.h"
#include "codegen.h"
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static void write_file(const std::string& path, const std::string& content) {
    // Read existing content and skip write if unchanged (avoids cmake re-configure)
    {
        std::ifstream existing(path);
        if (existing) {
            std::string old_content((std::istreambuf_iterator<char>(existing)),
                                     std::istreambuf_iterator<char>());
            if (old_content == content) {
                printf("  unchanged %s (%d bytes)\n", path.c_str(), (int)content.size());
                return;
            }
        }
    }
    std::ofstream f(path);
    if (!f) {
        fprintf(stderr, "Error: cannot write %s\n", path.c_str());
        return;
    }
    f << content;
    printf("  wrote %s (%d bytes)\n", path.c_str(), (int)content.size());
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: nanoc <project_dir> [-o <output_dir>]\n");
        return 1;
    }

    std::string input_arg = argv[1];
    std::string output_dir = "output";

    // Parse -o flag
    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc) {
            output_dir = argv[++i];
        }
    }

    // Input must be a directory containing main.atto
    fs::path input_p(input_arg);
    if (!fs::is_directory(input_p)) {
        fprintf(stderr, "Error: %s is not a directory\n", input_arg.c_str());
        return 1;
    }
    std::string input_path = (input_p / "main.atto").string();
    std::string source_name = input_p.filename().string();
    if (!fs::exists(input_path)) {
        fprintf(stderr, "Error: no main.atto found in %s\n", input_arg.c_str());
        return 1;
    }

    // Load the graph
    FlowGraph graph;
    load_atto(input_path, graph);

    // Resolve type-based pins (new, event! nodes)
    resolve_type_based_pins(graph);

    // Run type inference
    TypePool pool;
    GraphInference inference(pool);
    auto errors = inference.run(graph);

    // Collect all errors: inference errors + per-node errors
    for (auto& node : graph.nodes) {
        if (!node.error.empty())
            errors.push_back(std::string(node_type_str(node.type_id)) + " [" + node.guid.substr(0, 8) + "]: " + node.error);
    }

    if (!errors.empty()) {
        fprintf(stderr, "Errors (%d):\n", (int)errors.size());
        for (auto& e : errors)
            fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }

    printf("Loaded %d nodes, generating C++ project...\n", (int)graph.nodes.size());

    // Generate code
    CodeGenerator codegen(graph, pool, source_name);

    // Compute absolute paths for CMake references
    fs::path runtime_path = fs::absolute(fs::path(__FILE__).parent_path() / ".." / "attoruntime");
    std::string runtime_str = runtime_path.string();
    std::replace(runtime_str.begin(), runtime_str.end(), '\\', '/');

    fs::path nanodeps_path = fs::absolute(fs::path(__FILE__).parent_path() / ".." / ".." / "cmake" / "NanoDeps.cmake");
    std::string nanodeps_str = nanodeps_path.string();
    std::replace(nanodeps_str.begin(), nanodeps_str.end(), '\\', '/');

    fs::path nanoc_abs = fs::absolute(fs::path(argv[0]));
    std::string attoc_str = nanoc_abs.string();
    std::replace(attoc_str.begin(), attoc_str.end(), '\\', '/');

    // Pass the project directory (not the .atto file) — nanoc expects a folder
    fs::path atto_project_abs = fs::absolute(fs::path(input_arg));
    std::string atto_project_str = atto_project_abs.string();
    std::replace(atto_project_str.begin(), atto_project_str.end(), '\\', '/');

    // Also pass main.atto path for CMake DEPENDS
    fs::path atto_source_abs = fs::absolute(fs::path(input_path));
    std::string atto_source_str = atto_source_abs.string();
    std::replace(atto_source_str.begin(), atto_source_str.end(), '\\', '/');

    // Create output directory
    fs::create_directories(output_dir);

    // Write files
    try {
        write_file(output_dir + "/" + source_name + "_types.h", codegen.generate_types());
        write_file(output_dir + "/" + source_name + "_program.h", codegen.generate_header());
        write_file(output_dir + "/" + source_name + "_program.cpp", codegen.generate_impl());
        write_file(output_dir + "/CMakeLists.txt", codegen.generate_cmake(runtime_str, attoc_str, atto_project_str, atto_source_str, nanodeps_str));
        write_file(output_dir + "/vcpkg.json", codegen.generate_vcpkg());
    } catch (const std::runtime_error& e) {
        fprintf(stderr, "Codegen error: %s\n", e.what());
        return 2;
    }

    printf("Done! Build with:\n");
    printf("  cd %s && cmake -B build && cmake --build build\n", output_dir.c_str());
    return 0;
}
