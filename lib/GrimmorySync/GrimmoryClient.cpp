#include "GrimmoryClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <SecureHttpClient.h>

#include <cctype>
#include <cstdio>
#include <string>

#include "GrimmoryStore.h"

int GrimmoryClient::lastHttpCode = 0;

namespace {
constexpr char USER_AGENT[] = "CrossInk-Grimmory";

// Same conservative TLS floors as KOReaderSyncClient: the wolfSSL handshake
// needs working heap, and a failed allocation mid-handshake aborts.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

// Keep the parsed book page small: a Grimmory Book carries full metadata
// (descriptions, ratings, progress...). The JSON filter below keeps only the
// fields shown in the library list, so a page fits comfortably in heap.
constexpr int MAX_PAGE_SIZE = 20;

// JWT for the current network session only; never written to the SD card.
std::string accessToken;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("GRIM", "Insufficient heap for TLS: %u free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, MIN_MAX_ALLOC_HEAP_FOR_TLS);
    return true;
  }
  return false;
}

std::string md5Hex(const std::string& text) {
  MD5Builder md5;
  md5.begin();
  md5.add(text.c_str());
  md5.calculate();
  return md5.toString().c_str();
}

std::string lowercase(std::string value) {
  for (char& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return value;
}

std::string extensionOf(const std::string& fileName) {
  const size_t dot = fileName.rfind('.');
  return dot == std::string::npos ? "" : lowercase(fileName.substr(dot + 1));
}

GrimmoryClient::Error errorForStatus(const int httpCode) {
  if (httpCode <= 0) return GrimmoryClient::NETWORK_ERROR;
  if (httpCode == 401) return GrimmoryClient::AUTH_FAILED;
  if (httpCode == 403) return GrimmoryClient::FORBIDDEN;
  return GrimmoryClient::SERVER_ERROR;
}

// Prepares a request to <base><path>. Adds the bearer token when logged in.
bool beginRequest(freeink::SecureHttpClient& http, const std::string& path, const bool withToken) {
  const std::string url = GRIMMORY_STORE.getBaseUrl() + path;
  http.setInsecure();
  http.setUserAgent(USER_AGENT);
  if (!http.begin(url)) {
    LOG_ERR("GRIM", "Bad URL: %s", url.c_str());
    return false;
  }
  http.addHeader("Accept", "application/json");
  if (withToken && !accessToken.empty()) http.addHeader("Authorization", "Bearer " + accessToken);
  return true;
}

bool parseJson(const std::string& body, JsonDocument& doc, const char* context, const JsonDocument* filter = nullptr) {
  const DeserializationError error =
      filter ? deserializeJson(doc, body, DeserializationOption::Filter(filter->as<JsonVariantConst>()))
             : deserializeJson(doc, body);
  if (error) {
    LOG_ERR("GRIM", "%s: JSON parse failed: %s (%u bytes)", context, error.c_str(), static_cast<unsigned>(body.size()));
    return false;
  }
  return true;
}
}  // namespace

bool GrimmoryClient::isSupportedExtension(const std::string& extension) {
  return extension == "epub" || extension == "txt" || extension == "md" || extension == "xtc" || extension == "xtch";
}

GrimmoryClient::Error GrimmoryClient::login() {
  lastHttpCode = 0;
  accessToken.clear();
  if (!GRIMMORY_STORE.hasAccount()) return NO_ACCOUNT;
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument request;
  request["username"] = GRIMMORY_STORE.getUsername();
  request["password"] = GRIMMORY_STORE.getPassword();
  std::string body;
  serializeJson(request, body);

  freeink::SecureHttpClient http;
  if (!beginRequest(http, "/api/v1/auth/login", false)) return NETWORK_ERROR;
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.POST(body);
  lastHttpCode = httpCode;
  LOG_DBG("GRIM", "Login response: %d", httpCode);

  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    // Grimmory answers bad credentials with 401 or 400 depending on version.
    if (httpCode == 400) return AUTH_FAILED;
    return errorForStatus(httpCode);
  }

  JsonDocument filter;
  filter["accessToken"] = true;
  JsonDocument doc;
  const bool parsed = parseJson(http.getString(), doc, "Login", &filter);
  http.end();
  if (!parsed) return JSON_ERROR;

  const char* token = doc["accessToken"] | "";
  if (token[0] == '\0') {
    LOG_ERR("GRIM", "Login response had no access token");
    return JSON_ERROR;
  }
  accessToken = token;
  return OK;
}

