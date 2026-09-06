#include "theme_manager.h"

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QStyleHints>

namespace streamview::app {

ThemeManager& ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    if (auto* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] {
            if (mode_ == ThemeMode::System) {
                applyCurrentTheme();
            }
        });
    }
    applyCurrentTheme();
}

void ThemeManager::setThemeMode(ThemeMode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    applyCurrentTheme();
}

bool ThemeManager::isDark() const {
    if (mode_ == ThemeMode::Dark) {
        return true;
    }
    if (mode_ == ThemeMode::Light) {
        return false;
    }
    if (const auto* hints = QGuiApplication::styleHints()) {
        return hints->colorScheme() == Qt::ColorScheme::Dark;
    }
    return false;
}

QPalette ThemeManager::createLightPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0xf5, 0xf5, 0xf5));
    palette.setColor(QPalette::WindowText, QColor(0x1e, 0x1e, 0x1e));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor(0xf9, 0xf9, 0xf9));
    palette.setColor(QPalette::ToolTipBase, Qt::white);
    palette.setColor(QPalette::ToolTipText, QColor(0x1e, 0x1e, 0x1e));
    palette.setColor(QPalette::Text, QColor(0x1e, 0x1e, 0x1e));
    palette.setColor(QPalette::Button, QColor(0xe8, 0xe8, 0xe8));
    palette.setColor(QPalette::ButtonText, QColor(0x1e, 0x1e, 0x1e));
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, QColor(0x00, 0x66, 0xcc));
    palette.setColor(QPalette::Highlight, QColor(0x00, 0x78, 0xd4));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, QColor(0x8e, 0x8e, 0x8e));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9e, 0x9e, 0x9e));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9e, 0x9e, 0x9e));
    return palette;
}

QPalette ThemeManager::createDarkPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
    palette.setColor(QPalette::WindowText, QColor(0xd4, 0xd4, 0xd4));
    palette.setColor(QPalette::Base, QColor(0x25, 0x25, 0x26));
    palette.setColor(QPalette::AlternateBase, QColor(0x2d, 0x2d, 0x30));
    palette.setColor(QPalette::ToolTipBase, QColor(0x25, 0x25, 0x26));
    palette.setColor(QPalette::ToolTipText, QColor(0xd4, 0xd4, 0xd4));
    palette.setColor(QPalette::Text, QColor(0xd4, 0xd4, 0xd4));
    palette.setColor(QPalette::Button, QColor(0x2d, 0x2d, 0x30));
    palette.setColor(QPalette::ButtonText, QColor(0xd4, 0xd4, 0xd4));
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, QColor(0x37, 0x94, 0xff));
    palette.setColor(QPalette::Highlight, QColor(0x09, 0x47, 0x71));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, QColor(0x76, 0x76, 0x76));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6e, 0x6e, 0x6e));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6e, 0x6e, 0x6e));
    return palette;
}

void ThemeManager::applyCurrentTheme() {
    const bool dark = isDark();
    QApplication::setPalette(dark ? createDarkPalette() : createLightPalette());
    emit themeChanged(mode_, dark);
}

} // namespace streamview::app
