#include "GrimmoryConnectActivity.h"

#include <GfxRenderer.h>
#include <GrimmoryClient.h>
#include <GrimmoryStore.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

void GrimmoryConnectActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  WiFi.setSleep(false);
  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = CONNECTING;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("GRIM", "Connecting screen could not be rendered before request");
    requestUpdate(true);
  }

  performConnect();
}

void GrimmoryConnectActivity::performConnect() {
  GrimmoryClient::Error result = GrimmoryClient::login();
  if (result == GrimmoryClient::OK) result = GrimmoryClient::fetchSyncCredentials();
  GrimmoryClient::logout();
  LOG_INF("GRIM", "Connect result=%d http=%d", result, GrimmoryClient::lastHttpCode);

  {
    RenderLock lock(*this);
    if (result == GrimmoryClient::OK) {
      state = SUCCESS;
    } else {
      state = FAILED;
      errorMessage = GrimmoryClient::errorString(result);
    }
  }
  requestUpdate();
}

void GrimmoryConnectActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  if (!GRIMMORY_STORE.hasAccount()) {
    state = FAILED;
    errorMessage = tr(STR_GRIMMORY_NO_ACCOUNT);
    requestUpdate();
    return;
  }

  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GrimmoryConnectActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Launched from minimal network boot, so restore the full app state.
  silentRestart();
}

void GrimmoryConnectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  if ((state == SUCCESS || state == FAILED) && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_GRIMMORY), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_GRIMMORY));
  }
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;
  const Rect textArea{screen.x + metrics.contentSidePadding, screen.y, screen.width - metrics.contentSidePadding * 2,
                      screen.height};

  if (state == CONNECTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_GRIMMORY_CONNECTING));
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_GRIMMORY_CONNECTED), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(
        renderer, textArea, UI_10_FONT_ID, top + height + 10,
        GRIMMORY_STORE.getProgressSyncEnabled() ? tr(STR_GRIMMORY_SYNC_READY) : tr(STR_GRIMMORY_SYNC_OFF_HINT), 3, true,
        EpdFontFamily::REGULAR, 4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_GRIMMORY_CONNECT_FAILED), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top + height + 10, errorMessage.c_str(), 3,
                                     true, EpdFontFamily::REGULAR, 4);
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void GrimmoryConnectActivity::loop() {
  if (state != SUCCESS && state != FAILED) return;

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}
