#include "PasswordStore.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr uint8_t PASSWORDS_FILE_VERSION = 1;
}

PasswordStore PasswordStore::instance;

void PasswordStore::addEntry(const std::string& site, const std::string& username, const std::string& password) {
  if (entries.size() >= MAX_ENTRIES) {
    LOG_ERR("PWD", "Max entries reached (%zu)", MAX_ENTRIES);
    return;
  }

  // Check for duplicate site
  auto it = std::find_if(entries.begin(), entries.end(),
                         [&site](const PasswordEntry& e) { return e.site == site; });
  if (it != entries.end()) {
    // Update existing entry
    it->username = username;
    it->password = password;
  } else {
    // Add new entry
    entries.push_back({site, username, password});
  }

  if (!saveToFile()) {
    LOG_ERR("PWD", "Failed to save passwords to file");
  }
}

void PasswordStore::removeEntry(size_t index) {
  if (index >= entries.size()) {
    LOG_ERR("PWD", "Invalid entry index: %zu", index);
    return;
  }

  entries.erase(entries.begin() + index);

  if (!saveToFile()) {
    LOG_ERR("PWD", "Failed to save after removing entry");
  }
}

bool PasswordStore::loadFromFile() {
  FsFile file;
  if (!Storage.openFileForRead("PWD", PASSWORDS_FILE, file)) {
    LOG_DBG("PWD", "No password file found, starting fresh");
    return false;
  }

  entries.clear();

  uint8_t version;
  // readPod now returns void; errors are handled internally
  serialization::readPod(file, version);

  if (version != PASSWORDS_FILE_VERSION) {
    LOG_ERR("PWD", "Unsupported password file version: %u", version);
    return false;
  }

  uint8_t count;
  serialization::readPod(file, count);

  entries.reserve(count);

  for (uint8_t i = 0; i < count; i++) {
    std::string site, username, password;

  // Updated API: readString returns void and throws on error internally
  serialization::readString(file, site);
  serialization::readString(file, username);
  serialization::readString(file, password);

    entries.push_back({site, username, password});
  }

  LOG_DBG("PWD", "Loaded %zu password entries", entries.size());
  return true;
}

bool PasswordStore::saveToFile() const {
  Storage.mkdir("/biscuit");

  FsFile file;
  // openFileForWrite now takes three arguments; the boolean flag is removed
  if (!Storage.openFileForWrite("PWD", PASSWORDS_FILE, file)) {
    LOG_ERR("PWD", "Failed to open password file for writing");
    return false;
  }

  // Write version
  // writePod returns void; errors are handled internally
  serialization::writePod(file, PASSWORDS_FILE_VERSION);

  // Write entry count
  uint8_t count = static_cast<uint8_t>(entries.size());
  serialization::writePod(file, count);

  // Write entries
  for (const auto& entry : entries) {
    // Updated API: writeString returns void
    serialization::writeString(file, entry.site);
    serialization::writeString(file, entry.username);
    serialization::writeString(file, entry.password);
  }

  LOG_DBG("PWD", "Saved %zu password entries", entries.size());
  return true;
}
