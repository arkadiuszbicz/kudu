// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/security/ca/cert_management.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>

#include <glog/logging.h>
#include <openssl/conf.h>
#ifndef OPENSSL_NO_ENGINE
#include <openssl/engine.h>
#endif
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "kudu/gutil/strings/substitute.h"
#include "kudu/security/openssl_util.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/status.h"

using std::lock_guard;
using std::move;
using std::ostringstream;
using std::string;
using strings::Substitute;

#define CERT_CHECK_OK(call) \
  CHECK_GT((call), 0)

#define CERT_RET_NOT_OK(call, msg) \
  if ((call) <= 0) { \
    return Status::RuntimeError(Substitute("$0: $1", \
        (msg), GetOpenSSLErrors())); \
  }

#define CERT_RET_IF_NULL(call, msg) \
  if ((call) == nullptr) { \
    return Status::RuntimeError(Substitute("$0: $1", \
        (msg), GetOpenSSLErrors())); \
  }

namespace kudu {
namespace security {
namespace ca {

const string& DataFormatToString(DataFormat fmt) {
  static const string kStrFormatUnknown = "UNKNOWN";
  static const string kStrFormatDer = "DER";
  static const string kStrFormatPem = "PEM";
  switch (fmt) {
    case DataFormat::DER:
      return kStrFormatDer;
    case DataFormat::PEM:
      return kStrFormatPem;
    default:
      return kStrFormatUnknown;
  }
}

Status BasicWrapper::FromFile(const string& fpath, DataFormat format) {
  c_unique_ptr<BIO> bio(BIO_new(BIO_s_file()), BIO_free);
  CERT_RET_NOT_OK(BIO_read_filename(bio.get(), fpath.c_str()),
                  Substitute("$0: could not read from file", fpath));
  RETURN_NOT_OK_PREPEND(FromBIO(bio.get(), format),
                        Substitute("$0: unable to load data key from file",
                                   fpath));
  return Status::OK();
}

Status BasicWrapper::FromString(const string& data, DataFormat format) {
  const void* mdata = reinterpret_cast<const void*>(data.data());
  c_unique_ptr<BIO> bio(BIO_new_mem_buf(
#if OPENSSL_VERSION_NUMBER < 0x10002000L
      const_cast<void*>(mdata),
#else
      mdata,
#endif
      data.size()), BIO_free);
  RETURN_NOT_OK_PREPEND(FromBIO(bio.get(), format),
                        "unable to load data from memory");
  return Status::OK();
}

Status BasicWrapper::ToString(std::string* data, DataFormat format) const {
  CHECK(data);
  c_unique_ptr<BIO> bio(BIO_new(BIO_s_mem()), BIO_free);
  RETURN_NOT_OK_PREPEND(ToBIO(bio.get(), format), "error serializing data");
  BUF_MEM* membuf;
  CERT_CHECK_OK(BIO_get_mem_ptr(bio.get(), &membuf));
  data->assign(membuf->data, membuf->length);
  return Status::OK();
}

void Key::AdoptRawData(RawDataType* data) {
  c_unique_ptr<RawDataType> d(data, EVP_PKEY_free);
  data_.swap(d);
}

Status Key::FromBIO(BIO* bio, DataFormat format) {
  CHECK(bio);
  c_unique_ptr<EVP_PKEY> key(nullptr, EVP_PKEY_free);
  switch (format) {
    case DataFormat::DER:
      key.reset(d2i_PrivateKey_bio(bio, nullptr));
      break;
    case DataFormat::PEM:
      key.reset(PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr));
      break;
    default:
      return Status::InvalidArgument(Substitute("$0: unrecognized format",
                                                static_cast<uint16_t>(format)));
  }
  if (PREDICT_FALSE(!key)) {
    return Status::RuntimeError(
        Substitute("error reading private key: $0", GetOpenSSLErrors()));
  }
  key.swap(data_);
  return Status::OK();
}

Status Key::ToBIO(BIO* bio, DataFormat format) const {
  CHECK(bio);
  CHECK(data_);
  switch (format) {
    case DataFormat::DER:
      CERT_RET_NOT_OK(i2d_PrivateKey_bio(bio, data_.get()),
                      "error exporting private key in DER format");
      break;
    case DataFormat::PEM:
      {
        c_unique_ptr<RSA> rsa(EVP_PKEY_get1_RSA(data_.get()), RSA_free);
        CERT_RET_NOT_OK(PEM_write_bio_RSAPrivateKey(bio, rsa.get(),
                                                    nullptr, nullptr, 0,
                                                    nullptr, nullptr),
                        "error exporting private key in PEM format");
      }
      break;
    default:
      return Status::InvalidArgument(Substitute("$0: unrecognized format",
                                                static_cast<uint16_t>(format)));
  }
  CERT_RET_NOT_OK(BIO_flush(bio), "error flushing BIO");
  return Status::OK();
}

void Cert::AdoptRawData(RawDataType* data) {
  c_unique_ptr<RawDataType> d(data, X509_free);
  data_.swap(d);
}

Status Cert::FromBIO(BIO* bio, DataFormat format) {
  CHECK(bio);
  c_unique_ptr<X509> cert(nullptr, X509_free);
  switch (format) {
    case DataFormat::DER:
      cert.reset(d2i_X509_bio(bio, nullptr));
      break;
    case DataFormat::PEM:
      cert.reset(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr));
      break;
    default: {
      string err_msg = Substitute("$0: unrecognized data format",
                                  static_cast<uint16_t>(format));
      LOG(DFATAL) << err_msg;
      return Status::InvalidArgument(err_msg);
    }
  }
  if (PREDICT_FALSE(!cert)) {
    return Status::RuntimeError(
        Substitute("error loading X509 certificate: $0", GetOpenSSLErrors()));
  }
  cert.swap(data_);
  return Status::OK();
}

Status Cert::ToBIO(BIO* bio, DataFormat format) const {
  CHECK(data_);
  CHECK(bio);
  switch (format) {
    case DataFormat::DER:
      CERT_RET_NOT_OK(i2d_X509_bio(bio, data_.get()),
                      "error exporting X509 certificate in DER format");
      break;
    case DataFormat::PEM:
      CERT_RET_NOT_OK(PEM_write_bio_X509(bio, data_.get()),
                      "error exporting X509 certificate in PEM format");
      break;
    default:
      return Status::InvalidArgument(Substitute("$0: unrecognized format",
                                                static_cast<uint16_t>(format)));
  }
  CERT_RET_NOT_OK(BIO_flush(bio), "error flushing BIO");
  return Status::OK();
}

void CertSignRequest::AdoptRawData(RawDataType* data) {
  c_unique_ptr<RawDataType> d(data, X509_REQ_free);
  data_.swap(d);
}

Status CertSignRequest::FromBIO(BIO* bio, DataFormat format) {
  CHECK(bio);
  c_unique_ptr<X509_REQ> req(nullptr, X509_REQ_free);
  switch (format) {
    case DataFormat::DER:
      req.reset(d2i_X509_REQ_bio(bio, nullptr));
      break;
    case DataFormat::PEM:
      req.reset(PEM_read_bio_X509_REQ(bio, nullptr, nullptr, nullptr));
      break;
    default: {
      string err_msg = Substitute("$0: unrecognized data format",
                                  static_cast<uint16_t>(format));
      LOG(DFATAL) << err_msg;
      return Status::InvalidArgument(err_msg);
    }
  }
  if (PREDICT_FALSE(!req)) {
    return Status::RuntimeError(
        Substitute("error loading X509 CSR: $0", GetOpenSSLErrors()));
  }
  req.swap(data_);
  return Status::OK();
}

Status CertSignRequest::ToBIO(BIO* bio, DataFormat format) const {
  CHECK(bio);
  CHECK(data_);
  switch (format) {
    case DataFormat::DER:
      CERT_RET_NOT_OK(i2d_X509_REQ_bio(bio, data_.get()),
                      "error exporting X509 CSR in DER format");
      break;
    case DataFormat::PEM:
      CERT_RET_NOT_OK(PEM_write_bio_X509_REQ(bio, data_.get()),
                      "error exporting X509 CSR in PEM format");
      break;
    default:
      return Status::InvalidArgument(Substitute("$0: unrecognized format",
                                                static_cast<uint16_t>(format)));
  }
  CERT_RET_NOT_OK(BIO_flush(bio), "error flushing BIO");
  return Status::OK();
}

Status GeneratePrivateKey(int num_bits, Key* ret) {
  CHECK(ret);
  InitializeOpenSSL();
  c_unique_ptr<EVP_PKEY> key(EVP_PKEY_new(), EVP_PKEY_free);
  {
    c_unique_ptr<BIGNUM> bn(BN_new(), BN_free);
    CERT_CHECK_OK(BN_set_word(bn.get(), RSA_F4));
    c_unique_ptr<RSA> rsa(RSA_new(), RSA_free);
    CERT_RET_NOT_OK(RSA_generate_key_ex(rsa.get(), num_bits, bn.get(), nullptr),
                    "error generating RSA key");
    CERT_RET_NOT_OK(EVP_PKEY_set1_RSA(key.get(), rsa.get()),
                    "error assigning RSA key");
  }
  ret->AdoptRawData(key.release());

  return Status::OK();
}

CertRequestGeneratorBase::CertRequestGeneratorBase(Config config)
    : config_(move(config)) {
}

CertRequestGenerator::~CertRequestGenerator() {
  sk_X509_EXTENSION_pop_free(extensions_, X509_EXTENSION_free);
}

Status CertRequestGeneratorBase::GenerateRequest(const Key& key,
                                                 CertSignRequest* ret) const {
  CHECK(ret);
  CHECK(Initialized());
  c_unique_ptr<X509_REQ> req(X509_REQ_new(), X509_REQ_free);
  CERT_RET_NOT_OK(X509_REQ_set_pubkey(req.get(), key.GetRawData()),
                  "error setting X509 public key");
  X509_NAME* name = X509_REQ_get_subject_name(req.get());
  CHECK(name);

#define CERT_SET_SUBJ_FIELD(field, code, err_msg) \
  do { \
    const string& f = (field); \
    if (!f.empty()) { \
      CERT_RET_NOT_OK(X509_NAME_add_entry_by_txt(name, (code), MBSTRING_ASC,  \
          reinterpret_cast<const unsigned char*>(f.c_str()), -1, -1, 0), \
         ("error setting subject " # err_msg)); \
    } \
  } while (false)

  CERT_SET_SUBJ_FIELD(config_.country, "C", "country");
  CERT_SET_SUBJ_FIELD(config_.state, "ST", "state");
  CERT_SET_SUBJ_FIELD(config_.locality, "L", "locality/city");
  CERT_SET_SUBJ_FIELD(config_.org, "O", "organization");
  CERT_SET_SUBJ_FIELD(config_.unit, "OU", "organizational unit");
  CERT_SET_SUBJ_FIELD(config_.uuid, "CN", "common name");
#undef CERT_SET_SUBJ_FIELD

  // Set necessary extensions into the request.
  RETURN_NOT_OK(SetExtensions(req.get()));

  // And finally sign the result.
  CERT_RET_NOT_OK(X509_REQ_sign(req.get(), key.GetRawData(), EVP_sha256()),
                  "error signing X509 request");
  ret->AdoptRawData(req.release());

  return Status::OK();
}

Status CertRequestGeneratorBase::PushExtension(stack_st_X509_EXTENSION* st,
                                               int32_t nid, const char* value) {
  c_unique_ptr<X509_EXTENSION> ex(
      X509V3_EXT_conf_nid(nullptr, nullptr, nid, const_cast<char*>(value)),
      X509_EXTENSION_free);
  if (!ex) {
    return Status::RuntimeError("error configuring extension");
  }
  CERT_RET_NOT_OK(sk_X509_EXTENSION_push(st, ex.release()),
                  "error pushing extension into the stack");
  return Status::OK();
}

CertRequestGenerator::CertRequestGenerator(Config config)
    : CertRequestGeneratorBase(config),
      extensions_(nullptr),
      is_initialized_(false) {
}

Status CertRequestGenerator::Init() {
  InitializeOpenSSL();

  lock_guard<simple_spinlock> guard(lock_);
  if (is_initialized_) {
    return Status::OK();
  }
  if (config_.uuid.empty()) {
    return Status::InvalidArgument("missing end-entity UUID/name");
  }
  // Check that the config contain at least one entity (DNS name/IP address)
  // to bind the generated certificate.
  if (config_.hostnames.empty() && config_.ips.empty()) {
    return Status::InvalidArgument("SAN: missing DNS names and IP addresses");
  }

  extensions_ = sk_X509_EXTENSION_new_null();

  // Permitted usages for the generated keys is set via X509 V3
  // standard/extended key usage attributes.
  // See https://www.openssl.org/docs/man1.0.1/apps/x509v3_config.html
  // for details.

  // The generated certificates are for using as TLS certificates for
  // both client and server.
  RETURN_NOT_OK(PushExtension(extensions_, NID_key_usage,
                              "critical,digitalSignature,keyEncipherment"));
  // The generated certificates should be good for authentication
  // of a server to a client and vice versa: the intended users of the
  // certificates are tablet servers which are going to talk to master
  // and other tablet servers via TLS channels.
  RETURN_NOT_OK(PushExtension(extensions_, NID_ext_key_usage,
                              "critical,serverAuth,clientAuth"));
  // The generated certificates are not intended to be used as CA certificates
  // (i.e. they cannot be used to sign/issue certificates).
  RETURN_NOT_OK(PushExtension(extensions_, NID_basic_constraints,
                              "critical,CA:FALSE"));
  ostringstream san_hosts;
  for (size_t i = 0; i < config_.hostnames.size(); ++i) {
    const string& hostname = config_.hostnames[i];
    if (hostname.empty()) {
      // Basic validation: check for emptyness. Probably, more advanced
      // validation is needed here.
      return Status::InvalidArgument("SAN: an empty hostname");
    }
    if (i != 0) {
      san_hosts << ",";
    }
    san_hosts << "DNS." << i << ":" << hostname;
  }
  ostringstream san_ips;
  for (size_t i = 0; i < config_.ips.size(); ++i) {
    const string& ip = config_.ips[i];
    if (ip.empty()) {
      // Basic validation: check for emptyness. Probably, more advanced
      // validation is needed here.
      return Status::InvalidArgument("SAN: an empty IP address");
    }
    if (i != 0) {
      san_ips << ",";
    }
    san_ips << "IP." << i << ":" << ip;
  }
  // Encode hostname and IP address into the subjectAlternativeName attribute.
  const string alt_name = san_hosts.str() +
      ((!san_hosts.str().empty() && !san_ips.str().empty()) ? "," : "") +
      san_ips.str();
  RETURN_NOT_OK(PushExtension(extensions_, NID_subject_alt_name,
                              alt_name.c_str()));
  if (!config_.comment.empty()) {
    // Add the comment if it's not empty.
    RETURN_NOT_OK(PushExtension(extensions_, NID_netscape_comment,
                                config_.comment.c_str()));
  }
  is_initialized_ = true;

  return Status::OK();
}

bool CertRequestGenerator::Initialized() const {
  lock_guard<simple_spinlock> guard(lock_);
  return is_initialized_;
}

Status CertRequestGenerator::SetExtensions(X509_REQ* req) const {
  CERT_RET_NOT_OK(X509_REQ_add_extensions(req, extensions_),
                  "error setting X509 request extensions");
  return Status::OK();
}

CaCertRequestGenerator::CaCertRequestGenerator(Config config)
    : CertRequestGeneratorBase(config),
      extensions_(nullptr),
      is_initialized_(false) {
}

CaCertRequestGenerator::~CaCertRequestGenerator() {
  sk_X509_EXTENSION_pop_free(extensions_, X509_EXTENSION_free);
}

Status CaCertRequestGenerator::Init() {
  InitializeOpenSSL();

  lock_guard<simple_spinlock> guard(lock_);
  if (is_initialized_) {
    return Status::OK();
  }
  if (config_.uuid.empty()) {
    return Status::InvalidArgument("missing CA service UUID/name");
  }

  extensions_ = sk_X509_EXTENSION_new_null();

  // Permitted usages for the generated keys is set via X509 V3
  // standard/extended key usage attributes.
  // See https://www.openssl.org/docs/man1.0.1/apps/x509v3_config.html
  // for details.

  // The target ceritifcate is a CA certificate: it's for signing X509 certs.
  RETURN_NOT_OK(PushExtension(extensions_, NID_key_usage,
                              "critical,keyCertSign"));
  // The generated certificates are for the private CA service.
  RETURN_NOT_OK(PushExtension(extensions_, NID_basic_constraints,
                              "critical,CA:TRUE"));
  if (!config_.comment.empty()) {
    // Add the comment if it's not empty.
    RETURN_NOT_OK(PushExtension(extensions_, NID_netscape_comment,
                                config_.comment.c_str()));
  }
  is_initialized_ = true;

  return Status::OK();
}

bool CaCertRequestGenerator::Initialized() const {
  lock_guard<simple_spinlock> guard(lock_);
  return is_initialized_;
}

Status CaCertRequestGenerator::SetExtensions(X509_REQ* req) const {
  CERT_RET_NOT_OK(X509_REQ_add_extensions(req, extensions_),
                  "error setting X509 request extensions");
  return Status::OK();
}

CertSigner::CertSigner(Config config)
    : config_(move(config)),
      is_initialized_(false) {
}

Status CertSigner::Init() {
  InitializeOpenSSL();

  lock_guard<simple_spinlock> guard(lock_);
  if (is_initialized_) {
    return Status::OK();
  }
  RETURN_NOT_OK(ca_cert_.FromFile(config_.ca_cert_path, DataFormat::PEM));
  RETURN_NOT_OK(ca_private_key_.FromFile(config_.ca_private_key_path,
                DataFormat::PEM));
  CERT_RET_NOT_OK(X509_check_private_key(ca_cert_.GetRawData(),
                                         ca_private_key_.GetRawData()),
                  Substitute("$0, $1: CA certificate and private key "
                             "do not match",
                             config_.ca_cert_path, config_.ca_private_key_path));
  is_initialized_ = true;
  return Status::OK();
}

bool CertSigner::Initialized() const {
  lock_guard<simple_spinlock> guard(lock_);
  return is_initialized_;
}

const Cert& CertSigner::ca_cert() const {
  lock_guard<simple_spinlock> guard(lock_);
  DCHECK(is_initialized_);
  return ca_cert_;
}

const Key& CertSigner::ca_private_key() const {
  lock_guard<simple_spinlock> guard(lock_);
  DCHECK(is_initialized_);
  return ca_private_key_;
}

Status CertSigner::Sign(const CertSignRequest& req, Cert* ret) const {
  CHECK(ret);
  CHECK(Initialized());
  c_unique_ptr<X509> x509(X509_new(), X509_free);
  RETURN_NOT_OK(FillCertTemplateFromRequest(req.GetRawData(), x509.get()));
  RETURN_NOT_OK(DoSign(EVP_sha256(), config_.exp_interval_sec, x509.get()));
  ret->AdoptRawData(x509.release());

  return Status::OK();
}

// This is modeled after code in copy_extensions() function from
// $OPENSSL_ROOT/apps/apps.c with OpenSSL 1.0.2.
Status CertSigner::CopyExtensions(X509_REQ* req, X509* x) {
  CHECK(req);
  CHECK(x);
  STACK_OF(X509_EXTENSION)* exts = X509_REQ_get_extensions(req);
  auto exts_cleanup = MakeScopedCleanup([&exts]() {
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
  });
  for (size_t i = 0; i < sk_X509_EXTENSION_num(exts); ++i) {
    X509_EXTENSION* ext = sk_X509_EXTENSION_value(exts, i);
    ASN1_OBJECT* obj = X509_EXTENSION_get_object(ext);
    int32_t idx = X509_get_ext_by_OBJ(x, obj, -1);
    if (idx != -1) {
      // If extension exits, delete all extensions of same type.
      do {
        c_unique_ptr<X509_EXTENSION> tmpext(X509_get_ext(x, idx),
                                       X509_EXTENSION_free);
        X509_delete_ext(x, idx);
        idx = X509_get_ext_by_OBJ(x, obj, -1);
      } while (idx != -1);
    }
    CERT_RET_NOT_OK(X509_add_ext(x, ext, -1), "error adding extension");
  }

  return Status::OK();
}

Status CertSigner::FillCertTemplateFromRequest(X509_REQ* req, X509* tmpl) {
  CHECK(req);
  if (!req->req_info ||
      !req->req_info->pubkey ||
      !req->req_info->pubkey->public_key ||
      !req->req_info->pubkey->public_key->data) {
    return Status::RuntimeError("corrupted CSR: no public key");
  }
  c_unique_ptr<EVP_PKEY> pub_key(X509_REQ_get_pubkey(req), EVP_PKEY_free);
  if (!pub_key) {
    return Status::RuntimeError("error unpacking public key from CSR");
  }
  const int rc = X509_REQ_verify(req, pub_key.get());
  if (rc < 0) {
    return Status::RuntimeError("CSR signature verification error");
  }
  if (rc == 0) {
    return Status::RuntimeError("CSR signature mismatch");
  }
  CERT_RET_NOT_OK(X509_set_subject_name(tmpl, X509_REQ_get_subject_name(req)),
                  "error setting cert subject name");
  RETURN_NOT_OK(CopyExtensions(req, tmpl));
  CERT_RET_NOT_OK(X509_set_pubkey(tmpl, pub_key.get()),
                  "error setting cert public key");
  return Status::OK();
}

Status CertSigner::DigestSign(const EVP_MD* md, EVP_PKEY* pkey, X509* x) {
  CERT_RET_NOT_OK(X509_sign(x, pkey, md), "error signing certificate");
  return Status::OK();
}

Status CertSigner::GenerateSerial(c_unique_ptr<ASN1_INTEGER>* ret) {
  c_unique_ptr<BIGNUM> btmp(BN_new(), BN_free);
  CERT_RET_NOT_OK(BN_pseudo_rand(btmp.get(), 64, 0, 0),
                  "error generating random number");
  c_unique_ptr<ASN1_INTEGER> serial(ASN1_INTEGER_new(), ASN1_INTEGER_free);
  CERT_RET_IF_NULL(BN_to_ASN1_INTEGER(btmp.get(), serial.get()),
                   "error converting number into ASN1 representation");
  if (ret) {
    ret->swap(serial);
  }
  return Status::OK();
}

Status CertSigner::DoSign(const EVP_MD* digest, int32_t exp_seconds,
                          X509* ret) const {
  // Version 3 (v3) of X509 certificates. The integer value is one less
  // than the version it represents. This is not a typo. :)
  static const int kX509V3 = 2;

  CERT_RET_NOT_OK(
      X509_set_issuer_name(CHECK_NOTNULL(ret),
                           X509_get_subject_name(ca_cert_.GetRawData())),
      "error setting issuer name");
  c_unique_ptr<ASN1_INTEGER> serial;
  RETURN_NOT_OK(GenerateSerial(&serial));
  // set version to v3
  CERT_RET_NOT_OK(X509_set_version(ret, kX509V3), "error setting cert version");
  CERT_RET_NOT_OK(X509_set_serialNumber(ret, serial.get()),
                  "error setting cert serial");
  CERT_RET_IF_NULL(X509_gmtime_adj(X509_get_notBefore(ret), 0L),
                   "error setting cert validity time");
  CERT_RET_IF_NULL(X509_gmtime_adj(X509_get_notAfter(ret), exp_seconds),
                   "error setting cert expiration time");
  RETURN_NOT_OK(DigestSign(digest, ca_private_key_.GetRawData(), ret));

  return Status::OK();
}

} // namespace ca
} // namespace security
} // namespace kudu