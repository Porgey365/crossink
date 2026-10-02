#pragma once

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

/**
 * Connects to WiFi, logs in to Grimmory and caches the KOReader-sync
 * credentials Grimmory keeps for the account, so progress sync can use
 * Grimmory's KOReader-compatible endpoint. Launched from a minimal network
 * boot; leaving it restarts into the full app.
 */
class GrimmoryConnectActivity final : public Activity {
 public:
  explicit GrimmoryConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GrimmoryConnect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::string errorMessage;

  void onWifiSelectionComplete(bool success);
  void performConnect();
};
