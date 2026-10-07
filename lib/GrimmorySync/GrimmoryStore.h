#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

/**
 * Grimmory account settings, stored on the SD card.
 *
 * The user enters their Grimmory server URL, username and password once.
 * "Connect" logs in to the Grimmory REST API and caches the KOReader-sync
 * credentials Grimmory keeps for that account, so progress sync can use
 * Grimmory's KOReader-compatible endpoint (<server>/api/koreader) without a
 * second login. Passwords use the same MAC-tied obfuscation as the KOReader
 * credential store (not cryptographically secure).
 */
class GrimmoryStore : public PersistableStore<GrimmoryStore> {
 private:
  std::string serverUrl;
  std::string username;
  std::string password;
  // KOReader-sync account Grimmory keeps for this user (fetched on Connect).
  std::string syncUsername;
  std::string syncKeyMd5;
  // Grimmory access token from the last login (valid ~2 hours server-side),
  // reused so each sync does not log in again.
  std::string cachedToken;
  bool progressSyncEnabled = true;
  std::string downloadFolder = "/Grimmory";

  GrimmoryStore() = default;
  ~GrimmoryStore() = default;

  friend class PersistableStore<GrimmoryStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/grimmory.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::string& getServerUrl() const {
    ensureLoaded();
    return serverUrl;
  }
  const std::string& getUsername() const {
    ensureLoaded();
    return username;
  }
  const std::string& getPassword() const {
    ensureLoaded();
    return password;
  }
  const std::string& getSyncUsername() const {
    ensureLoaded();
    return syncUsername;
  }
  const std::string& getSyncKeyMd5() const {
    ensureLoaded();
    return syncKeyMd5;
  }
  const std::string& getCachedToken() const {
    ensureLoaded();
    return cachedToken;
  }
  bool getProgressSyncEnabled() const {
    ensureLoaded();
    return progressSyncEnabled;
  }
  const std::string& getDownloadFolder() const {
    ensureLoaded();
    return downloadFolder;
  }

  // Changing the server or account drops the cached sync credentials, which
  // belong to the previous account.
  void setServerUrl(const std::string& url);
  void setUsername(const std::string& user);
  void setPassword(const std::string& pass);
  void setSyncCredentials(const std::string& user, const std::string& keyMd5);
  void clearSyncCredentials();
  void setCachedToken(const std::string& token);
  void setProgressSyncEnabled(bool enabled);
  void setDownloadFolder(const std::string& folder);

  // Server URL, username and password are all set.
  bool hasAccount() const;
  // Connect has succeeded and cached the KOReader-sync credentials.
  bool hasSyncCredentials() const;
  // Progress sync should go to Grimmory instead of the KOReader sync server.
  bool isProgressSyncActive() const { return getProgressSyncEnabled() && hasSyncCredentials(); }

  // Normalized base URL (scheme added, trailing slashes removed). Empty if unset.
  std::string getBaseUrl() const;
  // Grimmory's KOReader-compatible sync endpoint.
  std::string getKoSyncBaseUrl() const;
};

#define GRIMMORY_STORE GrimmoryStore::getInstance()

/**
 * Maps the KOReader partial-MD5 hash of each book downloaded from Grimmory to
 * its Grimmory book ID, stored in /.crosspoint/grimmory_books.json.
 *
 * Grimmory's REST progress endpoint needs the book ID, and the server offers no
 * lookup by file hash, so only books downloaded through Grimmory Library can
 * update Grimmory's main reading progress.
 */
namespace GrimmoryBookIndex {
// Records (or replaces) the book ID for a file hash.
bool remember(const std::string& documentHash, int64_t bookId);
// Book ID for a file hash, or 0 when the book was not downloaded from Grimmory.
int64_t find(const std::string& documentHash);
// Main-progress percentage (0-100) last sent for this hash, or -1 if none.
float lastSentPercent(const std::string& documentHash);
// Remember the percentage just sent, so an unchanged position is not re-sent.
void setLastSentPercent(const std::string& documentHash, float percent);
}  // namespace GrimmoryBookIndex
