#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

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
