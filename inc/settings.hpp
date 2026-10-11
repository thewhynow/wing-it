#pragma once
// User settings: Hack Club AI API key + model. Stored in ~/.wing-it/settings.json (outside the repo, mode 0600),
// HACKCLUB_API_KEY in the environment is the fallback when no key was saved. Thread-safe.
#include <string>

constexpr const char* kDefaultAiModel = "anthropic/claude-sonnet-5.5";
constexpr const char* kAiBaseUrl = "https://ai.hackclub.com/proxy/v1";

void settingsLoad();                                        // call once at startup
bool settingsSave(const std::string& apiKey, const std::string& model, std::string& err);
std::string settingsApiKey();                               // saved key, else $HACKCLUB_API_KEY, else ""
std::string settingsModel();                                // never empty
bool settingsKeyFromEnv();                                  // true when the key in use comes from the environment
std::string settingsMaskedKey();                            // "sk-hc-v1-…ab12" for display, "" if none
