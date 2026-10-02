#include "ProgressSyncService.h"

#include <GrimmoryStore.h>
#include <I18n.h>
#include <KOReaderSyncClient.h>

namespace ProgressSync {

bool usesGrimmory() { return GRIMMORY_STORE.isProgressSyncActive(); }

bool hasCredentials() { return usesGrimmory() || KOREADER_STORE.hasCredentials(); }

DocumentMatchMethod matchMethod() {
  return usesGrimmory() ? DocumentMatchMethod::BINARY : KOREADER_STORE.getMatchMethod();
}

void applyEndpoint() {
  if (!usesGrimmory()) {
    KOReaderSyncClient::clearEndpointOverride();
    return;
  }
  KOReaderSyncEndpoint endpoint;
  endpoint.baseUrl = GRIMMORY_STORE.getKoSyncBaseUrl();
  endpoint.username = GRIMMORY_STORE.getSyncUsername();
  endpoint.md5Key = GRIMMORY_STORE.getSyncKeyMd5();
  KOReaderSyncClient::setEndpointOverride(endpoint);
}

void clearEndpoint() { KOReaderSyncClient::clearEndpointOverride(); }

const char* title() { return usesGrimmory() ? tr(STR_GRIMMORY_SYNC) : tr(STR_KOREADER_SYNC); }

}  // namespace ProgressSync
