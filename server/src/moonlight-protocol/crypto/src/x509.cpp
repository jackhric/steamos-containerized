#include <crypto/crypto.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <optional>
#include <stdexcept>
#include <string>

namespace x509 {

pkey_ptr generate_key() {
  auto pkey = EVP_PKEY_new();
  if (!pkey) {
    throw std::runtime_error("Unable to create EVP_PKEY structure.");
  }

  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  if (!ctx) {
    throw std::runtime_error("Unable to generate 2048-bit RSA key.");
  }
  if (EVP_PKEY_keygen_init(ctx) <= 0) {
    throw std::runtime_error("Unable to generate 2048-bit RSA key.");
  }
  if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) {
    throw std::runtime_error("Unable to generate 2048-bit RSA key.");
  }
  if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
    throw std::runtime_error("Unable to generate 2048-bit RSA key.");
  }
  EVP_PKEY_CTX_free(ctx);

  return pkey_ptr(pkey, EVP_PKEY_free);
}

x509_ptr generate_x509(pkey_ptr pkey) {
  /* Allocate memory for the X509 structure. */
  X509 *x509 = X509_new();
  if (!x509) {
    throw std::runtime_error("Unable to create X509 structure.");
  }

  /* Set the serial number. */
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1); // Set the serial number.
  X509_set_version(x509, 2);

  auto valid_years = 630720000L; // This certificate is valid for 20 years
  X509_gmtime_adj(X509_get_notBefore(x509), 0);
  X509_gmtime_adj(X509_get_notAfter(x509), valid_years);

  /* Set the public key for our certificate. */
  X509_set_pubkey(x509, pkey.get());

  /* We want to copy the subject name to the issuer name. */
  X509_NAME *name = X509_get_subject_name(x509);

  /* Set the country code and common name. */
  X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char *)"IT", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (unsigned char *)"GamesOnWhales", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char *)"localhost", -1, -1, 0);
  X509_set_issuer_name(x509, name);

  /* Actually sign the certificate with our key. */
  if (!X509_sign(x509, pkey.get(), EVP_sha256())) {
    throw std::runtime_error("Error signing certificate.");
  }

  return x509_ptr(x509, X509_free);
}

x509_ptr cert_from_string(std::string_view cert) {
  BIO *bio;
  X509 *certificate;

  bio = BIO_new(BIO_s_mem());
  BIO_puts(bio, cert.data());
  certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);

  BIO_free(bio);
  return x509_ptr(certificate, X509_free);
}

x509_ptr cert_from_file(std::string_view cert_path) {
  X509 *certificate;
  BIO *bio;

  bio = BIO_new(BIO_s_file());
  if (BIO_read_filename(bio, cert_path.data()) <= 0) {
    throw std::runtime_error("Error reading certificate");
  }
  certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);

  BIO_free(bio);
  return x509_ptr(certificate, X509_free);
}

pkey_ptr pkey_from_file(std::string_view pkey_path) {
  EVP_PKEY *pkey;
  BIO *bio;

  bio = BIO_new(BIO_s_file());
  if (BIO_read_filename(bio, pkey_path.data()) <= 0) {
    throw std::runtime_error("Error reading private key");
  }
  pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);

  BIO_free(bio);
  return pkey_ptr(pkey, EVP_PKEY_free);
}

bool write_to_disk(pkey_ptr pkey, std::string_view pkey_filename, x509_ptr x509, std::string_view cert_filename) {
  // Both halves are written to temporaries and renamed into place only once both succeeded:
  // a key without its cert (or vice versa) is unrecoverable state that startup refuses to load.
  const std::string pkey_path{pkey_filename}, cert_path{cert_filename};
  const std::string pkey_tmp = pkey_path + ".tmp", cert_tmp = cert_path + ".tmp";

  auto write_pem = [](const std::string &path, const auto &writer) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
      throw std::runtime_error("Unable to open " + path + " for writing.");
    }
    bool ok = writer(f);
    fclose(f);
    if (!ok) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
      throw std::runtime_error("Unable to write " + path + " to disk.");
    }
  };

  write_pem(pkey_tmp, [&](FILE *f) {
    return PEM_write_PrivateKey(f, pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) != 0;
  });
  write_pem(cert_tmp, [&](FILE *f) { return PEM_write_X509(f, x509.get()) != 0; });

  std::error_code ec;
  std::filesystem::rename(pkey_tmp, pkey_path, ec);
  if (ec) {
    throw std::runtime_error("Unable to install " + pkey_path + ": " + ec.message());
  }
  std::filesystem::rename(cert_tmp, cert_path, ec);
  if (ec) {
    throw std::runtime_error("Unable to install " + cert_path + ": " + ec.message());
  }

  return true;
}

