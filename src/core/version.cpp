#include <streamview/core/version.h>

namespace streamview::core {

QString version() {
    return QStringLiteral(STREAMVIEW_VERSION);
}

} // namespace streamview::core
