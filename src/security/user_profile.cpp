#include "user_profile.h"

#include "src/platform/mros_nvs.h"

namespace mros::profile {
namespace {

using mros::platform::NvsNamespace;
using mros::platform::NvsPartitionMode;

constexpr const char* kNamespace = "user_profile";
constexpr const char* kLocaleKey = "locale";

bool is_supported_locale(const String& value) {
  return value == kLocaleTurkish || value == kLocaleEnglish;
}

bool open_profile_nvs(NvsNamespace* pref, const bool read_only) {
  return pref != nullptr &&
         pref->open(kNamespace, read_only, NvsPartitionMode::UserPartitionsThenDefault);
}

}  // namespace

String locale() {
  NvsNamespace pref;
  std::string value;
  if (open_profile_nvs(&pref, true) && pref.get_string(kLocaleKey, &value)) {
    String out(value.c_str());
    if (is_supported_locale(out)) {
      return out;
    }
  }
  return kLocaleTurkish;
}

bool set_locale(const String& locale_value) {
  String clean = locale_value;
  clean.trim();
  if (!is_supported_locale(clean)) {
    return false;
  }
  NvsNamespace pref;
  if (!open_profile_nvs(&pref, false)) {
    return false;
  }
  return pref.set_string(kLocaleKey, std::string(clean.c_str(), clean.length()));
}

bool is_turkish_locale() { return locale() == kLocaleTurkish; }

String display_locale_name(const String& locale_value) {
  if (locale_value == kLocaleEnglish) {
    return "English (United States)";
  }
  return "Turkce (Turkiye)";
}

}  // namespace mros::profile
