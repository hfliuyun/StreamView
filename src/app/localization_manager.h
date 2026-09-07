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

    /// Check whether a Chinese translation mapping exists for the given source text literal.
    [[nodiscard]] static bool hasChineseTranslation(std::string_view sourceText);

    /// Returns the Chinese translation for the given source text literal, or null QString if unmapped.
    [[nodiscard]] static QString translateToChinese(std::string_view sourceText);

    /// Returns the total number of keys in the Chinese translation dictionary.
    [[nodiscard]] static std::size_t chineseDictionarySize() noexcept;

signals:
    void languageChanged(Language lang);

private:
    explicit LocalizationManager(QObject* parent = nullptr);

    Language currentLanguage_{Language::English};
    std::unique_ptr<QTranslator> chineseTranslator_;
};

} // namespace streamview::app
