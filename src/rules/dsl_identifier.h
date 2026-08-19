#pragma once

#include <QStringView>

namespace streamview::rules::detail {

[[nodiscard]] inline bool isDslIdentifier(QStringView text) noexcept {
    if (text.isEmpty() || text.size() > 64) {
        return false;
    }
    const QChar first = text.at(0);
    if (first != QLatin1Char('_') &&
        !(first >= QLatin1Char('a') && first <= QLatin1Char('z')) &&
        !(first >= QLatin1Char('A') && first <= QLatin1Char('Z'))) {
        return false;
    }
    for (qsizetype i = 1; i < text.size(); ++i) {
        const QChar character = text.at(i);
        if (character != QLatin1Char('_') &&
            !(character >= QLatin1Char('a') && character <= QLatin1Char('z')) &&
            !(character >= QLatin1Char('A') && character <= QLatin1Char('Z')) &&
            !(character >= QLatin1Char('0') && character <= QLatin1Char('9'))) {
            return false;
        }
    }
    return true;
}

} // namespace streamview::rules::detail
