#include "GrimmoryStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
constexpr size_t MAX_SECRET_LENGTH = 128;

std::string readSecret(JsonVariantConst value, const char* label) {
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string secret = obfuscation::deobfuscateFromBase64(value | "", MAX_SECRET_LENGTH, &status);
  if (status != obfuscation::DecodeStatus::VALIDATED && status != obfuscation::DecodeStatus::LEGACY) {
    if (status != obfuscation::DecodeStatus::EMPTY) LOG_ERR("GRS", "Ignoring unreadable Grimmory %s", label);
    return "";
  }
  return secret;
}
}  // namespace

void GrimmoryStore::toJson(JsonDocument& doc) const {
  // Serialize fields directly: getters lazy-load and saveToFile() holds the store mutex.
  doc["serverUrl"] = serverUrl;
  doc["username"] = username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(password);
  doc["syncUsername"] = syncUsername;
  doc["syncKey_obf"] = obfuscation::obfuscateToBase64(syncKeyMd5);
  doc["progressSync"] = progressSyncEnabled;
  doc["downloadFolder"] = downloadFolder;
}

bool GrimmoryStore::fromJson(JsonVariantConst doc) {
  serverUrl = doc["serverUrl"] | "";
  username = doc["username"] | "";
  password = readSecret(doc["password_obf"], "password");
  syncUsername = doc["syncUsername"] | "";
  syncKeyMd5 = readSecret(doc["syncKey_obf"], "sync key");
  progressSyncEnabled = doc["progressSync"] | true;
  const char* folder = doc["downloadFolder"] | "/Grimmory";
  downloadFolder = (folder[0] == '/') ? folder : "/Grimmory";
  return true;
}

void GrimmoryStore::setServerUrl(const std::string& url) {
  ensureLoaded();
  if (url != serverUrl) clearSyncCredentials();
  serverUrl = url;
}

void GrimmoryStore::setUsername(const std::string& user) {
  ensureLoaded();
  if (user != username) clearSyncCredentials();
  username = user;
}

void GrimmoryStore::setPassword(const std::string& pass) {
  ensureLoaded();
  password = pass;
}

void GrimmoryStore::setSyncCredentials(const std::string& user, const std::string& keyMd5) {
  ensureLoaded();
  syncUsername = user;
  syncKeyMd5 = keyMd5;
}

void GrimmoryStore::clearSyncCredentials() {
  ensureLoaded();
  syncUsername.clear();
  syncKeyMd5.clear();
}

void GrimmoryStore::setProgressSyncEnabled(const bool enabled) {
  ensureLoaded();
  progressSyncEnabled = enabled;
}

void GrimmoryStore::setDownloadFolder(const std::string& folder) {
  ensureLoaded();
  downloadFolder = (!folder.empty() && folder[0] == '/') ? folder : "/Grimmory";
  while (downloadFolder.size() > 1 && downloadFolder.back() == '/') downloadFolder.pop_back();
}

bool GrimmoryStore::hasAccount() const {
  ensureLoaded();
  return !serverUrl.empty() && !username.empty() && !password.empty();
}

bool GrimmoryStore::hasSyncCredentials() const {
  ensureLoaded();
  return !serverUrl.empty() && !syncUsername.empty() && !syncKeyMd5.empty();
}

std::string GrimmoryStore::getBaseUrl() const {
  ensureLoaded();
  if (serverUrl.empty()) return "";
  // Self-hosted servers without a scheme are usually plain HTTP on the LAN,
  // matching the KOReader sync URL normalization.
  std::string url = serverUrl.find("://") == std::string::npos ? "http://" + serverUrl : serverUrl;
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

std::string GrimmoryStore::getKoSyncBaseUrl() const {
  const std::string base = getBaseUrl();
  return base.empty() ? "" : base + "/api/koreader";
}
