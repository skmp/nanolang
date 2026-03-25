# Organic Assembler

An operating system for instruments, written in attolang. Instruments are visual dataflow programs — authored as node graphs in the **attoflow** editor, compiled to native code, and run in real time.

Each instrument is a self-contained `.atto` program that defines its audio synthesis, event handling, and UI. The system compiles these programs to C++ and runs them with hot-reload support.

See an [example instrument](scenes/klavier/main.atto) and the full [language specification](docs/attolang.md).

## Components

| Target | Description |
|--------|-------------|
| **attolang** | Core language library — type system, expression parser, type inference, serialization |
| **attoflow** | Visual node editor for authoring instruments (SDL3 + Dear ImGui) |
| **attoc** | Standalone compiler (`.atto` → C++) |
| **attoruntime** | Instrument runtime with GUI and audio bindings |

## Language Highlights

- **Rich type system** — scalars (`u8`–`s64`, `f32`/`f64`, `bool`, `string`), containers (`vector`, `map`, `list`, `set`, `queue`), fixed-size arrays, tensors, named struct types, function types
- **First-class literals, symbols, and types** — `literal<T,V>`, `symbol<name,type>`, `type<T>` as compile-time values
- **Bidirectional type inference** with automatic integer upcasting and iterator-to-reference decay
- **Bang-driven control flow** — nodes postfixed with `!` have explicit execution ordering via bang signals
- **Inline expressions** — node arguments can embed literals, variable refs, and sub-expressions directly
- **Lambda construction** — expression nodes can be captured as callable lambdas with automatic capture/parameter resolution
- **FFI support** — declare external C functions and call them from the graph
- **Standard library modules** — e.g. `decl_import "std/imgui"` for ImGui bindings

## Building

Requires CMake 3.25+ and a C++20 compiler. SDL3 and Dear ImGui are fetched automatically on Linux/macOS; on Windows they are managed via [vcpkg](https://vcpkg.io).

### Linux

```bash
# install SDL3 build dependencies
sudo apt-get install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev libegl-dev libgles-dev

cmake -B build && cmake --build build --parallel
```

### Windows

```bash
# install vcpkg if you haven't already
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.bat

cmake -B build -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel --config Release
```

## License

MIT — see [LICENSE](LICENSE).
