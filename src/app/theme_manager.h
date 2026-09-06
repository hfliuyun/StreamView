#pragma once

#include <QObject>
#include <QPalette>

namespace streamview::app {

enum class ThemeMode {
    System,
    Light,
    Dark,
};

class ThemeManager final : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();

    [[nodiscard]] ThemeMode themeMode() const noexcept { return mode_; }
    void setThemeMode(ThemeMode mode);

    [[nodiscard]] bool isDark() const;

    [[nodiscard]] static QPalette createLightPalette();
    [[nodiscard]] static QPalette createDarkPalette();

signals:
    void themeChanged(ThemeMode mode, bool isDark);

private:
    explicit ThemeManager(QObject* parent = nullptr);
    void applyCurrentTheme();

    ThemeMode mode_{ThemeMode::System};
};

} // namespace streamview::app
