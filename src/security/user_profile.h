#pragma once

#include "WString.h"

namespace mros::profile {

constexpr const char* kLocaleTurkish = "tr_TR.utf8";
constexpr const char* kLocaleEnglish = "en_US.utf8";

String locale();
bool set_locale(const String& locale);
bool is_turkish_locale();
String display_locale_name(const String& locale);

}  // namespace mros::profile
