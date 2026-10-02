#include "GrimmoryLibraryActivity.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <GrimmoryStore.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_CANCEL = 2;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
constexpr int64_t ALL_BOOKS_SHELF = 0;

// Each page response is buffered before parsing. PSRAM-less boards keep pages
// small so the raw JSON (full book metadata) stays well inside free heap.
#if FREEINK_MCU_S3
constexpr int BOOKS_PER_PAGE = 20;
#else
constexpr int BOOKS_PER_PAGE = 5;
#endif

std::string formatPageLabel(const char* format, const int page, const int totalPages) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), format, page, totalPages);
  return buffer;
}
}  // namespace

GrimmoryLibraryActivity::GrimmoryLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GrimmoryLibrary", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void GrimmoryLibraryActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  state = State::CHECK_WIFI;
  rows.clear();
  books.clear();
  loggedIn = false;
  inBookList = false;
  shelfId = ALL_BOOKS_SHELF;
  page = 0;
  totalPages = 0;
  selectorIndex = 0;
  rootSelectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &GrimmoryLibraryActivity::onRowEvent, this);
  app.on(ACTION_CANCEL, &GrimmoryLibraryActivity::onCancelEvent, this);
  app.setScreen(&GrimmoryLibraryActivity::rootScreen, this);
  requestUpdate();

  if (!GRIMMORY_STORE.hasAccount()) {
    showError(GrimmoryClient::NO_ACCOUNT);
    return;
  }
  checkAndConnectWifi();
}

void GrimmoryLibraryActivity::onExit() {
  Activity::onExit();
  GrimmoryClient::logout();
  rows.clear();
  books.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Launched from minimal network boot, so restore the full app state even
  // if setup failed before WiFi was started.
  silentRestart();
}

void GrimmoryLibraryActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<GrimmoryLibraryActivity*>(user);
  if (self->state != State::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->rows.size())) return;
  self->selectorIndex = event.value;
  self->app.clearTapFlash();
  self->activateSelected();
}

void GrimmoryLibraryActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<GrimmoryLibraryActivity*>(user);
  if (self->state != State::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

bool GrimmoryLibraryActivity::preventAutoSleep() {
  return state == State::CHECK_WIFI || state == State::WIFI_SELECTION || state == State::LOADING ||
         state == State::DOWNLOADING;
}

void GrimmoryLibraryActivity::loop() {
  if (state == State::WIFI_SELECTION || state == State::DOWNLOADING) return;

  if (state == State::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        reload();
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) onGoHome();
    return;
  }

  // BROWSING
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    navigateBack();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    navigateBack();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
      if (state != State::BROWSING) return;
    }
  }

  if (rows.empty()) return;
  const int count = static_cast<int>(rows.size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, count);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, count](const int index) {
    selectorIndex = index;
    topIndex = followListSelection(selectorIndex, topIndex, visibleRows, count);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([&] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, count)); });
  buttonNavigator.onPreviousRelease([&] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, count)); });
  buttonNavigator.onNextContinuous(
      [&] { moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, count, visibleRows)); });
  buttonNavigator.onPreviousContinuous(
      [&] { moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, count, visibleRows)); });
}

void GrimmoryLibraryActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<GrimmoryLibraryActivity*>(user);
  switch (self->state) {
    case State::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case State::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

void GrimmoryLibraryActivity::screenHeader(UiApp::ScreenType& screen) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  const char* title = inBookList && !listTitle.empty() ? listTitle.c_str() : tr(STR_GRIMMORY_LIBRARY);
  if (state == State::BROWSING && mappedInput.hasTouchHardware()) {
    const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    TouchHeaderBackButton::draw(renderer, uiTarget, headerRect, title, false);
    screen.takeTop(static_cast<int16_t>(headerRect.height));
  } else {
    fui::HeaderProps header;
    header.title = title;
    header.borderEdges = fui::EdgeBottom;
    screen.header(header);
  }
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void GrimmoryLibraryActivity::buildBrowsingScreen(UiApp::ScreenType& screen) {
  screenHeader(screen);

  if (rows.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Transient per-render list; items point into `rows` strings.
  std::vector<fui::ListItem> items;
  items.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    fui::ListItem item;
    item.label = row.label.c_str();
    if (!row.subtitle.empty()) item.subtitle = row.subtitle.c_str();
    if (row.kind == Row::Kind::ALL_BOOKS || row.kind == Row::Kind::SHELF) item.value = ">";
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  const auto visible = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = visible > 0 ? visible : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(rows.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void GrimmoryLibraryActivity::buildDownloadScreen(UiApp::ScreenType& screen) {
  screenHeader(screen);

  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void GrimmoryLibraryActivity::buildStatusScreen(UiApp::ScreenType& screen) {
  screenHeader(screen);

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == State::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 3 : 2) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  screen.centeredText(statusMessage.c_str(), centered);
}

void GrimmoryLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING: {
      const bool isBook = selectorIndex >= 0 && selectorIndex < static_cast<int>(rows.size()) &&
                          rows[selectorIndex].kind == Row::Kind::BOOK;
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), isBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN),
                                     tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case State::ERROR:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  uiReady = false;
  app.render();
  uiReady = true;
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void GrimmoryLibraryActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    reload();
    return;
  }
  launchWifiSelection();
}

void GrimmoryLibraryActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GrimmoryLibraryActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }
  WiFi.setSleep(false);
  reload();
}

