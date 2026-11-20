// Shared precompiled header of every Qt-free target (fv_pch_core). Only headers that nearly every
// module includes belong here; a module still includes what it uses.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// The engine-only build (the default preset) has no vcpkg and so no nlohmann-json.
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#endif
