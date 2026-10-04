// Test-only stand-in for solc. Unit tests compile the same PT contracts and the same generated
// entities over and over; this forwards every invocation to the real solc and, when
// DECENTRALISED_ART_TEST_SOLC_CACHE_DIR is set, keeps the output of successful compilations so
// a repeated compilation is answered from the cache.
//
// Entries live in <cache>/<key>/<sources>. The key covers the real solc binary, the compiler
// options and the compiled source's unit name and text; <sources> covers every source unit
// solc's metadata lists with the keccak256 solc recorded for it. An entry is reused only while
// each of those units still resolves, unambiguously, to a file with that hash, so a hit leaves
// exactly the files solc would write. Entries are never changed or removed once published: an
// entry that validates holds the output for exactly the sources validated, whatever other
// processes publish meanwhile.
//
// solc runs without a deadline of its own, so it stays in this process's tree and whoever set
// a deadline on this process kills it along with it.
//
// ponytail: no eviction; entries for one-off sources accumulate until the directory is deleted.
// ponytail: concurrent cold misses of one key each run solc; a per-key lock would dedupe them.

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
#endif

#include <nlohmann/json.hpp>

#include "decentralised_art.hpp"

#ifndef DECENTRALISED_ART_REAL_SOLC_PATH
    #error "DECENTRALISED_ART_REAL_SOLC_PATH is not defined"
#endif

namespace
{
    namespace fs = std::filesystem;

    // A compilation of one source into an output directory, the way EVM invokes solc.
    struct Compilation
    {
        // Every argument but the output directory, the source roots and the source.
        std::vector<std::string> options;
        fs::path out_dir;
        // The base path, then the include paths: where solc looks up source units.
        std::vector<fs::path> roots;
        fs::path source;
    };

    std::optional<Compilation> parseCompilation(const std::vector<std::string> & args)
    {
        static const std::set<std::string> options_with_value{"--evm-version", "--optimize-runs"};

        Compilation compilation;
        fs::path base_path;
        std::vector<fs::path> include_paths;
        std::optional<fs::path> source;
        for(std::size_t i = 0; i < args.size(); ++i)
        {
            const std::string & arg = args[i];
            const bool has_value = i + 1 < args.size();
            if(arg == "-o" && has_value) compilation.out_dir = args[++i];
            else if(arg == "--base-path" && has_value) base_path = args[++i];
            else if(arg == "--include-path" && has_value) include_paths.emplace_back(args[++i]);
            else if(options_with_value.contains(arg) && has_value)
            {
                compilation.options.push_back(arg);
                compilation.options.push_back(args[++i]);
            }
            else if(arg.starts_with("-")) compilation.options.push_back(arg);
            else if(source) return std::nullopt;
            else source = arg;
        }

        if(compilation.out_dir.empty() || !source) return std::nullopt;
        compilation.source = *source;
        compilation.roots.push_back(base_path);
        compilation.roots.insert(compilation.roots.end(), include_paths.begin(), include_paths.end());
        return compilation;
    }

