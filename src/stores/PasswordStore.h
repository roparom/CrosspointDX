#pragma once
#include <string>
#include <vector>

struct PasswordEntry {
  std::string site;
  std::string username;
  std::string password;
};

class PasswordStore {
 private:
  static PasswordStore instance;
  std::vector<PasswordEntry> entries;

  static constexpr const char* PASSWORDS_FILE = "/biscuit/passwords.bin";
  static constexpr size_t MAX_ENTRIES = 64;

  PasswordStore() = default;

 public:
  PasswordStore(const PasswordStore&) = delete;
  PasswordStore& operator=(const PasswordStore&) = delete;

  static PasswordStore& getInstance() { return instance; }

  // Add a new password entry
  void addEntry(const std::string& site, const std::string& username, const std::string& password);

  // Remove entry by index
  void removeEntry(size_t index);

  // Load passwords from file
  bool loadFromFile();

  // Save passwords to file
  bool saveToFile() const;

  // Get all entries
  const std::vector<PasswordEntry>& getEntries() const { return entries; }

  // Get number of entries
  size_t size() const { return entries.size(); }

  // Clear all entries
  void clear() { entries.clear(); }
};

// Singleton macro for convenience
#define PASSWORD_STORE PasswordStore::getInstance()
