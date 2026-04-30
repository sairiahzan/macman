// Arda Yiğit - Hazani
// i18n.hpp — Internationalization Manager
// Provides a centralized way to manage translated strings.
// Supports switching languages at runtime (EN, TR).

#pragma once

#include <string>
#include <map>
#include <vector>

namespace macman {

enum class Language {
    EN,
    TR
};

class I18n {
public:
    static I18n& instance();

    void set_language(Language lang);
    Language get_language() const { return current_lang_; }

    // Get a translated string by its ID
    std::string get(const std::string& id) const;

private:
    I18n();
    
    Language current_lang_ = Language::EN;
    std::map<Language, std::map<std::string, std::string>> translations_;

    void load_translations();
};

// Convenience macro for translating
#define _T(id) macman::I18n::instance().get(id)

} // namespace macman