void GrimmoryClient::logout() {
  accessToken.clear();
  accessToken.shrink_to_fit();
}

GrimmoryClient::Error GrimmoryClient::fetchSyncCredentials() {
  lastHttpCode = 0;
  if (accessToken.empty()) return AUTH_FAILED;
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  if (!beginRequest(http, "/api/v1/koreader-users/me", true)) return NETWORK_ERROR;
  int httpCode = http.GET();
  lastHttpCode = httpCode;
  LOG_DBG("GRIM", "KOReader account response: %d", httpCode);

  if (httpCode == 404) {
    // Grimmory only creates this account when the user sets up KOReader sync
    // in its web UI. Creating it from the device would invent a password the
    // user never chose, so ask them to do it there instead.
    http.end();
    return NO_KOREADER_ACCOUNT;
  }
  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    return errorForStatus(httpCode);
  }

  JsonDocument filter;
  filter["username"] = true;
  filter["password"] = true;
  filter["passwordMD5"] = true;
  filter["syncEnabled"] = true;
  JsonDocument doc;
  const bool parsed = parseJson(http.getString(), doc, "KOReader account", &filter);
  if (!parsed) {
    http.end();
    return JSON_ERROR;
  }

  const std::string syncUser = doc["username"] | "";
  std::string syncKey = doc["passwordMD5"] | "";
  if (syncKey.empty()) {
    const std::string plain = doc["password"] | "";
    if (!plain.empty()) syncKey = md5Hex(plain);
  }
  const bool syncEnabled = doc["syncEnabled"] | false;
  if (syncUser.empty() || syncKey.empty()) {
    http.end();
    LOG_ERR("GRIM", "KOReader account response had no username or key");
    return NO_KOREADER_ACCOUNT;
  }

  if (!syncEnabled) {
    // The KOReader plugin turns sync on the same way; without it Grimmory
    // rejects every progress request for this account.
    LOG_INF("GRIM", "Enabling KOReader sync on the Grimmory account");
    if (!beginRequest(http, "/api/v1/koreader-users/me/sync?enabled=true", true)) return NETWORK_ERROR;
    httpCode = http.sendRequest("PATCH", std::string());
    lastHttpCode = httpCode;
    LOG_DBG("GRIM", "Enable sync response: %d", httpCode);
    if (httpCode < 200 || httpCode >= 300) {
      http.end();
      return errorForStatus(httpCode);
    }
  }
  http.end();

  GRIMMORY_STORE.setSyncCredentials(syncUser, lowercase(syncKey));
  if (!GRIMMORY_STORE.saveToFile()) {
    LOG_ERR("GRIM", "Failed to save Grimmory sync credentials");
    return FILE_ERROR;
  }
  return OK;
}

GrimmoryClient::Error GrimmoryClient::listShelves(std::vector<Shelf>& outShelves) {
  lastHttpCode = 0;
  outShelves.clear();
  if (accessToken.empty()) return AUTH_FAILED;
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  if (!beginRequest(http, "/api/v1/shelves", true)) return NETWORK_ERROR;
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  LOG_DBG("GRIM", "Shelves response: %d", httpCode);
  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    return errorForStatus(httpCode);
  }

  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["name"] = true;
  filter[0]["bookCount"] = true;
  JsonDocument doc;
  const bool parsed = parseJson(http.getString(), doc, "Shelves", &filter);
  http.end();
  if (!parsed) return JSON_ERROR;

  const JsonArrayConst shelves = doc.as<JsonArrayConst>();
  outShelves.reserve(shelves.size());
  for (JsonObjectConst item : shelves) {
    Shelf shelf;
    shelf.id = item["id"] | static_cast<int64_t>(0);
    shelf.name = item["name"] | "";
    shelf.bookCount = item["bookCount"] | 0;
    if (shelf.id > 0) outShelves.push_back(std::move(shelf));
  }
  return OK;
}

