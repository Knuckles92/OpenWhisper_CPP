#include "ui_theme/icons.h"

#include <QHash>
#include <QSize>

namespace fv::ui {
namespace {

// Never destroyed: icons outliving QApplication must not be released after it.
QHash<QString, QIcon>& tablerCache() {
    static auto* cache = new QHash<QString, QIcon>();
    return *cache;
}

QHash<QString, QIcon>& designCache() {
    static auto* cache = new QHash<QString, QIcon>();
    return *cache;
}

} // namespace

QString tablerPath(const QString& file) {
    return QStringLiteral(":/ui_theme/assets/tabler/") + file;
}

QIcon tablerIcon(const QString& file) {
    QHash<QString, QIcon>& cache = tablerCache();
    const auto found = cache.constFind(file);
    if (found != cache.constEnd()) {
        return *found;
    }
    const QIcon icon(tablerPath(file));
    cache.insert(file, icon);
    return icon;
}

QPixmap tablerPixmap(const QString& file, int size) {
    return tablerIcon(file).pixmap(QSize(size, size));
}

QIcon designIcon(const QString& file) {
    QHash<QString, QIcon>& cache = designCache();
    const auto found = cache.constFind(file);
    if (found != cache.constEnd()) {
        return *found;
    }
    QIcon icon(tablerIcon(file));
    icon.addPixmap(icon.pixmap(24, 24), QIcon::Disabled, QIcon::Off);
    cache.insert(file, icon);
    return icon;
}

} // namespace fv::ui
