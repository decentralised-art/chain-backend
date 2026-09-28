#include "file.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <functional>
#include <thread>

#if defined(_WIN32)
    #include <io.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace dcn::file
{
    std::optional<std::string> loadTextFile(std::filesystem::path path)
    {
        if(std::filesystem::exists(path) == false)
        {
            spdlog::error(std::format("Cannot find {} in the resources directory.", path.string()));
            return std::nullopt;
        }
        
        std::ifstream file(path, std::ios::in);

        if(file.good() == false)
        {  
            spdlog::error(std::format("Failed to open file {}", path.string()));
            return std::nullopt;
        }

        const std::string file_content = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        file.close();

        return file_content;
    }

    std::optional<std::vector<std::byte>> loadBinaryFile(std::filesystem::path path)
    {    
        if (!std::filesystem::exists(path)) {
            spdlog::error("Cannot find {} file.", path.string());
            return std::nullopt;
        }
    
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            spdlog::error("Failed to open file {}", path.string());
            return std::nullopt;
        }
    
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            spdlog::error("File {} is empty or unreadable.", path.string());
            return std::nullopt;
        }
    
        std::vector<std::byte> buffer(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            spdlog::error("Failed to read file {}", path.string());
            return std::nullopt;
        }
    
        return buffer;
    }

    namespace
    {
        std::filesystem::path _temporarySibling(const std::filesystem::path & path)
        {
            static std::atomic<std::uint64_t> counter{0};
            const std::uint64_t unique = std::hash<std::thread::id>{}(std::this_thread::get_id())
                ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
                ^ (counter.fetch_add(1) << 1);
            return path.parent_path() / std::format("{}.{:016x}.tmp", path.filename().string(), unique);
        }

        std::FILE * _openForWrite(const std::filesystem::path & path)
        {
#if defined(_WIN32)
            std::FILE * file = nullptr;
            return ::_wfopen_s(&file, path.c_str(), L"wb") == 0 ? file : nullptr;
#else
            return std::fopen(path.c_str(), "wb");
#endif
        }

        bool _flushToDisk(std::FILE * file)
        {
            if(std::fflush(file) != 0) return false;
#if defined(_WIN32)
            return ::_commit(::_fileno(file)) == 0;
#else
            return ::fsync(::fileno(file)) == 0;
#endif
        }

        void _flushDirectory([[maybe_unused]] const std::filesystem::path & directory)
        {
#if !defined(_WIN32)
            const int fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY);
            if(fd >= 0)
            {
                (void)::fsync(fd);
                (void)::close(fd);
            }
#endif
        }
    }

    std::expected<void, std::string> writeFileDurably(const std::filesystem::path & path, const std::string_view bytes)
    {
        std::error_code ec;
        if(!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), ec);
            if(ec) return std::unexpected(std::format("cannot create directory: {}", ec.message()));
        }

        const std::filesystem::path temporary = _temporarySibling(path);
        std::FILE * file = _openForWrite(temporary);
        if(file == nullptr)
        {
            return std::unexpected("cannot create temporary file");
        }

        const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && _flushToDisk(file);
        const bool closed = std::fclose(file) == 0;
        if(!written || !closed)
        {
            std::filesystem::remove(temporary, ec);
            return std::unexpected("cannot write temporary file");
        }

        std::filesystem::rename(temporary, path, ec);
        if(ec)
        {
            std::error_code remove_ec;
            std::filesystem::remove(temporary, remove_ec);
            return std::unexpected(std::format("cannot replace file: {}", ec.message()));
        }

        _flushDirectory(path.parent_path());
        return {};
    }
}
