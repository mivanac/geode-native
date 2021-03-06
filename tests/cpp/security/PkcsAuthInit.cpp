/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PkcsAuthInit.hpp"

#include <openssl-compat.h>

#include <cstdio>
#include <string>
#include <util/Log.hpp>

#include <geode/CacheableBuiltins.hpp>
#include <geode/ExceptionTypes.hpp>
#include <geode/Properties.hpp>

#include "SerializationRegistry.hpp"
#include "security_export.h"

namespace apache {
namespace geode {
namespace client {
std::shared_ptr<CacheableString> convertBytesToString(const uint8_t* bytes,
                                                      int32_t length,
                                                      size_t maxLength) {
  if (bytes) {
    std::string str;
    size_t totalBytes = 0;
    char byteStr[20];
    for (int32_t index = 0; index < length; ++index) {
      int len = sprintf(byteStr, "%d ", bytes[index]);
      totalBytes += len;
      // no use going beyond maxLength since LOG* methods will truncate
      // in any case
      if (maxLength > 0 && totalBytes > maxLength) {
        break;
      }
      str.append(byteStr, len);
    }
    return CacheableString::create(str);
  }
  return CacheableString::create("");
}

extern "C" {
SECURITY_EXPORT AuthInitialize* createPKCSAuthInitInstance() {
  return new PKCSAuthInitInternal();
}

uint8_t* createSignature(EVP_PKEY* key, X509* cert,
                         const unsigned char* inputBuffer,
                         uint32_t inputBufferLen, unsigned int* signatureLen) {
  if (!key || !cert || !inputBuffer) {
    return nullptr;
  }
  const ASN1_OBJECT* macobj;
  const X509_ALGOR* algorithm = nullptr;
  X509_ALGOR_get0(&macobj, nullptr, nullptr, algorithm);
  const EVP_MD* signatureDigest = EVP_get_digestbyobj(macobj);
  EVP_MD_CTX* signatureCtx = EVP_MD_CTX_new();
  auto signatureData =
      std::unique_ptr<uint8_t[]>(new uint8_t[EVP_PKEY_size(key)]);
  bool result =
      (EVP_SignInit_ex(signatureCtx, signatureDigest, nullptr) &&
       EVP_SignUpdate(signatureCtx, inputBuffer, inputBufferLen) &&
       EVP_SignFinal(signatureCtx, signatureData.get(), signatureLen, key));
  EVP_MD_CTX_free(signatureCtx);
  if (result) {
    return signatureData.release();
  }
  return nullptr;
}

bool readPKCSPublicPrivateKey(FILE* keyStoreFP, const char* keyStorePassword,
                              EVP_PKEY** outPrivateKey, X509** outCertificate) {
  PKCS12* p12;

  if (!keyStoreFP || !keyStorePassword || (keyStorePassword[0] == '\0')) {
    return (false);
  }

  p12 = d2i_PKCS12_fp(keyStoreFP, nullptr);

  if (!p12) {
    return (false);
  }

  if (!PKCS12_parse(p12, keyStorePassword, outPrivateKey, outCertificate,
                    nullptr)) {
    return (false);
  }

  PKCS12_free(p12);

  return (outPrivateKey && outCertificate);
}

bool openSSLInit() {
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();

  return true;
}

static bool s_initDone = openSSLInit();
}
// end of extern "C"
std::shared_ptr<Properties> PKCSAuthInitInternal::getCredentials(
    const std::shared_ptr<Properties>& securityprops, const std::string&) {
  if (!s_initDone) {
    throw AuthenticationFailedException(
        "PKCSAuthInit::getCredentials: "
        "OpenSSL initialization failed.");
  }
  if (securityprops == nullptr || securityprops->getSize() <= 0) {
    throw AuthenticationRequiredException(
        "PKCSAuthInit::getCredentials: "
        "No security-* properties are set.");
  }

  auto keyStoreptr = securityprops->find(KEYSTORE_FILE_PATH1);

  const char* keyStorePath = keyStoreptr->value().c_str();

  if (!keyStorePath) {
    throw AuthenticationFailedException(
        "PKCSAuthInit::getCredentials: "
        "key-store file path property KEYSTORE_FILE_PATH not set.");
  }

  auto aliasptr = securityprops->find(KEYSTORE_ALIAS1);

  const char* alias = aliasptr->value().c_str();

  if (!alias) {
    throw AuthenticationFailedException(
        "PKCSAuthInit::getCredentials: "
        "key-store alias property KEYSTORE_ALIAS not set.");
  }

  auto keyStorePassptr = securityprops->find(KEYSTORE_PASSWORD1);

  const char* keyStorePass = keyStorePassptr->value().c_str();

  if (!keyStorePass) {
    throw AuthenticationFailedException(
        "PKCSAuthInit::getCredentials: "
        "key-store password property KEYSTORE_PASSWORD not set.");
  }

  FILE* keyStoreFP = fopen(keyStorePath, "r");
  if (!keyStoreFP) {
    char msg[1024];
    sprintf(msg, "PKCSAuthInit::getCredentials: Unable to open keystore %s",
            keyStorePath);
    throw AuthenticationFailedException(msg);
  }

  EVP_PKEY* privateKey = nullptr;
  X509* cert = nullptr;

  /* Read the Public and Private Key from keystore in file */
  if (!readPKCSPublicPrivateKey(keyStoreFP, keyStorePass, &privateKey, &cert)) {
    fclose(keyStoreFP);
    char msg[1024];
    sprintf(msg,
            "PKCSAuthInit::getCredentials: Unable to read PKCS "
            "public key from %s",
            keyStorePath);
    throw AuthenticationFailedException(msg);
  }

  fclose(keyStoreFP);
  unsigned int lengthEncryptedData = 0;

  auto signatureData = std::unique_ptr<uint8_t[]>(createSignature(
      privateKey, cert, reinterpret_cast<const unsigned char*>(alias),
      static_cast<uint32_t>(strlen(alias)), &lengthEncryptedData));
  EVP_PKEY_free(privateKey);
  X509_free(cert);
  if (!signatureData) {
    throw AuthenticationFailedException(
        "PKCSAuthInit::getCredentials: "
        "Unable to create signature");
  }
  std::shared_ptr<Cacheable> signatureValPtr;
  if (m_stringCredentials) {
    // convert signature bytes to base64
    signatureValPtr =
        convertBytesToString(signatureData.get(), lengthEncryptedData, 2048);
    LOGINFO(" Converting CREDS to STRING: %s",
            signatureValPtr->toString().c_str());
  } else {
    signatureValPtr = CacheableBytes::create(std::vector<int8_t>(
        signatureData.get(), signatureData.get() + lengthEncryptedData));
    LOGINFO(" Converting CREDS to BYTES: %s",
            signatureValPtr->toString().c_str());
  }
  auto credentials = Properties::create();
  credentials->insert(KEYSTORE_ALIAS1, alias);
  credentials->insert(CacheableString::create(SIGNATURE_DATA1),
                      signatureValPtr);
  return credentials;
}
}  // namespace client
}  // namespace geode
}  // namespace apache
