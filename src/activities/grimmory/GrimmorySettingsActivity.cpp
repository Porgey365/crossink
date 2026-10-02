#include "GrimmorySettingsActivity.h"

#include <GfxRenderer.h>
#include <GrimmoryStore.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem : int {
  ITEM_SERVER_URL = 0,
  ITEM_USERNAME,
  ITEM_PASSWORD,
  ITEM_CONNECT,
  ITEM_PROGRESS_SYNC,
  ITEM_LIBRARY,
  MENU_ITEMS,
};
const StrId menuNames[MENU_ITEMS] = {
    StrId::STR_GRIMMORY_SERVER_URL,    StrId::STR_USERNAME,        StrId::STR_PASSWORD, StrId::STR_GRIMMORY_CONNECT,
    StrId::STR_GRIMMORY_PROGRESS_SYNC, StrId::STR_GRIMMORY_LIBRARY};
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

GrimmorySettingsActivity::GrimmorySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GrimmorySettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void GrimmorySettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<GrimmorySettingsActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEMS) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
}

void GrimmorySettingsActivity::onEnter() {
  Activity::onEnter();

  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &GrimmorySettingsActivity::onRowEvent, this);
  app.setScreen(&GrimmorySettingsActivity::listScreen, this);
  GRIMMORY_STORE.ensureLoaded();
  requestUpdate();
}

void GrimmorySettingsActivity::onExit() { Activity::onExit(); }

void GrimmorySettingsActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });
}

void GrimmorySettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case ITEM_SERVER_URL: {
      // Prefill with https:// if empty to save typing.
      const std::string currentUrl = GRIMMORY_STORE.getServerUrl();
      const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GRIMMORY_SERVER_URL),
                                                                     prefillUrl, 128, InputType::Url),
                             [](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               GRIMMORY_STORE.setServerUrl(url);
                               GRIMMORY_STORE.saveToFile();
                             });
      break;
    }
    case ITEM_USERNAME:
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME),
                                                                     GRIMMORY_STORE.getUsername(), 64, InputType::Text),
                             [](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               GRIMMORY_STORE.setUsername(std::get<KeyboardResult>(result.data).text);
                               GRIMMORY_STORE.saveToFile();
                             });
      break;
    case ITEM_PASSWORD:
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD),
                                                                     GRIMMORY_STORE.getPassword(), 64, InputType::Text),
                             [](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               GRIMMORY_STORE.setPassword(std::get<KeyboardResult>(result.data).text);
                               GRIMMORY_STORE.saveToFile();
                             });
      break;
    case ITEM_CONNECT:
      if (!GRIMMORY_STORE.hasAccount()) return;
      silentRestartToNetwork(NetworkBootTarget::GRIMMORY_CONNECT);
      break;
    case ITEM_PROGRESS_SYNC:
      GRIMMORY_STORE.setProgressSyncEnabled(!GRIMMORY_STORE.getProgressSyncEnabled());
      GRIMMORY_STORE.saveToFile();
      requestUpdate();
      break;
    case ITEM_LIBRARY:
      if (!GRIMMORY_STORE.hasAccount()) return;
      silentRestartToNetwork(NetworkBootTarget::GRIMMORY_LIBRARY);
      break;
    default:
      break;
  }
}

void GrimmorySettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<GrimmorySettingsActivity*>(user)->buildListScreen(screen);
}

void GrimmorySettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool hasAccount = GRIMMORY_STORE.hasAccount();
  const std::string needAccount = std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";

  // Per-render owned value strings; items point into them for the draw only.
  std::vector<std::string> values(MENU_ITEMS);
  values[ITEM_SERVER_URL] = GRIMMORY_STORE.getServerUrl().empty() ? tr(STR_NOT_SET) : GRIMMORY_STORE.getServerUrl();
  values[ITEM_USERNAME] = GRIMMORY_STORE.getUsername().empty() ? tr(STR_NOT_SET) : GRIMMORY_STORE.getUsername();
  values[ITEM_PASSWORD] = GRIMMORY_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
  if (!hasAccount) {
    values[ITEM_CONNECT] = needAccount;
  } else {
    values[ITEM_CONNECT] =
        GRIMMORY_STORE.hasSyncCredentials() ? tr(STR_GRIMMORY_CONNECTED) : tr(STR_GRIMMORY_NOT_CONNECTED);
  }
  values[ITEM_LIBRARY] = hasAccount ? "" : needAccount;

  std::vector<fui::ListItem> items;
  items.reserve(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuNames[i]);
    if (!values[i].empty()) item.value = values[i].c_str();
    item.toggle = i == ITEM_PROGRESS_SYNC;
    item.toggleChecked = GRIMMORY_STORE.getProgressSyncEnabled();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, MENU_ITEMS);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void GrimmorySettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_GRIMMORY), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_GRIMMORY));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