    std::optional<std::string> readFile(const fs::path & path)
    {
        std::ifstream input(path, std::ios::binary);
        if(!input.is_open()) return std::nullopt;
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    // 128 bits of keccak256: a directory name short enough that <cache>/<key>/<entry>/<file>
    // stays within Windows' MAX_PATH.
    std::string nameHash(const std::string & text)
    {
        return dcn::chain::bytes32ToHex(dcn::chain::keccakString(text)).substr(2, 32);
    }

    std::string keccakHex(const std::string & text)
    {
        return dcn::chain::bytes32ToHex(dcn::chain::keccakString(text));
    }

    // The source's name relative to the root holding it: what reaches solc's output, unlike
    // the per-test directory the source happens to sit in.
    std::string unitName(const Compilation & compilation)
    {
        for(const auto & root : compilation.roots)
        {
            if(root.empty()) continue;
            const fs::path relative = compilation.source.lexically_relative(root);
            if(!relative.empty() && *relative.begin() != "..") return relative.generic_string();
        }
        return compilation.source.generic_string();
    }

    std::optional<std::string> cacheKey(const fs::path & solc, const Compilation & compilation)
    {
        std::error_code ec;
        const auto solc_size = fs::file_size(solc, ec);
        if(ec) return std::nullopt;
        const auto solc_time = fs::last_write_time(solc, ec);
        if(ec) return std::nullopt;
        const auto source_text = readFile(compilation.source);
        if(!source_text) return std::nullopt;

        std::string key = std::format("{}\n{}\n{}\n", solc.string(), solc_size, solc_time.time_since_epoch().count());
        for(const auto & option : compilation.options) key += option + '\n';
        key += unitName(compilation) + '\n' + *source_text;
        return nameHash(key);
    }

    // Where solc finds a source unit: as given when absolute, otherwise under the one root that
    // holds it. solc rejects a unit more than one root holds, so that resolves to nothing.
    std::optional<fs::path> resolveUnit(const std::string & unit, const std::vector<fs::path> & roots)
    {
        std::error_code ec;
        const fs::path unit_path(unit);
        if(unit_path.is_absolute()) return fs::exists(unit_path, ec) ? std::optional(unit_path) : std::nullopt;
        std::optional<fs::path> found;
        for(const auto & root : roots)
        {
            if(root.empty() || !fs::exists(root / unit_path, ec)) continue;
            if(found) return std::nullopt;
            found = root / unit_path;
        }
        return found;
    }

    // Every source unit the metadata in `entry` lists, with the keccak256 solc recorded for it.
    std::optional<std::map<std::string, std::string>> recordedSources(const fs::path & entry)
    {
        std::map<std::string, std::string> recorded;
        std::error_code ec;
        for(const auto & file : fs::directory_iterator(entry, ec))
        {
            if(!file.path().filename().string().ends_with("_meta.json")) continue;

            const auto text = readFile(file.path());
            if(!text) return std::nullopt;
            const nlohmann::json metadata = nlohmann::json::parse(*text, nullptr, false);
            const auto sources = metadata.is_object() ? metadata.find("sources") : metadata.end();
            if(metadata.is_discarded() || sources == metadata.end() || !sources->is_object()) return std::nullopt;

            for(const auto & [unit, description] : sources->items())
            {
                const auto hash = description.is_object() ? description.find("keccak256") : description.end();
                if(hash == description.end() || !hash->is_string()) return std::nullopt;
                recorded.emplace(unit, dcn::chain::normalizeHex(hash->get<std::string>()));
            }
        }
        if(ec || recorded.empty()) return std::nullopt;
        return recorded;
    }

    // The name of the entry holding the output compiled from exactly `sources`.
    std::string entryName(const std::map<std::string, std::string> & sources)
    {
        std::string text;
        for(const auto & [unit, hash] : sources) text += unit + '\n' + hash + '\n';
        return nameHash(text);
    }

    bool entryValid(const fs::path & entry, const std::vector<fs::path> & roots)
    {
        const auto recorded = recordedSources(entry);
        if(!recorded) return false;

        for(const auto & [unit, hash] : *recorded)
        {
            const auto path = resolveUnit(unit, roots);
            if(!path) return false;
            const auto content = readFile(*path);
            if(!content || keccakHex(*content) != hash) return false;
        }
        return true;
    }

    bool copyFiles(const fs::path & from, const fs::path & to)
    {
        std::error_code ec;
        fs::create_directories(to, ec);
        if(ec) return false;
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    std::string randomHex()
    {
        std::random_device rng;
        return std::format("{:08x}{:08x}", rng(), rng());
    }

    // The exit code a child process reports when the solc executable could not be started.
    constexpr int EXEC_FAILED_EXIT_CODE = 127;

    // The solc this forwards to: the configured one, unless tests point it elsewhere.
    fs::path realSolc()
    {
        const char * solc = std::getenv("DECENTRALISED_ART_TEST_SOLC_CACHE_REAL_SOLC");
        return solc && *solc ? fs::path(solc) : fs::path(DECENTRALISED_ART_REAL_SOLC_PATH);
    }

    int runSolc(std::vector<std::string> args)
    {
        const fs::path solc = realSolc();
        try
        {
            const auto [exit_code, output] = dcn::native::runProcess(solc.string(), std::move(args));
            std::cout << output;
            return exit_code;
        }
        catch(const std::exception & error)
        {
            std::cout << std::format("cannot run '{}': {}\n", solc.string(), error.what());
            return EXEC_FAILED_EXIT_CODE;
        }
    }

    std::vector<std::string> withOutputDirectory(std::vector<std::string> args, const fs::path & out_dir)
    {
        for(std::size_t i = 0; i + 1 < args.size(); ++i)
        {
            if(args[i] == "-o") args[i + 1] = out_dir.string();
        }
        return args;
    }
}

int main(int argc, char * argv[])
{
    // Whoever starts this reads its output as solc's (stdout and stderr alike), so nothing
    // the linked server code logs may reach it.
    spdlog::set_level(spdlog::level::off);
#if defined(_WIN32)
    // solc's output is passed on byte for byte, without newline translation.
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    const std::vector<std::string> args(argv + 1, argv + argc);
    const char * cache_dir = std::getenv("DECENTRALISED_ART_TEST_SOLC_CACHE_DIR");
    const auto compilation = parseCompilation(args);
    if(!cache_dir || !*cache_dir || !compilation) return runSolc(args);

    const fs::path cache_root(cache_dir);
    const auto key = cacheKey(realSolc(), *compilation);
    if(!key) return runSolc(args);

    const fs::path entries = cache_root / *key;
    std::error_code ec;
    for(const auto & entry : fs::directory_iterator(entries, ec))
    {
        if(entryValid(entry.path(), compilation->roots) && copyFiles(entry.path(), compilation->out_dir)) return 0;
    }

    // solc writes to a staging directory, which is then renamed into place, so a concurrent
    // reader never sees a half-written entry.
    const fs::path staging = cache_root / std::format("staging-{}", randomHex());
    fs::create_directories(staging, ec);
    if(ec) return runSolc(args);

    const int exit_code = runSolc(withOutputDirectory(args, staging));
    if(exit_code == 0)
    {
        if(!copyFiles(staging, compilation->out_dir))
        {
            std::cout << std::format("cannot copy solc output to '{}'\n", compilation->out_dir.string());
            fs::remove_all(staging, ec);
            return 1;
        }

        // An entry already published under this name holds the same output: that one stays.
        if(const auto sources = recordedSources(staging))
        {
            fs::create_directories(entries, ec);
            fs::rename(staging, entries / entryName(*sources), ec);
        }
    }
    fs::remove_all(staging, ec);
    return exit_code;
}
