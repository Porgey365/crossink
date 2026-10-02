#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <GrimmoryClient.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "util/ButtonNavigator.h"

/**
 * Browse the Grimmory library (all books or one shelf, newest first) and
 * download books to the SD card. Launched from a minimal network boot like
 * the OPDS browser; leaving it restarts into the full app.
 */
class GrimmoryLibraryActivity final : public Activity {
 public:
  explicit GrimmoryLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR };

  struct Row {
    enum class Kind : uint8_t { ALL_BOOKS, SHELF, BOOK, PREV_PAGE, NEXT_PAGE };
    Kind kind = Kind::BOOK;
    std::string label;
    std::string subtitle;
    int64_t id = 0;
    // Index into `books` for BOOK rows.
    int bookIndex = -1;
  };

  using UiApp = freeink::ui::FreeInkApp<24, 4>;

  ButtonNavigator buttonNavigator;
  State state = State::CHECK_WIFI;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::vector<Row> rows;
  std::vector<GrimmoryClient::Book> books;
  bool loggedIn = false;
  // 0 = shelf list; otherwise browsing books (shelfId -1 = whole library).
  bool inBookList = false;
  int64_t shelfId = 0;
  std::string listTitle;
  int page = 0;
  int totalPages = 0;
  int rootSelectorIndex = 0;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelDownload = false;
  bool goHomeAfterCancel = false;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiApp::ScreenType& screen);
  void buildBrowsingScreen(UiApp::ScreenType& screen);
  void buildDownloadScreen(UiApp::ScreenType& screen);
  void buildStatusScreen(UiApp::ScreenType& screen);

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoading();
  bool ensureLoggedIn();
  void reload();
  void loadShelves();
  void loadBooks();
  void activateSelected();
  void navigateBack();
  void downloadBook(const GrimmoryClient::Book& book);
  std::string destinationPathFor(const GrimmoryClient::Book& book) const;
  void showError(GrimmoryClient::Error error);
};