bool cert_exists(std::string_view pkey_filename, std::string_view cert_filename) {
  // std::fstream's default openmode is in|out, so a present-but-unwritable file reads as
  // missing -- and the caller's response to "missing" is to regenerate the server identity
  // over it. Ask the filesystem instead, and treat a zero-byte file (interrupted write) as
  // absent so we never load a truncated key.
  auto ok = [](std::string_view p) {
    std::error_code ec;
    std::filesystem::path path{std::string(p)};
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 0;
  };
  return ok(pkey_filename) && ok(cert_filename);
}

std::string get_cert_signature(x509_ptr cert) {
  const ASN1_BIT_STRING *asn1 = nullptr;
  X509_get0_signature(&asn1, nullptr, cert.get());

  return {(const char *)asn1->data, (std::size_t)asn1->length};
}

std::string get_cert_pem(x509_ptr cert) {
  BIO *bio_out = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio_out, cert.get());
  BUF_MEM *bio_buf;
  BIO_get_mem_ptr(bio_out, &bio_buf);
  std::string pem = std::string(bio_buf->data, bio_buf->length);
  BIO_free(bio_out);
  return pem;
}

std::string get_key_content(pkey_ptr pkey, bool private_key) {
  BIO *bio;

  bio = BIO_new(BIO_s_mem());

  if (private_key) {
    PEM_write_bio_PrivateKey(bio, pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
  } else {
    PEM_write_bio_PUBKEY(bio, pkey.get());
  }

  const int keylen = BIO_pending(bio);
  char *key = (char *)calloc(keylen + 1, 1);
  BIO_read(bio, key, keylen);
  BIO_free_all(bio);

  std::string result(key, static_cast<size_t>(keylen));
  free(key);
  return result;
}

std::string get_pkey_content(pkey_ptr pkey) {
  return get_key_content(std::move(pkey), true);
}

std::string get_cert_public_key(x509_ptr cert) {
  auto pkey = X509_get_pubkey(cert.get());
  if (!pkey) {
    return "";
  }
  return get_key_content(pkey_ptr(pkey, EVP_PKEY_free), false);
}

/**
 * @brief: adapted from Sunshine
 */
static int openssl_verify_cb(int ok, X509_STORE_CTX *ctx) {
  int err_code = X509_STORE_CTX_get_error(ctx);

  switch (err_code) {
  // TODO: Checking for X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY is a temporary workaround to get
  // moonlight-embedded to work on the raspberry pi
  case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    return 1;

  // Expired or not-yet-valid certificates are fine. Sometimes Moonlight is running on embedded devices
  // that don't have accurate clocks (or haven't yet synchronized by the time Moonlight first runs).
  // This behavior also matches what GeForce Experience does.
  case X509_V_ERR_CERT_NOT_YET_VALID:
  case X509_V_ERR_CERT_HAS_EXPIRED:
    return 1;

  default:
    return ok;
  }
}

/**
 * @brief: adapted from Sunshine
 */
std::optional<std::string> verification_error(x509_ptr paired_cert, x509_ptr untrusted_cert) {
  auto x509_store{X509_STORE_new()};
  X509_STORE_add_cert(x509_store, paired_cert.get());

  auto _cert_ctx{X509_STORE_CTX_new()};

  X509_STORE_CTX_init(_cert_ctx, x509_store, untrusted_cert.get(), nullptr);
  X509_STORE_CTX_set_verify_cb(_cert_ctx, openssl_verify_cb);

  // We don't care to validate the entire chain for the purposes of client auth.
  // Some versions of clients forked from Moonlight Embedded produce client certs
  // that OpenSSL doesn't detect as self-signed due to some X509v3 extensions.
  X509_STORE_CTX_set_flags(_cert_ctx, X509_V_FLAG_PARTIAL_CHAIN);

  auto err = X509_verify_cert(_cert_ctx);
  X509_STORE_free(x509_store);

  if (err == 1) {
    X509_STORE_CTX_free(_cert_ctx);
    return std::nullopt;
  }

  int err_code = X509_STORE_CTX_get_error(_cert_ctx);
  X509_STORE_CTX_free(_cert_ctx);
  return X509_verify_cert_error_string(err_code);
}

} // namespace x509