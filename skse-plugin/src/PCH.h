#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

// Windows headers go after CommonLib, and WinSock2 before Windows.h.
#include <WinSock2.h>
#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace std::literals;

namespace logger = SKSE::log;
namespace stl = SKSE::stl;
