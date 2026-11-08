#pragma once

namespace i18n {

enum class Lang { ID, EN };

Lang& current();

inline bool isEn() { return current() == Lang::EN; }

inline const char* tr(const char* id, const char* en) { return isEn() ? en : id; }

}  // namespace i18n