GrimmoryClient::Error GrimmoryClient::listBooks(const int64_t shelfId, const int page, const int pageSize,
                                                BookPage& outPage) {
  lastHttpCode = 0;
  outPage = BookPage{};
  if (accessToken.empty()) return AUTH_FAILED;
  if (insufficientHeap()) return LOW_MEMORY;

  const int size = pageSize < 1 ? 1 : (pageSize > MAX_PAGE_SIZE ? MAX_PAGE_SIZE : pageSize);
  char path[160];
  if (shelfId > 0) {
    snprintf(path, sizeof(path), "/api/v1/books/page?sort=-addedOn&page=%d&size=%d&facet=shelf%%3A%lld", page, size,
             static_cast<long long>(shelfId));
  } else {
    snprintf(path, sizeof(path), "/api/v1/books/page?sort=-addedOn&page=%d&size=%d", page, size);
  }

  freeink::SecureHttpClient http;
  if (!beginRequest(http, path, true)) return NETWORK_ERROR;
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  LOG_DBG("GRIM", "Books page %d response: %d (%d bytes)", page, httpCode, http.getSize());
  if (httpCode < 200 || httpCode >= 300) {
    http.end();
    return errorForStatus(httpCode);
  }

  JsonDocument filter;
  filter["content"][0]["id"] = true;
  filter["content"][0]["title"] = true;
  filter["content"][0]["metadata"]["title"] = true;
  filter["content"][0]["metadata"]["authors"] = true;
  filter["content"][0]["primaryFile"]["fileName"] = true;
  filter["content"][0]["primaryFile"]["extension"] = true;
  filter["content"][0]["primaryFile"]["fileSizeKb"] = true;
  filter["page"]["number"] = true;
  filter["page"]["totalPages"] = true;
  filter["page"]["totalElements"] = true;
  JsonDocument doc;
  const bool parsed = parseJson(http.getString(), doc, "Books", &filter);
  http.end();
  if (!parsed) return JSON_ERROR;

  outPage.pageNumber = doc["page"]["number"] | page;
  outPage.totalPages = doc["page"]["totalPages"] | 0;
  outPage.totalBooks = doc["page"]["totalElements"] | 0L;

  const JsonArrayConst content = doc["content"].as<JsonArrayConst>();
  outPage.books.reserve(content.size());
  for (JsonObjectConst item : content) {
    const JsonObjectConst file = item["primaryFile"].as<JsonObjectConst>();
    if (file.isNull()) continue;

    Book book;
    book.id = item["id"] | static_cast<int64_t>(0);
    book.fileName = file["fileName"] | "";
    book.extension = lowercase(file["extension"] | "");
    if (book.extension.empty()) book.extension = extensionOf(book.fileName);
    if (book.id <= 0 || !isSupportedExtension(book.extension)) continue;

    book.fileSizeKb = file["fileSizeKb"] | 0u;
    book.title = item["metadata"]["title"] | "";
    if (book.title.empty()) book.title = item["title"] | "";
    if (book.title.empty()) book.title = book.fileName;
    for (JsonVariantConst author : item["metadata"]["authors"].as<JsonArrayConst>()) {
      const char* name = author | "";
      if (name[0] == '\0') continue;
      if (!book.authors.empty()) book.authors += ", ";
      book.authors += name;
    }
    outPage.books.push_back(std::move(book));
  }
  return OK;
}

