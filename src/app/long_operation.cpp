// Long operations (app/long_operation.h, spec/app-ux.md §9.3, FX7-UI-006).
#include "app/long_operation.h"

#include "app/messages.h"
#include "app/theme.h"

#include "base/thread.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <memory>
#include <thread>

namespace construe::app {

namespace {

std::atomic<bool> g_hold{false};
std::atomic<bool> g_release{false};

}  // namespace

void LongOperation::setHoldForTest(bool hold) {
    g_hold = hold;
    g_release = false;
}

void LongOperation::releaseForTest() { g_release = true; }

bool LongOperation::run(QWidget* parent, const QString& title, const std::function<void(const Cancel*)>& work) {
    auto cancel = std::make_shared<Cancel>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    const bool hold = g_hold.load();
    WorkerThread worker([cancel, done, hold, &work] {  // explicit stack size on macOS (base/thread.h)
        while (hold && !g_release && !cancel->isRequested()) std::this_thread::sleep_for(std::chrono::milliseconds(LONG_OP_POLL_MS));
        if (!cancel->isRequested()) work(cancel.get());
        *done = true;
    });
    // Until the window appears, the interface draws but takes no input (the command is in progress).
    QApplication::setOverrideCursor(Qt::WaitCursor);
    {
        QElapsedTimer t;
        t.start();
        while (!*done && t.elapsed() < LONG_OP_SHOW_DELAY_MS) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, LONG_OP_POLL_MS);
            if (!*done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    QApplication::restoreOverrideCursor();
    if (!*done) {
        QDialog dlg(parent);
        dlg.setObjectName("longOperationDialog");
        dlg.setWindowTitle(text("ui.longOp.window"));
        dlg.setModal(true);
        auto* v = new QVBoxLayout(&dlg);
        auto* label = new QLabel(title);
        label->setObjectName("longOperationText");
        label->setWordWrap(true);
        v->addWidget(label);
        auto* bar = new QProgressBar;
        bar->setObjectName("longOperationProgress");
        bar->setRange(0, 0);  // busy until the operation reports its steps
        v->addWidget(bar);
        auto* abort = new QPushButton(text("ui.longOp.abort"));
        abort->setObjectName("longOperationAbort");
        v->addWidget(abort, 0, Qt::AlignRight);
        QObject::connect(abort, &QPushButton::clicked, &dlg, [cancel, abort] {
            cancel->requested = true;
            abort->setEnabled(false);
            abort->setText(text("ui.longOp.aborting"));
        });
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &dlg, [&] {
            if (*done) return dlg.accept();
            const int total = cancel->total.load(), n = cancel->done.load();
            if (total > 0) {
                bar->setRange(0, total);
                bar->setValue(std::min(n, total));
            }
        });
        poll.start(LONG_OP_POLL_MS);
        dlg.exec();  // Esc and the window cross are aborts as well: the window closes only when the work has stopped
        while (!*done) {
            cancel->requested = true;
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, LONG_OP_POLL_MS);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    worker.join();
    return !cancel->isRequested();
}

}  // namespace construe::app
