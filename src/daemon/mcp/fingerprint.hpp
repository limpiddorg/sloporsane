#ifndef SOS_MCP_FINGERPRINT_HPP
#define SOS_MCP_FINGERPRINT_HPP

#include <openssl/sha.h>
#include <string>
#include <string_view>

namespace mcp {
// Stable SHA-256 hex encoding shared by credential and runtime-tool identities.
inline std::string fingerprint(std::string_view bytes) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    result.reserve(2 * SHA256_DIGEST_LENGTH);
    for (const auto byte : digest) { result += HEX[byte >> 4]; result += HEX[byte & 15]; }
    return result;
}
}

#endif // SOS_MCP_FINGERPRINT_HPP
