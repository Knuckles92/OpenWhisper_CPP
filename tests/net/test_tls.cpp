// The host's self-signed identity (tls.py _generate) and certificate fingerprints
// (protocol.certificate_fingerprint over ssl.PEM_cert_to_DER_cert).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "net/tls.h"

using namespace fv::net;

namespace {

// A P-256 certificate made once with the openssl tool. Its SHA-256 was computed with Python:
// hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest().upper().
constexpr std::string_view kFixturePem = R"(-----BEGIN CERTIFICATE-----
MIIBgzCCASmgAwIBAgIUPYagGXB4mxgaHmkNP6AQVCzhmx8wCgYIKoZIzj0EAwIw
FzEVMBMGA1UEAwwMRml4dHVyZSBob3N0MB4XDTI2MTExNjA3MTgzN1oXDTQ2MTEx
MTA3MTgzN1owFzEVMBMGA1UEAwwMRml4dHVyZSBob3N0MFkwEwYHKoZIzj0CAQYI
KoZIzj0DAQcDQgAEp8dLlNRy2ehIlPeIStq5/QuM2ebUgLfgOD6djCSYQt8dmDcx
V46lcTV5iQ9EZKTsh7LW5YExohkebXakbMX8EqNTMFEwHQYDVR0OBBYEFJDsEyS8
DWg5Vlxsz1041G+ViJoxMB8GA1UdIwQYMBaAFJDsEyS8DWg5Vlxsz1041G+ViJox
MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIgLuqvmDJ2q80Lra3I
VISXwgHwPNd52ERr47gGU4gsYDECIQDbRTXC3zhhSscA/l0Aw2ebIcBKl2ap5Fbb
9wGYv674uQ==
-----END CERTIFICATE-----
)";
constexpr std::string_view kFixtureSha256 =
    "703029C12E6FF3E19BC8C9A74A5D378404502C97AF4C683CDA604A4B14544DE6";
constexpr std::size_t kFixtureDerBytes = 391;

struct X509Free {
    void operator()(X509* certificate) const { X509_free(certificate); }
};
struct KeyFree {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
struct BioFree {
    void operator()(BIO* bio) const { BIO_free(bio); }
};

std::unique_ptr<X509, X509Free> read_certificate(std::string_view pem) {
    std::unique_ptr<BIO, BioFree> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return std::unique_ptr<X509, X509Free>(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

std::unique_ptr<EVP_PKEY, KeyFree> read_key(std::string_view pem) {
    std::unique_ptr<BIO, BioFree> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return std::unique_ptr<EVP_PKEY, KeyFree>(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

std::vector<std::byte> der_of(X509* certificate) {
    unsigned char* der = nullptr;
    const int size = i2d_X509(certificate, &der);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::memcpy(bytes.data(), der, bytes.size());
    OPENSSL_free(der);
    return bytes;
}

std::string common_name(X509_NAME* name) {
    const int at = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (at < 0) {
        return {};
    }
    const ASN1_STRING* data = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, at));
    return std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(data)),
                       static_cast<std::size_t>(ASN1_STRING_length(data)));
}

} // namespace

TEST_CASE("the fingerprint of a DER certificate is its uppercase SHA-256", "[platform]") {
    const auto certificate = read_certificate(kFixturePem);
    REQUIRE(certificate);
    const auto der = der_of(certificate.get());
    CHECK(der.size() == kFixtureDerBytes);
    CHECK(certificate_sha256_from_der(der) == kFixtureSha256);
}

TEST_CASE("the fingerprint of a PEM certificate is that of its DER", "[platform]") {
    CHECK(certificate_sha256_from_pem(kFixturePem) == kFixtureSha256);
    // PEM_cert_to_DER_cert strips the text before it looks for the footer.
    CHECK(certificate_sha256_from_pem(std::string(kFixturePem) + " \r\n\n") == kFixtureSha256);
    // Without line breaks inside, as a2b_base64 skips them anyway.
    std::string joined;
    for (const char c : kFixturePem) {
        if (c != '\n') {
            joined += c;
        }
    }
    CHECK(certificate_sha256_from_pem(joined) == kFixtureSha256);
}

