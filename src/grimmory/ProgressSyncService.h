#pragma once

#include "KOReaderCredentialStore.h"

/**
 * Chooses where "Sync Progress" goes: Grimmory (when connected and enabled in
 * Settings -> Grimmory) or the configured KOReader sync server. Every sync
 * entry point (reader menu, quick actions, frontlight drawer, network boot)
 * asks this instead of reading the KOReader store directly, so switching
 * services needs no change at each call site.
 */
namespace ProgressSync {

// True when progress sync should talk to Grimmory.
bool usesGrimmory();

// True when the active service has everything it needs to sync.
bool hasCredentials();

// Grimmory identifies books by KOReader's partial-MD5 file hash only.
DocumentMatchMethod matchMethod();

// Points KOReaderSyncClient at the active service. Call before network sync.
void applyEndpoint();

// Restores KOReaderSyncClient to the KOReader credential store.
void clearEndpoint();

// Screen title for the active service.
const char* title();

}  // namespace ProgressSync
