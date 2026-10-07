#include "GrimmoryStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
constexpr size_t MAX_SECRET_LENGTH = 128;
// JWTs are much longer than passwords.
constexpr size_t MAX_TOKEN_LENGTH = 4096;

std::string readSecret(JsonVariantConst value, const char* label, const size_t maxLength = MAX_SECRET_LENGTH) {
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string secret = obfuscation::deobfuscateFromBase64(value | "", maxLength, &status);
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
  if (!cachedToken.empty()) doc["token_obf"] = obfuscation::obfuscateToBase64(cachedToken);
  doc["progressSync"] = progressSyncEnabled;
  doc["downloadFolder"] = downloadFolder;
}

bool GrimmoryStore::fromJson(JsonVariantConst doc) {
  serverUrl = doc["serverUrl"] | "";
  username = doc["username"] | "";
  password = readSecret(doc["password_obf"], "password");
  syncUsername = doc["syncUsername"] | "";
  syncKeyMd5 = readSecret(doc["syncKey_obf"], "sync key");
  cachedToken = doc["token_obf"].isNull() ? "" : readSecret(doc["token_obf"], "token", MAX_TOKEN_LENGTH);
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
  if (pass != password) cachedToken.clear();
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
  cachedToken.clear();
}

void GrimmoryStore::setCachedToken(const std::string& token) {
  ensureLoaded();
  cachedToken = token;
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

namespace GrimmoryBookIndex {
namespace {
constexpr char INDEX_PATH[] = "/.crosspoint/grimmory_books.json";
// Each entry is ~50 bytes; this keeps the file and its parse well under 64 KB.
constexpr size_t MAX_ENTRIES = 1000;
}  // namespace

int64_t idOf(JsonVariantConst entry) {
  // Older index files stored the bare ID.
  if (entry.is<JsonObjectConst>()) return entry["id"] | static_cast<int64_t>(0);
  return entry | static_cast<int64_t>(0);
}

bool save(const JsonDocument& doc) {
  if (!PersistableStoreBase::writeDocToFileAtomically(INDEX_PATH, doc)) {
    LOG_ERR("GRS", "Failed to save Grimmory book index");
    return false;
  }
  return true;
}

bool remember(const std::string& documentHash, const int64_t bookId) {
  if (documentHash.empty() || bookId <= 0) return false;
  JsonDocument doc;
  PersistableStoreBase::readDocFromFile(INDEX_PATH, doc);
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();
  JsonObject books = doc.as<JsonObject>();
  if (books[documentHash.c_str()].isNull() && books.size() >= MAX_ENTRIES) {
    // Drop the oldest entry (insertion order) to make room.
    books.remove(books.begin());
  }
  // A new download may be a different file version, so forget the last
  // percentage sent for it.
  books.remove(documentHash.c_str());
  JsonObject entry = books[documentHash.c_str()].to<JsonObject>();
  entry["id"] = bookId;
  return save(doc);
}

int64_t find(const std::string& documentHash) {
  if (documentHash.empty()) return 0;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(INDEX_PATH, doc)) return 0;
  return idOf(doc[documentHash.c_str()]);
}

float lastSentPercent(const std::string& documentHash) {
  if (documentHash.empty()) return -1.0f;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(INDEX_PATH, doc)) return -1.0f;
  JsonVariantConst entry = doc[documentHash.c_str()];
  if (!entry.is<JsonObjectConst>()) return -1.0f;
  return entry["pct"] | -1.0f;
}

void setLastSentPercent(const std::string& documentHash, const float percent) {
  if (documentHash.empty()) return;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(INDEX_PATH, doc)) return;
  JsonVariant entry = doc[documentHash.c_str()];
  if (entry.isNull()) return;
  if (!entry.is<JsonObject>()) {
    const int64_t id = idOf(entry);
    entry.to<JsonObject>()["id"] = id;
  }
  entry["pct"] = percent;
  save(doc);
}
}  // namespace GrimmoryBookIndex
