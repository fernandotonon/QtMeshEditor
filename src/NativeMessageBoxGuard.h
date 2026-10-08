/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef NATIVEMESSAGEBOXGUARD_H
#define NATIVEMESSAGEBOXGUARD_H

// Forces every QMessageBox onto Qt's own (widget) implementation.
//
// On macOS 27 a native QMessageBox (an NSAlert) crashes the process the moment
// it is laid out: AppKit raises an Objective-C exception while rasterising the
// alert's SF Symbol icon (`-[CUINamedVectorGlyph _rasterizeImageUsingScaleFactor:…]`
// inside `-[NSAlert runModal]`), and `+[NSApplication _crashOnException:]`
// turns it into SIGTRAP. Reproduced with Qt 6.9.3 in a 20-line program that only
// calls `QMessageBox::information` — it is not specific to any of our dialogs.
// Native file and colour dialogs are unaffected.
//
// The static helpers (`QMessageBox::information/warning/question/…`, ~80 call
// sites) build the box and `exec()` it immediately, so there is no hook to set
// `QMessageBox::Option::DontUseNativeDialog` on them. `Qt::AA_DontUseNativeDialogs`
// would also replace the native FILE pickers, which work. Instead this filter,
// installed on the application, sees the `ChildAdded` events a QMessageBox
// receives while its constructor builds its labels and buttons — i.e. before
// `exec()` decides between native and widget — and sets the option there.

#include <QCoreApplication>
#include <QEvent>
#include <QMessageBox>
#include <QObject>
#include <QtGlobal>

class NativeMessageBoxGuard : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        if (event->type() == QEvent::ChildAdded) {
            if (auto* box = qobject_cast<QMessageBox*>(watched)) {
                if (!box->testOption(QMessageBox::Option::DontUseNativeDialog))
                    box->setOption(QMessageBox::Option::DontUseNativeDialog);
            }
        }
#endif
        return QObject::eventFilter(watched, event);
    }

    /// Install one guard on `app` (idempotent). Returns the guard.
    static NativeMessageBoxGuard* install(QCoreApplication* app)
    {
        if (!app)
            return nullptr;
        if (auto* existing = app->findChild<NativeMessageBoxGuard*>(QString(), Qt::FindDirectChildrenOnly))
            return existing;
        auto* guard = new NativeMessageBoxGuard(app);
        app->installEventFilter(guard);
        return guard;
    }
};

#endif // NATIVEMESSAGEBOXGUARD_H
