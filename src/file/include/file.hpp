#pragma once

#include <expected>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

namespace dcn::file
{
    std::optional<std::string> loadTextFile(std::filesystem::path path);

    std::optional<std::vector<std::byte>> loadBinaryFile(std::filesystem::path path);

    /// Replaces `path` with `bytes` so that a reader sees either the old or the new
    /// content, never a partial file, and the new content survives a crash once this
    /// returns: unique temporary file in the same directory, flushed to disk, renamed
    /// over the target, directory entry flushed where the platform supports it.
    std::expected<void, std::string> writeFileDurably(const std::filesystem::path & path, std::string_view bytes);
}