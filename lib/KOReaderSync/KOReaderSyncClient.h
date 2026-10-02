#pragma once
#include <cstdint>
#include <optional>
#include <string>

/**
 * Optional document metadata sent alongside progress sync requests.
 * Mirrors the metadata object added in KOReader PR #15306.
 * The official sync server ignores this field; custom servers may use it.
 */
struct KOReaderMetadata {
  std::string filename;  // e.g. "my_book.epub"
  std::string title;     // Document title from EPUB metadata
  std::string authors;   // Author(s) from EPUB metadata
};

/**
 * Rich CrossPoint position sent alongside progress uploads. Maps 1:1 onto the
 * crosspoint-sync extended `position` object (see crosspoint-sync docs/API.md).
 * It is only transmitted to sync.crosspointreader.com, where it makes
 * CrossPoint<->CrossPoint sync lossless instead of xpath-approximated.
 */
struct KOReaderRichPosition {
  uint32_t pctQ = 0;                       // Percentage quantized 0..1,000,000 (authoritative)
  uint16_t spineIndex = 0;                 // Spine (chapter) index
  uint16_t pageNumber = 0;                 // Page within spine (layout-dependent hint)
  uint16_t totalPages = 1;                 // Spine page count (layout-dependent hint)
  std::optional<uint16_t> paragraphIndex;  // Synthetic 1-based paragraph index
  std::string xpath;                       // KOReader-style xpath (server cap: 120 bytes)
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                          // Document hash
  std::string progress;                          // XPath-like progress string
  float percentage;                              // Progress percentage (0.0 to 1.0)
  std::string device;                            // Device name
  std::string deviceId;                          // Device ID
  int64_t timestamp;                             // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;      // Optional document metadata
  std::optional<KOReaderRichPosition> position;  // Optional rich position (crosspoint-sync servers only)
};

/**
 * Explicit sync endpoint that replaces the KOReader credential store for the
 * current session (e.g. Grimmory's KOReader-compatible /api/koreader endpoint).
 */
struct KOReaderSyncEndpoint {
  std::string baseUrl;   // without trailing slash, e.g. "https://books.example/api/koreader"
  std::string username;  // x-auth-user
  std::string md5Key;    // x-auth-key (MD5 of the sync password)
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   GET /users/auth - Authenticate (validate credentials)
 *   GET /syncs/progress/:document - Get progress for a document
 *   PUT /syncs/progress - Update progress for a document
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    INVALID_AUTH_RESPONSE,
    LOW_MEMORY,
    USER_EXISTS
  };

  /**
   * Authenticate with the sync server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Register a new account on the sync server using the stored credentials
   * (POST /users/create with the MD5 auth key — the server never sees the
   * plain password).
   * @return OK on success, USER_EXISTS if the username is taken
   */
  static Error createUser();

  /**
   * Get reading progress for a document.
   * @param documentHash The document hash (from KOReaderDocumentId)
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress);

  /**
   * Get human-readable error message.
   */
  static std::string errorString(Error error);

  /**
   * Route requests to an explicit endpoint instead of the KOReader credential
   * store until clearEndpointOverride() is called.
   */
  static void setEndpointOverride(const KOReaderSyncEndpoint& endpoint);
  static void clearEndpointOverride();
  static bool hasEndpointOverride();

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /** Transport-layer error from the last request (for diagnostics). */
  static int lastTransportError;
};
