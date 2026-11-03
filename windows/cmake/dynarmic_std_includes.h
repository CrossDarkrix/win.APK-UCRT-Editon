// dynarmic_std_includes.h -- force-included into dynarmic and the libraries
// it bundles (CMakeLists.txt, WINAPK_ARM64_JIT). Their sources rely on
// standard headers that older libc++ pulled in through other headers and
// current libc++ no longer does.

#pragma once

#ifdef __cplusplus
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#endif
