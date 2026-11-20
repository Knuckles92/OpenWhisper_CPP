#include "ui_settings/settings_binder.h"

#include <algorithm>
#include <utility>

#include <QAbstractButton>
#include <QComboBox>
#include <QEvent>
#include <QLineEdit>
#include <QObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSpinBox>
#include <QTextDocument>

#include "appkit/qt_bridge.h"
#include "pyport/json.h"

namespace fv {
namespace {

// Python's bool(value) for a JSON value.
bool truthy(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<long long>() != 0;
    }
    if (value.is_number()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    if (value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return false;
}

QString text(const Json& value) {
    if (value.is_string()) {
        return qt::toQString(value.get_ref<const std::string&>());
    }
    if (value.is_null()) {
        return {};
    }
    return qt::toQString(py::dumps(value));
}

// Calls save whenever its editor, which owns it, loses focus (_FocusOutSaver).
class FocusOutSaver final : public QObject {
public:
    FocusOutSaver(QPlainTextEdit* edit, std::function<void()> save)
        : QObject(edit), save_(std::move(save)) {
        edit->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::FocusOut) {
            save_();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> save_;
};

} // namespace

QVariant jsonToVariant(const Json& value) {
    if (value.is_null()) {
        return {};
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_unsigned()) {
        return QVariant::fromValue(value.get<qulonglong>());
    }
    if (value.is_number_integer()) {
        return QVariant::fromValue(value.get<qlonglong>());
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return qt::toQString(value.get_ref<const std::string&>());
    }
    return qt::toQString(py::dumps(value));
}

Json variantToJson(const QVariant& value) {
    if (!value.isValid() || value.isNull()) {
        return nullptr;
    }
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::Short:
    case QMetaType::Long:
    case QMetaType::LongLong:
        return value.toLongLong();
    case QMetaType::UInt:
    case QMetaType::UShort:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
        return value.toULongLong();
    case QMetaType::Double:
    case QMetaType::Float:
        return value.toDouble();
    default:
        return qt::toUtf8(value.toString());
    }
}

SettingsBinder::SettingsBinder(Persist persist) : persist_(std::move(persist)) {}

void SettingsBinder::checkbox(QAbstractButton* check, std::string key, Resolve resolve) {
    QObject::connect(check, &QAbstractButton::toggled, check,
                     [persist = persist_, key](bool checked) { persist(key, Json(checked)); });
    QPointer<QAbstractButton> guarded(check);
    bindings_.push_back({std::move(resolve), [guarded](const Json& value) {
                             if (guarded) {
                                 guarded->setChecked(truthy(value));
                             }
                         }});
}

void SettingsBinder::combo(QComboBox* combo, std::string key, Resolve resolve) {
    QPointer<QComboBox> guarded(combo);
    QObject::connect(combo, &QComboBox::currentIndexChanged, combo,
                     [persist = persist_, key, guarded](int) {
                         persist(key, variantToJson(guarded->currentData()));
                     });
    bindings_.push_back({std::move(resolve), [guarded](const Json& value) {
                             if (guarded) {
                                 guarded->setCurrentIndex(
                                     std::max(0, guarded->findData(jsonToVariant(value))));
                             }
                         }});
}

void SettingsBinder::spin(QSpinBox* spin, std::string key, Resolve resolve) {
    QObject::connect(spin, &QSpinBox::valueChanged, spin,
                     [persist = persist_, key](int value) { persist(key, Json(value)); });
    QPointer<QSpinBox> guarded(spin);
    bindings_.push_back({std::move(resolve), [guarded](const Json& value) {
                             if (!guarded || !value.is_number()) {
                                 return;
                             }
                             guarded->setValue(value.is_number_float()
                                                   ? static_cast<int>(value.get<double>())
                                                   : static_cast<int>(value.get<long long>()));
                         }});
}

void SettingsBinder::lineEdit(QLineEdit* edit, std::string key, Resolve resolve) {
    QPointer<QLineEdit> guarded(edit);
    QObject::connect(edit, &QLineEdit::editingFinished, edit, [persist = persist_, key, guarded] {
        persist(key, Json(qt::toUtf8(guarded->text().trimmed())));
    });
    bindings_.push_back({std::move(resolve), [guarded](const Json& value) {
                             if (guarded) {
                                 guarded->setText(text(value));
                             }
                         }});
}

void SettingsBinder::plainText(QPlainTextEdit* edit, std::string key, Resolve resolve) {
    QPointer<QPlainTextEdit> guarded(edit);
    auto save = [persist = persist_, key, guarded] {
        QTextDocument* document = guarded->document();
        if (!document->isModified()) {
            return;
        }
        document->setModified(false);
        const QString typed = guarded->toPlainText().trimmed();
        persist(key, Json(typed == guarded->placeholderText().trimmed() ? std::string()
                                                                        : qt::toUtf8(typed)));
    };
    new FocusOutSaver(edit, std::move(save));
    bindings_.push_back({std::move(resolve), [guarded](const Json& value) {
                             if (guarded) {
                                 guarded->setPlainText(text(value));
                             }
                         }});
}

void SettingsBinder::custom(Resolve resolve, Apply apply) {
    bindings_.push_back({std::move(resolve), std::move(apply)});
}

void SettingsBinder::load(const Json& settings, int start) const {
    for (std::size_t index = static_cast<std::size_t>(std::max(0, start)); index < bindings_.size();
         ++index) {
        const Binding& binding = bindings_[index];
        binding.apply(binding.resolve(settings));
    }
}

} // namespace fv
