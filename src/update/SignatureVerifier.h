#pragma once

#include <string>
#include <string_view>

namespace cheburnet::update {

enum class SignatureStatus { Verified, UnknownKey, InvalidEncoding, InvalidSignature, CryptoFailure };

// Signature is base64 of the 64-byte IEEE-P1363 r||s value. The exact manifest
// bytes are hashed with SHA-256; no JSON reserialization occurs.
SignatureStatus VerifyManifestSignature(std::string_view manifestBytes,
                                        std::string_view signatureBase64,
                                        std::string_view keyId);

// Exposed for deterministic test vectors and controlled key rotation tests.
SignatureStatus VerifyEcdsaP256(std::string_view bytes, std::string_view signatureBase64,
                               const unsigned char x[32], const unsigned char y[32]);

} // namespace cheburnet::update
