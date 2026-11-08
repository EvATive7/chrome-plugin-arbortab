// Extension <-> native message protocol version range (see AGENTS.md, "版本与兼容").
//
// The extension and the native module are released together but updated independently (the
// store updates the extension, users replace version.dll by hand), so compatibility is decided
// by this number, never by the release version. Bump kProtocolMax for every incompatible
// change to an existing message; raise kProtocolMin only when the module drops support for an
// old protocol. Additive fields that old peers can ignore do not need a bump.
#pragma once

namespace arbortab {

constexpr int kProtocolMin = 1;
constexpr int kProtocolMax = 1;

}  // namespace arbortab