GrimmoryClient::Error GrimmoryClient::downloadBook(const int64_t bookId, const std::string& destPath,
                                                   const ProgressCallback& progress,
                                                   const CancelCallback& shouldCancel) {
  lastHttpCode = 0;
  if (accessToken.empty()) return AUTH_FAILED;
  if (insufficientHeap()) return LOW_MEMORY;

  // Write to a .part file and rename on success, so an interrupted download
  // never leaves a truncated book that looks complete in the file browser.
  const std::string partPath = destPath + ".part";
  if (Storage.exists(partPath.c_str())) Storage.remove(partPath.c_str());

  HalFile file;
  if (!Storage.openFileForWrite("GRIM", partPath, file)) {
    LOG_ERR("GRIM", "Failed to open %s for writing", partPath.c_str());
    return FILE_ERROR;
  }

  char path[64];
  snprintf(path, sizeof(path), "/api/v1/books/%lld/download", static_cast<long long>(bookId));
  freeink::SecureHttpClient http;
  if (!beginRequest(http, path, true)) {
    file.close();
    Storage.remove(partPath.c_str());
    return NETWORK_ERROR;
  }
  http.setFollowRedirects(3);

  bool writeFailed = false;
  bool cancelled = false;
  if (progress) {
    http.setProgressCallback([&progress](const size_t downloaded, const size_t total) {
      progress(downloaded, total);
      return true;
    });
  }
  const int httpCode = http.GET(
      [&](const uint8_t* data, const size_t len) {
        // Only keep a successful response body; an error page is not a book.
        if (http.getStatus() < 200 || http.getStatus() >= 300) return true;
        if (file.write(data, len) != len) {
          writeFailed = true;
          return false;
        }
        return true;
      },
      [&]() {
        if (shouldCancel && shouldCancel()) cancelled = true;
        return cancelled;
      });
  lastHttpCode = httpCode;
  const bool complete = http.responseComplete();
  http.end();
  file.flush();
  file.close();
  LOG_DBG("GRIM", "Download %lld response: %d complete=%d", static_cast<long long>(bookId), httpCode, complete);

  Error result = OK;
  if (cancelled) {
    result = CANCELLED;
  } else if (writeFailed) {
    result = FILE_ERROR;
  } else if (httpCode < 200 || httpCode >= 300) {
    result = errorForStatus(httpCode);
  } else if (!complete) {
    result = NETWORK_ERROR;
  }

  if (result != OK) {
    Storage.remove(partPath.c_str());
    return result;
  }

  if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  if (!Storage.rename(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("GRIM", "Failed to rename %s to %s", partPath.c_str(), destPath.c_str());
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  return OK;
}

std::string GrimmoryClient::errorString(const Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_ACCOUNT:
      return tr(STR_GRIMMORY_NO_ACCOUNT);
    case NETWORK_ERROR:
      return tr(STR_GRIMMORY_NETWORK_ERROR);
    case AUTH_FAILED:
      return tr(STR_GRIMMORY_AUTH_FAILED);
    case FORBIDDEN:
      return tr(STR_GRIMMORY_FORBIDDEN);
    case NO_KOREADER_ACCOUNT:
      return tr(STR_GRIMMORY_NO_KOREADER_ACCOUNT);
    case SERVER_ERROR: {
      char buffer[64];
      snprintf(buffer, sizeof(buffer), tr(STR_GRIMMORY_SERVER_ERROR_FORMAT), lastHttpCode);
      return buffer;
    }
    case JSON_ERROR:
      return tr(STR_GRIMMORY_BAD_RESPONSE);
    case LOW_MEMORY:
      return tr(STR_KOREADER_SYNC_LOW_MEMORY);
    case FILE_ERROR:
      return tr(STR_GRIMMORY_FILE_ERROR);
    case CANCELLED:
      return tr(STR_CANCEL);
  }
  return tr(STR_GRIMMORY_BAD_RESPONSE);
}
