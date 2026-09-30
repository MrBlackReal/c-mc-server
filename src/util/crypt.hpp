#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// OpenSSL types, kept out of the header.
struct evp_pkey_st;
struct evp_cipher_ctx_st;

namespace mc::crypt {

// Port of net.minecraft.util.Crypt's key exchange: the server's RSA-1024 key pair.
class KeyPair {
public:
    static KeyPair generate(); // Crypt.generateKeyPair
    ~KeyPair();
    KeyPair(KeyPair&&) noexcept;
    KeyPair& operator=(KeyPair&&) noexcept;

    // PublicKey.getEncoded(): the X.509 SubjectPublicKeyInfo DER.
    const std::vector<uint8_t>& publicKeyDer() const { return der_; }
    // Crypt.decryptUsingKey (RSA/ECB/PKCS1Padding); empty on failure.
    std::optional<std::vector<uint8_t>> decrypt(std::span<const uint8_t> data) const;

private:
    KeyPair() = default;
    evp_pkey_st* key_ = nullptr;
    std::vector<uint8_t> der_;
};

// Crypt.digestData + new BigInteger(digest).toString(16): the server id hash sent to the session
// server (SHA-1 of the server id, the shared secret and the public key, as a signed hex number).
std::string serverIdHash(const std::string& serverId, std::span<const uint8_t> sharedSecret, std::span<const uint8_t> publicKeyDer);

// SignatureValidator: an RSA PKCS#1 v1.5 signature over `data` by the X.509 DER public key, with
// SHA-256 (SHA256withRSA: chat messages) or SHA-1 (SHA1withRSA: Mojang's services keys).
enum class Digest { Sha1, Sha256 };
bool verifySignature(std::span<const uint8_t> publicKeyDer, Digest digest, std::span<const uint8_t> data, std::span<const uint8_t> signature);
// Whether the DER decodes as an RSA public key (Crypt.byteToPublicKey).
bool isRsaPublicKey(std::span<const uint8_t> publicKeyDer);

// Crypt.getCipher: AES/CFB8/NoPadding with the shared secret as key and IV, as a stream.
class Cfb8Cipher {
public:
    Cfb8Cipher(std::span<const uint8_t> key, bool encrypt);
    ~Cfb8Cipher();
    Cfb8Cipher(const Cfb8Cipher&) = delete;
    Cfb8Cipher& operator=(const Cfb8Cipher&) = delete;
    void update(uint8_t* data, size_t size); // in place

private:
    evp_cipher_ctx_st* ctx_ = nullptr;
};

} // namespace mc::crypt
