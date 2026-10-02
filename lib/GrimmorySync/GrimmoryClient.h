#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * Minimal client for the Grimmory REST API (https://github.com/grimmory-tools/grimmory),
 * modeled on the endpoints the official KOReader Grimmory plugin uses.
 *
 * Authentication: POST /api/v1/auth/login returns a JWT access token, sent as
 * "Authorization: Bearer <token>" on later calls. The token is kept in RAM only,
 * for the lifetime of one network session.
 *
 * Endpoints:
 *   POST  /api/v1/auth/login                     - log in with username/password
 *   GET   /api/v1/koreader-users/me              - KOReader-sync account for this user
 *   PATCH /api/v1/koreader-users/me/sync?enabled - turn KOReader sync on
 *   GET   /api/v1/shelves                        - the user's shelves
 *   GET   /api/v1/books/page                     - one page of books (optionally one shelf)
 *   GET   /api/v1/books/{id}/download            - the book file
 *
 * Progress itself goes through Grimmory's KOReader-compatible endpoint
 * (/api/koreader/syncs/progress) using KOReaderSyncClient.
 */
class GrimmoryClient {
 public:
  enum Error {
    OK = 0,
    NO_ACCOUNT,
    NETWORK_ERROR,
    AUTH_FAILED,
    FORBIDDEN,
    NO_KOREADER_ACCOUNT,
    SERVER_ERROR,
    JSON_ERROR,
    LOW_MEMORY,
    FILE_ERROR,
    CANCELLED,
  };

  struct Shelf {
    int64_t id = 0;
    std::string name;
    int bookCount = 0;
  };

  struct Book {
    int64_t id = 0;
    std::string title;
    std::string authors;
    std::string fileName;   // primary file name as stored on the server
    std::string extension;  // lowercase, without dot
    uint32_t fileSizeKb = 0;
  };

  struct BookPage {
    std::vector<Book> books;
    int pageNumber = 0;
    int totalPages = 0;
    long totalBooks = 0;
  };

  // Called with downloaded/total bytes (total may be 0 when unknown).
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  using CancelCallback = std::function<bool()>;

  /** Log in with the stored username/password. Required before the calls below. */
  static Error login();

  /**
   * Read the KOReader-sync account Grimmory keeps for this user, turn sync on
   * if it is off, and cache the username + MD5 key in GrimmoryStore.
   */
  static Error fetchSyncCredentials();

  static Error listShelves(std::vector<Shelf>& outShelves);

  /**
   * One page of books, newest first. shelfId 0 lists the whole library.
   * Only books whose primary file CrossInk can open are kept.
   */
  static Error listBooks(int64_t shelfId, int page, int pageSize, BookPage& outPage);

  /** Stream a book's primary file to destPath (written via a .part file). */
  static Error downloadBook(int64_t bookId, const std::string& destPath, const ProgressCallback& progress,
                            const CancelCallback& shouldCancel);

  /** Drop the in-memory access token (call when leaving the network session). */
  static void logout();

  static std::string errorString(Error error);

  /** True if CrossInk can open a file with this extension (lowercase, no dot). */
  static bool isSupportedExtension(const std::string& extension);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;
};