void GrimmoryLibraryActivity::showLoading() {
  uiReady = false;
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("GRIM", "Loading screen could not be rendered before request");
    requestUpdate(true);
  }
}

void GrimmoryLibraryActivity::showError(const GrimmoryClient::Error error) {
  uiReady = false;
  state = State::ERROR;
  errorMessage = GrimmoryClient::errorString(error);
  requestUpdate();
}

bool GrimmoryLibraryActivity::ensureLoggedIn() {
  if (loggedIn) return true;
  const auto result = GrimmoryClient::login();
  if (result != GrimmoryClient::OK) {
    showError(result);
    return false;
  }
  loggedIn = true;
  return true;
}

void GrimmoryLibraryActivity::reload() {
  showLoading();
  if (!ensureLoggedIn()) return;
  inBookList ? loadBooks() : loadShelves();
}

void GrimmoryLibraryActivity::loadShelves() {
  std::vector<GrimmoryClient::Shelf> shelves;
  auto result = GrimmoryClient::listShelves(shelves);
  if (result == GrimmoryClient::AUTH_FAILED) {
    // The session token may have expired while browsing; log in once more.
    loggedIn = false;
    if (!ensureLoggedIn()) return;
    result = GrimmoryClient::listShelves(shelves);
  }
  if (result != GrimmoryClient::OK) {
    showError(result);
    return;
  }

  uiReady = false;
  rows.clear();
  books.clear();
  rows.reserve(shelves.size() + 1);
  Row all;
  all.kind = Row::Kind::ALL_BOOKS;
  all.label = tr(STR_GRIMMORY_ALL_BOOKS);
  all.id = ALL_BOOKS_SHELF;
  rows.push_back(std::move(all));
  for (auto& shelf : shelves) {
    Row row;
    row.kind = Row::Kind::SHELF;
    row.label = std::move(shelf.name);
    row.id = shelf.id;
    rows.push_back(std::move(row));
  }
  selectorIndex = std::min(rootSelectorIndex, static_cast<int>(rows.size()) - 1);
  topIndex = 0;
  state = State::BROWSING;
  requestUpdate();
}

void GrimmoryLibraryActivity::loadBooks() {
  GrimmoryClient::BookPage result;
  auto error = GrimmoryClient::listBooks(shelfId, page, BOOKS_PER_PAGE, result);
  if (error == GrimmoryClient::AUTH_FAILED) {
    loggedIn = false;
    if (!ensureLoggedIn()) return;
    error = GrimmoryClient::listBooks(shelfId, page, BOOKS_PER_PAGE, result);
  }
  if (error != GrimmoryClient::OK) {
    showError(error);
    return;
  }

  uiReady = false;
  totalPages = result.totalPages;
  books = std::move(result.books);
  rows.clear();
  rows.reserve(books.size() + 2);
  if (page > 0) {
    Row prev;
    prev.kind = Row::Kind::PREV_PAGE;
    prev.label = mappedInput.resolveLabel(mappedInput.withPreviousPageArrow(tr(STR_PREV_PAGE)));
    prev.subtitle = formatPageLabel(tr(STR_GRIMMORY_PAGE_FORMAT), page + 1, totalPages);
    rows.push_back(std::move(prev));
  }
  for (size_t i = 0; i < books.size(); ++i) {
    Row row;
    row.kind = Row::Kind::BOOK;
    row.label = books[i].title;
    row.subtitle = books[i].authors;
    const std::string localPath = destinationPathFor(books[i]);
    if (Storage.exists(localPath.c_str())) {
      // Books downloaded before the ID index existed: record them now so
      // progress sync can update Grimmory's main progress for them too.
      const std::string documentHash = KOReaderDocumentId::calculate(localPath);
      if (!documentHash.empty() && GrimmoryBookIndex::find(documentHash) != books[i].id) {
        GrimmoryBookIndex::remember(documentHash, books[i].id);
      }
      row.subtitle =
          row.subtitle.empty() ? tr(STR_GRIMMORY_ON_DEVICE) : row.subtitle + " - " + tr(STR_GRIMMORY_ON_DEVICE);
    }
    row.id = books[i].id;
    row.bookIndex = static_cast<int>(i);
    rows.push_back(std::move(row));
  }
  if (page + 1 < totalPages) {
    Row next;
    next.kind = Row::Kind::NEXT_PAGE;
    next.label = mappedInput.resolveLabel(mappedInput.withNextPageArrow(tr(STR_NEXT_PAGE)));
    next.subtitle = formatPageLabel(tr(STR_GRIMMORY_PAGE_FORMAT), page + 1, totalPages);
    rows.push_back(std::move(next));
  }
  selectorIndex = 0;
  topIndex = 0;
  state = State::BROWSING;
  requestUpdate();
}