TEST_CASE("PEM_cert_to_DER_cert's refusals throw", "[platform]") {
    CHECK_THROWS_AS(certificate_sha256_from_pem(""), std::invalid_argument);
    // The header is looked for before the text is stripped.
    CHECK_THROWS_WITH(certificate_sha256_from_pem("\r\n  " + std::string(kFixturePem)),
                      "Invalid PEM encoding; must start with -----BEGIN CERTIFICATE-----");
    // Header and footer sharing their dashes leave nothing between: the DER is empty.
    CHECK(certificate_sha256_from_pem("-----BEGIN CERTIFICATE-----END CERTIFICATE-----") ==
          "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
    CHECK_THROWS_WITH(certificate_sha256_from_pem("MIIB\n-----END CERTIFICATE-----"),
                      "Invalid PEM encoding; must start with -----BEGIN CERTIFICATE-----");
    CHECK_THROWS_WITH(certificate_sha256_from_pem("-----BEGIN CERTIFICATE-----\nMIIB\n"),
                      "Invalid PEM encoding; must end with -----END CERTIFICATE-----");
    // A data character left over after whole quanta.
    CHECK_THROWS_WITH(
        certificate_sha256_from_pem(
            "-----BEGIN CERTIFICATE-----\nMIIBg\n-----END CERTIFICATE-----"),
        "Invalid base64-encoded string: number of data characters (5) cannot be 1 more than a "
        "multiple of 4");
    CHECK_THROWS_WITH(certificate_sha256_from_pem(
                          "-----BEGIN CERTIFICATE-----\nMIIBgz\n-----END CERTIFICATE-----"),
                      "Incorrect padding");
    // A private key is no certificate.
    CHECK_THROWS_AS(certificate_sha256_from_pem(generate_self_signed_p256("x").key_pem),
                    std::invalid_argument);
}

TEST_CASE("generate_self_signed_p256 makes OpenWhisper's host certificate", "[platform]") {
    const std::time_t before = std::time(nullptr);
    const TlsIdentity identity = generate_self_signed_p256("OpenWhisper_CPP host x");
    const std::time_t after = std::time(nullptr);

    CHECK(identity.cert_pem.starts_with("-----BEGIN CERTIFICATE-----\n"));
    CHECK(identity.key_pem.starts_with("-----BEGIN PRIVATE KEY-----\n")); // PKCS#8, unencrypted
    CHECK(identity.disable_tickets);

    const auto certificate = read_certificate(identity.cert_pem);
    REQUIRE(certificate);
    const auto key = read_key(identity.key_pem);
    REQUIRE(key);
    CHECK(X509_check_private_key(certificate.get(), key.get()) == 1);

    // X.509 v3, subject = issuer = one CommonName, no extensions.
    CHECK(X509_get_version(certificate.get()) == X509_VERSION_3);
    CHECK(X509_NAME_cmp(X509_get_subject_name(certificate.get()),
                        X509_get_issuer_name(certificate.get())) == 0);
    CHECK(X509_NAME_entry_count(X509_get_subject_name(certificate.get())) == 1);
    CHECK(common_name(X509_get_subject_name(certificate.get())) == "OpenWhisper_CPP host x");
    CHECK(X509_get_ext_count(certificate.get()) == 0);

    // EC P-256, signed with ECDSA SHA-256.
    CHECK(EVP_PKEY_get_base_id(key.get()) == EVP_PKEY_EC);
    char group[64] = {};
    std::size_t group_length = 0;
    REQUIRE(EVP_PKEY_get_group_name(key.get(), group, sizeof(group), &group_length) == 1);
    CHECK(std::string(group, group_length) == "prime256v1");
    CHECK(X509_get_signature_nid(certificate.get()) == NID_ecdsa_with_SHA256);
    CHECK(X509_verify(certificate.get(), key.get()) == 1);

    // A random positive serial below 2^159.
    std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(
        ASN1_INTEGER_to_BN(X509_get0_serialNumber(certificate.get()), nullptr), &BN_free);
    REQUIRE(serial);
    CHECK_FALSE(BN_is_negative(serial.get()));
    CHECK_FALSE(BN_is_zero(serial.get()));
    CHECK(BN_num_bits(serial.get()) <= 159);

    // Valid from a day ago for 7,300 days after now: 7,301 days in all.
    int days = 0;
    int seconds = 0;
    REQUIRE(ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(certificate.get()),
                           X509_get0_notAfter(certificate.get())) == 1);
    CHECK(days == 7301);
    CHECK(seconds == 0);
    // X509_cmp_time: -1 when the certificate's time is not later than the one given, else 1.
    std::time_t day_ago_early = before - 86'400 - 1;
    std::time_t day_ago_late = after - 86'400;
    CHECK(X509_cmp_time(X509_get0_notBefore(certificate.get()), &day_ago_early) == 1);
    CHECK(X509_cmp_time(X509_get0_notBefore(certificate.get()), &day_ago_late) == -1);

    CHECK(certificate_sha256_from_pem(identity.cert_pem) ==
          certificate_sha256_from_der(der_of(certificate.get())));
}

TEST_CASE("two identities have different keys and serials", "[platform]") {
    const auto first = generate_self_signed_p256("OpenWhisper_CPP host x");
    const auto second = generate_self_signed_p256("OpenWhisper_CPP host x");
    CHECK(first.key_pem != second.key_pem);
    CHECK(certificate_sha256_from_pem(first.cert_pem) !=
          certificate_sha256_from_pem(second.cert_pem));
}

TEST_CASE("the common name is cut to 64 code points, as tls.py slices it", "[platform]") {
    // "OpenWhisper_CPP host " and a host name of 60 two-byte characters: 75 code points.
    std::string host;
    for (int i = 0; i < 60; ++i) {
        host += "\xC3\xA9"; // e acute
    }
    const auto identity = generate_self_signed_p256("OpenWhisper_CPP host " + host);
    const auto certificate = read_certificate(identity.cert_pem);
    REQUIRE(certificate);
    std::string expected = "OpenWhisper_CPP host ";
    for (int i = 0; i < 64 - 15; ++i) {
        expected += "\xC3\xA9";
    }
    CHECK(common_name(X509_get_subject_name(certificate.get())) == expected);
}
