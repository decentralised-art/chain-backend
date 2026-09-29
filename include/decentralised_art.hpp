#pragma once

#include <format>
#include <csignal>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "native.h"
#include "async.hpp"
#include "cmd.hpp"
#include "config.hpp"
#include "server.hpp"
#include "http.hpp"
#include "evm.hpp"
#include "pt.hpp"
#include "file.hpp"
#include "storage.hpp"
#include "registry.hpp"
#include "events.hpp"
#include "api.hpp"
#include "auth.hpp"
#include "utils.hpp"
#include "version.hpp"
#include "loader.hpp"
#include "chain.hpp"
#include "crypto.hpp"
#include "publish.hpp"

namespace dcn
{
    const asio::ip::port_type DEFAULT_PORT = 54321;
    const asio::ip::port_type DEFAULT_TLS_PORT = 54322;
}