void GrimmoryLibraryActivity::activateSelected() {
  if (selectorIndex < 0 || selectorIndex >= static_cast<int>(rows.size())) return;
  const Row row = rows[selectorIndex];
  switch (row.kind) {
    case Row::Kind::ALL_BOOKS:
    case Row::Kind::SHELF:
      rootSelectorIndex = selectorIndex;
      inBookList = true;
      shelfId = row.id;
      listTitle = row.label;
      page = 0;
      reload();
      break;
    case Row::Kind::PREV_PAGE:
      if (page > 0) page--;
      reload();
      break;
    case Row::Kind::NEXT_PAGE:
      page++;
      reload();
      break;
    case Row::Kind::BOOK:
      if (row.bookIndex >= 0 && row.bookIndex < static_cast<int>(books.size())) downloadBook(books[row.bookIndex]);
      break;
  }
}

void GrimmoryLibraryActivity::navigateBack() {
  if (!inBookList) {
    onGoHome();
    return;
  }
  inBookList = false;
  listTitle.clear();
  page = 0;
  reload();
}

std::string GrimmoryLibraryActivity::destinationPathFor(const GrimmoryClient::Book& book) const {
  std::string base = book.authors.empty() ? book.title : book.title + " - " + book.authors;
  std::string path = GRIMMORY_STORE.getDownloadFolder();
  path += '/';
  path += StringUtils::sanitizeFilename(base);
  path += '.';
  path += book.extension;
  return path;
}

void GrimmoryLibraryActivity::downloadBook(const GrimmoryClient::Book& book) {
  uiReady = false;
  state = State::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = 0;
  downloadTotal = static_cast<size_t>(book.fileSizeKb) * 1024;
  cancelDownload = false;
  goHomeAfterCancel = false;
  requestUpdate(true);

  const std::string folder = GRIMMORY_STORE.getDownloadFolder();
  if (!Storage.exists(folder.c_str()) && !Storage.mkdir(folder.c_str())) {
    LOG_ERR("GRIM", "Could not create download folder %s", folder.c_str());
    showError(GrimmoryClient::FILE_ERROR);
    return;
  }

  const std::string destPath = destinationPathFor(book);
  LOG_INF("GRIM", "Downloading book %lld -> %s", static_cast<long long>(book.id), destPath.c_str());

  bool cancelRequested = false;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  // The activity loop is blocked for the whole download; pump input from the
  // transfer callbacks so the Cancel button or a Back press can abort it.
  auto pumpInput = [this, &cancelRequested] {
    mappedInput.update();
    if (mappedInput.wasHomeGesture()) {
      goHomeAfterCancel = true;
      cancelRequested = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested = true;
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) app.route(snap);
    }
    if (cancelDownload) cancelRequested = true;
  };

  auto download = [&]() {
    return GrimmoryClient::downloadBook(
        book.id, destPath,
        [&](const size_t downloaded, const size_t total) {
          downloadProgress = downloaded;
          if (total > 0) downloadTotal = total;
          pumpInput();
          const int percent =
              downloadTotal > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / downloadTotal) : 0;
          const unsigned long now = millis();
          if (percent >= 100 || lastRenderedPercent < 0 ||
              percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
              now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
            lastRenderedPercent = percent;
            lastProgressUpdateMs = now;
            requestUpdate(true);
          }
        },
        [&]() {
          if (!cancelRequested) pumpInput();
          return cancelRequested;
        });
  };

  auto result = download();
  if (result == GrimmoryClient::AUTH_FAILED) {
    loggedIn = false;
    if (!ensureLoggedIn()) return;
    result = download();
  }

  if (result == GrimmoryClient::OK) {
    // A re-downloaded book may differ from what an old cache was built from.
    clearBookCache(destPath);
    // Refresh the "on device" marker for this row.
    for (Row& row : rows) {
      if (row.kind == Row::Kind::BOOK && row.id == book.id) {
        row.subtitle =
            book.authors.empty() ? tr(STR_GRIMMORY_ON_DEVICE) : book.authors + " - " + tr(STR_GRIMMORY_ON_DEVICE);
      }
    }
    state = State::BROWSING;
  } else if (result == GrimmoryClient::CANCELLED) {
    LOG_INF("GRIM", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    mappedInput.suppressNextBackRelease();
    state = State::BROWSING;
  } else {
    showError(result);
    return;
  }
  requestUpdate();
}
