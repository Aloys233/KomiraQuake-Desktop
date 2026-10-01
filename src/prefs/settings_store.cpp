#include "prefs/settings_store.h"

namespace komira {

SettingsStore::SettingsStore(QObject* parent) : QObject(parent) {}

void SettingsStore::resetToDefaults() {
    s_.clear();
    emit changed();
}

} // namespace komira
