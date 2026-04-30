#include "core/i18n.hpp"

namespace macman {

I18n& I18n::instance() {
    static I18n inst;
    return inst;
}

I18n::I18n() {
    load_translations();
}

void I18n::set_language(Language lang) {
    current_lang_ = lang;
}

std::string I18n::get(const std::string& id) const {
    auto it_lang = translations_.find(current_lang_);
    if (it_lang != translations_.end()) {
        auto it_id = it_lang->second.find(id);
        if (it_id != it_lang->second.end()) {
            return it_id->second;
        }
    }
    
    // Fallback to English
    if (current_lang_ != Language::EN) {
        auto it_en = translations_.find(Language::EN);
        if (it_en != translations_.end()) {
            auto it_id = it_en->second.find(id);
            if (it_id != it_en->second.end()) {
                return it_id->second;
            }
        }
    }

    return id; // Return the ID itself if no translation found
}

void I18n::load_translations() {
    // English
    translations_[Language::EN] = {
        {"error_root", "you cannot perform this operation unless you are root."},
        {"error_no_targets", "no targets specified (use -h for help)"},
        {"action_resolving", "Resolving packages concurrently..."},
        {"warning_no_install", "No packages to install (or failed to resolve)."},
        {"status_installing", "Installing"},
        {"success_installed", "installed successfully"},
        {"error_not_found", "target not found"},
        {"doctor_running", "Macman Doctor: Running system health check..."},
        {"confirm_proceed", "Proceed with installation? [Y/n] "}
    };

    // Turkish
    translations_[Language::TR] = {
        {"error_root", "bu işlemi gerçekleştirmek için yetkili kullanıcı (root) olmanız gerekir."},
        {"error_no_targets", "herhangi bir hedef belirtilmedi (yardım için -h kullanın)"},
        {"action_resolving", "Paket bağımlılıkları çözümleniyor..."},
        {"warning_no_install", "Yüklenecek paket bulunamadı (veya çözümleme başarısız)."},
        {"status_installing", "Yükleniyor"},
        {"success_installed", "başarıyla yüklendi"},
        {"error_not_found", "hedef bulunamadı"},
        {"doctor_running", "Macman Doctor: Sistem sağlık kontrolü yapılıyor..."},
        {"confirm_proceed", "Yükleme işlemine devam edilsin mi? [E/h] "}
    };
}

} // namespace macman
