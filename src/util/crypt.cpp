#include "util/crypt.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <stdexcept>
#include <utility>

namespace mc::crypt {

KeyPair KeyPair::generate() {
    KeyPair kp;
    kp.key_ = EVP_RSA_gen(1024);
    if (!kp.key_) throw std::runtime_error("RSA key generation failed");
    unsigned char* buf = nullptr;
    int len = i2d_PUBKEY(kp.key_, &buf);
    if (len <= 0) throw std::runtime_error("public key encoding failed");
    kp.der_.assign(buf, buf + len);
    OPENSSL_free(buf);
    return kp;
}

KeyPair::~KeyPair() {
    if (key_) EVP_PKEY_free(key_);
}
KeyPair::KeyPair(KeyPair&& o) noexcept : key_(std::exchange(o.key_, nullptr)), der_(std::move(o.der_)) {}
KeyPair& KeyPair::operator=(KeyPair&& o) noexcept {
    std::swap(key_, o.key_);
    std::swap(der_, o.der_);
    return *this;
}

std::optional<std::vector<uint8_t>> KeyPair::decrypt(std::span<const uint8_t> data) const {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key_, nullptr);
    if (!ctx) return std::nullopt;
    std::optional<std::vector<uint8_t>> result;
    size_t outLen = 0;
    if (EVP_PKEY_decrypt_init(ctx) > 0 && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) > 0 &&
        EVP_PKEY_decrypt(ctx, nullptr, &outLen, data.data(), data.size()) > 0) {
        std::vector<uint8_t> out(outLen);
        if (EVP_PKEY_decrypt(ctx, out.data(), &outLen, data.data(), data.size()) > 0) {
            out.resize(outLen);
            result = std::move(out);
        }
    }
    EVP_PKEY_CTX_free(ctx);
    return result;
}

std::string serverIdHash(const std::string& serverId, std::span<const uint8_t> sharedSecret, std::span<const uint8_t> publicKeyDer) {
    uint8_t digest[SHA_DIGEST_LENGTH];
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md, EVP_sha1(), nullptr);
    EVP_DigestUpdate(md, serverId.data(), serverId.size()); // (ISO-8859-1: the id is ASCII)
    EVP_DigestUpdate(md, sharedSecret.data(), sharedSecret.size());
    EVP_DigestUpdate(md, publicKeyDer.data(), publicKeyDer.size());
    unsigned int len = 0;
    EVP_DigestFinal_ex(md, digest, &len);
    EVP_MD_CTX_free(md);
    // BigInteger(byte[]) is two's complement: a negative number prints as "-" and its magnitude.
    bool negative = digest[0] & 0x80;
    if (negative) {
        bool carry = true;
        for (int i = SHA_DIGEST_LENGTH - 1; i >= 0; i--) {
            digest[i] = static_cast<uint8_t>(~digest[i]);
            if (carry) carry = ++digest[i] == 0;
        }
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : digest) hex += kHex[b >> 4], hex += kHex[b & 15];
    size_t first = hex.find_first_not_of('0');
    hex = first == std::string::npos ? "0" : hex.substr(first);
    return negative ? "-" + hex : hex;
}

namespace {
EVP_PKEY* decodePublicKey(std::span<const uint8_t> der) {
    const unsigned char* p = der.data();
    EVP_PKEY* key = d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size()));
    if (key && EVP_PKEY_get_base_id(key) != EVP_PKEY_RSA) {
        EVP_PKEY_free(key);
        return nullptr;
    }
    return key;
}
} // namespace

bool isRsaPublicKey(std::span<const uint8_t> publicKeyDer) {
    EVP_PKEY* key = decodePublicKey(publicKeyDer);
    if (key) EVP_PKEY_free(key);
    return key != nullptr;
}

bool verifySignature(std::span<const uint8_t> publicKeyDer, Digest digest, std::span<const uint8_t> data, std::span<const uint8_t> signature) {
    EVP_PKEY* key = decodePublicKey(publicKeyDer);
    if (!key) return false;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(md, nullptr, digest == Digest::Sha1 ? EVP_sha1() : EVP_sha256(), nullptr, key) == 1 &&
              EVP_DigestVerify(md, signature.data(), signature.size(), data.data(), data.size()) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(key);
    return ok;
}

Cfb8Cipher::Cfb8Cipher(std::span<const uint8_t> key, bool encrypt) {
    if (key.size() != 16) throw std::runtime_error("AES-128 needs a 16 byte key");
    ctx_ = EVP_CIPHER_CTX_new();
    if (!ctx_ || EVP_CipherInit_ex(ctx_, EVP_aes_128_cfb8(), nullptr, key.data(), key.data(), encrypt ? 1 : 0) != 1)
        throw std::runtime_error("cipher setup failed");
}

Cfb8Cipher::~Cfb8Cipher() {
    if (ctx_) EVP_CIPHER_CTX_free(ctx_);
}

void Cfb8Cipher::update(uint8_t* data, size_t size) {
    while (size > 0) {
        int chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
        int outLen = 0;
        if (EVP_CipherUpdate(ctx_, data, &outLen, data, chunk) != 1 || outLen != chunk) throw std::runtime_error("cipher update failed");
        data += chunk;
        size -= static_cast<size_t>(chunk);
    }
}

} // namespace mc::crypt
