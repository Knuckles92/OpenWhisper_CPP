#include "net/tls.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include "base/hash.h"

namespace fv::net {

namespace {

// tls.py's _VALIDITY_DAYS (20 * 365) and the day it backdates the start.
constexpr long kValidityDays = 20 * 365;
// x509.random_serial_number: 20 random bytes shifted right once, so below 2^159.
constexpr int kSerialBytes = 20;
// The upper bound of X.520's CommonName, which tls.py cuts the name to.
constexpr std::size_t kCommonNameMax = 64;

constexpr std::string_view kPemHeader = "-----BEGIN CERTIFICATE-----";
constexpr std::string_view kPemFooter = "-----END CERTIFICATE-----";

[[noreturn]] void fail(const char* what) {
    const unsigned long code = ERR_get_error();
    ERR_clear_error();
    std::string message = std::string("Could not create the host certificate: ") + what;
    if (code != 0) {
        char text[256];
        ERR_error_string_n(code, text, sizeof(text));
        message += std::string(" (") + text + ")";
    }
    throw std::runtime_error(message);
}

struct KeyFree {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
struct CertFree {
    void operator()(X509* certificate) const { X509_free(certificate); }
};
struct BioFree {
    void operator()(BIO* bio) const { BIO_free(bio); }
};
struct BnFree {
    void operator()(BIGNUM* number) const { BN_free(number); }
};

std::string bio_text(BIO* bio) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return size > 0 && data != nullptr ? std::string(data, static_cast<std::size_t>(size))
                                       : std::string();
}

// text[:64] of a Python str: whole UTF-8 code points.
std::string_view first_code_points(std::string_view text, std::size_t count) {
    std::size_t at = 0;
    for (std::size_t seen = 0; at < text.size() && seen < count; ++seen) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const std::size_t width = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        at = std::min(text.size(), at + width);
    }
    return text.substr(0, at);
}

// str.strip() of ASCII text: the characters Python counts as whitespace below 0x80.
std::string_view strip(std::string_view text) {
    constexpr std::string_view kSpace = " \t\n\r\v\f\x1c\x1d\x1e\x1f";
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

// binascii.a2b_base64(text) without strict_mode: characters outside the alphabet are skipped,
// and the data ends at the padding that completes a quantum.
std::vector<std::byte> a2b_base64(std::string_view text) {
    std::vector<std::byte> out;
    int quad_pos = 0;
    int pads = 0;
    unsigned left = 0;
    for (const char c : text) {
        if (c == '=') {
            if (quad_pos >= 2 && quad_pos + ++pads >= 4) {
                return out;
            }
            continue;
        }
        const int value = base64_value(c);
        if (value < 0) {
            continue;
        }
        const auto v = static_cast<unsigned>(value);
        pads = 0;
        switch (quad_pos) {
        case 0:
            quad_pos = 1;
            left = v;
            break;
        case 1:
            quad_pos = 2;
            out.push_back(static_cast<std::byte>(((left << 2) | (v >> 4)) & 0xff));
            left = v & 0x0f;
            break;
        case 2:
            quad_pos = 3;
            out.push_back(static_cast<std::byte>(((left << 4) | (v >> 2)) & 0xff));
            left = v & 0x03;
            break;
        default:
            quad_pos = 0;
            out.push_back(static_cast<std::byte>(((left << 6) | v) & 0xff));
            left = 0;
            break;
        }
    }
    if (quad_pos == 1) {
        // CPython counts the characters back from the bytes decoded.
        throw std::invalid_argument("Invalid base64-encoded string: number of data characters (" +
                                    std::to_string(out.size() / 3 * 4 + 1) +
                                    ") cannot be 1 more than a multiple of 4");
    }
    if (quad_pos != 0) {
        throw std::invalid_argument("Incorrect padding");
    }
    return out;
}

} // namespace

std::string certificate_sha256_from_der(std::span<const std::byte> der) {
    Sha256 hash;
    hash.update(der);
    std::string hex = hash.hexdigest();
    for (char& c : hex) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return hex;
}

std::string certificate_sha256_from_pem(std::string_view pem) {
    // ssl.PEM_cert_to_DER_cert tests the header before it strips the text: leading whitespace
    // is refused, trailing whitespace is not.
    if (!pem.starts_with(kPemHeader)) {
        throw std::invalid_argument("Invalid PEM encoding; must start with " +
                                    std::string(kPemHeader));
    }
    const std::string_view text = strip(pem);
    if (!text.ends_with(kPemFooter)) {
        throw std::invalid_argument("Invalid PEM encoding; must end with " +
                                    std::string(kPemFooter));
    }
    // text[len(header):-len(footer)]: empty when the two overlap.
    const std::string_view body =
        text.size() < kPemHeader.size() + kPemFooter.size()
            ? std::string_view()
            : text.substr(kPemHeader.size(), text.size() - kPemHeader.size() - kPemFooter.size());
    for (const char c : body) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            throw std::invalid_argument("Invalid PEM encoding; the certificate is not ASCII");
        }
    }
    return certificate_sha256_from_der(a2b_base64(body));
}

TlsIdentity generate_self_signed_p256(std::string_view common_name) {
    ERR_clear_error();
    std::unique_ptr<EVP_PKEY, KeyFree> key(EVP_EC_gen("P-256"));
    if (!key) {
        fail("no P-256 key");
    }
    std::unique_ptr<X509, CertFree> certificate(X509_new());
    if (!certificate || X509_set_version(certificate.get(), X509_VERSION_3) != 1) {
        fail("no certificate");
    }

    std::unique_ptr<BIGNUM, BnFree> serial(BN_new());
    std::array<unsigned char, kSerialBytes> random{};
    do {
        if (!serial || RAND_bytes(random.data(), kSerialBytes) != 1 ||
            BN_bin2bn(random.data(), kSerialBytes, serial.get()) == nullptr ||
            BN_rshift1(serial.get(), serial.get()) != 1) {
            fail("no serial number");
        }
    } while (BN_is_zero(serial.get()));
    if (BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get())) == nullptr) {
        fail("no serial number");
    }

    std::time_t now = std::time(nullptr);
    if (X509_time_adj_ex(X509_getm_notBefore(certificate.get()), -1, 0, &now) == nullptr ||
        X509_time_adj_ex(X509_getm_notAfter(certificate.get()), kValidityDays, 0, &now) ==
            nullptr) {
        fail("no validity period");
    }

    const std::string_view name = first_code_points(common_name, kCommonNameMax);
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>(name.data()),
                                   static_cast<int>(name.size()), -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 ||
        X509_set_pubkey(certificate.get(), key.get()) != 1) {
        fail("no subject");
    }
    if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
        fail("not signed");
    }

    std::unique_ptr<BIO, BioFree> cert_bio(BIO_new(BIO_s_mem()));
    std::unique_ptr<BIO, BioFree> key_bio(BIO_new(BIO_s_mem()));
    if (!cert_bio || !key_bio || PEM_write_bio_X509(cert_bio.get(), certificate.get()) != 1 ||
        PEM_write_bio_PKCS8PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                      nullptr) != 1) {
        fail("not written");
    }
    TlsIdentity identity;
    identity.cert_pem = bio_text(cert_bio.get());
    identity.key_pem = bio_text(key_bio.get());
    identity.disable_tickets = true;
    return identity;
}

} // namespace fv::net
