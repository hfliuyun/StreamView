#pragma once

#include <QObject>
#include <memory>

#include <QTranslator>

namespace streamview::app {

enum class Language {
    English,
    SimplifiedChinese,
};

class LocalizationManager final : public QObject {
    Q_OBJECT
public:
    static LocalizationManager& instance();

    [[nodiscard]] Language currentLanguage() const noexcept { return currentLanguage_; }
    void setLanguage(Language lang);

signals:
    void languageChanged(Language lang);

private:
    explicit LocalizationManager(QObject* parent = nullptr);

    Language currentLanguage_{Language::English};
    std::unique_ptr<QTranslator> chineseTranslator_;
};

} // namespace streamview::app
