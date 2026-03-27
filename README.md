# Organic Assembler

An Operating System for Instruments, written in attolang. Instruments are multimodal dataflow programs — authored as node graphs possibly using the **attoflow** editor, compiled, and run in real time.

Each instrument is a self-contained `.atto` program that defines its Functionality. The System compiles these programs runs them with hot-reload support.

<img width="3840" height="2088" alt="nanolang" src="https://github.com/nilware-io/orgasm/blob/main/docs/nanolang.png" />


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

## Instructions

New to the project? Start with the [Instructions](docs/instructions.md) — a guide to interpreting and following instructions, operating on the codebase, and building instruments with the Organic Assembler. It covers everything from the build system and architectural layers to the anatomy of an instrument and the audio callback pattern.

For naming philosophy, see [Names](docs/names.md). For the full documentation suite: [Architecture](docs/architecture.md), [Language Spec](docs/attolang.md), [Patterns](docs/patterns.md), [Thinking](docs/thinking.md), [Style](docs/style.md), [Coding](docs/coding.md), [Changelog](docs/changelog.md).

## License

MIT — see [LICENSE](LICENSE).
