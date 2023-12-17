#include "qtwindowcontext_p.h"

#include <QtCore/QDebug>

namespace QWK {

    static constexpr const quint8 kDefaultResizeBorderThickness = 8;

    static Qt::CursorShape calculateCursorShape(const QWindow *window, const QPoint &pos) {
#ifdef Q_OS_MACOS
        Q_UNUSED(window);
        Q_UNUSED(pos);
        return Qt::ArrowCursor;
#else
        Q_ASSERT(window);
        if (!window) {
            return Qt::ArrowCursor;
        }
        if (window->visibility() != QWindow::Windowed) {
            return Qt::ArrowCursor;
        }
        const int x = pos.x();
        const int y = pos.y();
        const int w = window->width();
        const int h = window->height();
        if (((x < kDefaultResizeBorderThickness) && (y < kDefaultResizeBorderThickness)) ||
            ((x >= (w - kDefaultResizeBorderThickness)) &&
             (y >= (h - kDefaultResizeBorderThickness)))) {
            return Qt::SizeFDiagCursor;
        }
        if (((x >= (w - kDefaultResizeBorderThickness)) && (y < kDefaultResizeBorderThickness)) ||
            ((x < kDefaultResizeBorderThickness) && (y >= (h - kDefaultResizeBorderThickness)))) {
            return Qt::SizeBDiagCursor;
        }
        if ((x < kDefaultResizeBorderThickness) || (x >= (w - kDefaultResizeBorderThickness))) {
            return Qt::SizeHorCursor;
        }
        if ((y < kDefaultResizeBorderThickness) || (y >= (h - kDefaultResizeBorderThickness))) {
            return Qt::SizeVerCursor;
        }
        return Qt::ArrowCursor;
#endif
    }

    static inline Qt::Edges calculateWindowEdges(const QWindow *window, const QPoint &pos) {
#ifdef Q_OS_MACOS
        Q_UNUSED(window);
        Q_UNUSED(pos);
        return {};
#else
        Q_ASSERT(window);
        if (!window) {
            return {};
        }
        if (window->visibility() != QWindow::Windowed) {
            return {};
        }
        Qt::Edges edges = {};
        const int x = pos.x();
        const int y = pos.y();
        if (x < kDefaultResizeBorderThickness) {
            edges |= Qt::LeftEdge;
        }
        if (x >= (window->width() - kDefaultResizeBorderThickness)) {
            edges |= Qt::RightEdge;
        }
        if (y < kDefaultResizeBorderThickness) {
            edges |= Qt::TopEdge;
        }
        if (y >= (window->height() - kDefaultResizeBorderThickness)) {
            edges |= Qt::BottomEdge;
        }
        return edges;
#endif
    }

    class QtWindowEventFilter : public QObject {
    public:
        explicit QtWindowEventFilter(AbstractWindowContext *context, QObject *parent = nullptr);
        ~QtWindowEventFilter() override;

        enum WindowStatus {
            Idle,
            WaitingRelease,
            PreparingMove,
            Moving,
            Resizing,
        };

    protected:
        bool eventFilter(QObject *object, QEvent *event) override;

    private:
        AbstractWindowContext *m_context;
        bool m_cursorShapeChanged;
        WindowStatus m_windowStatus;
    };

    QtWindowEventFilter::QtWindowEventFilter(AbstractWindowContext *context, QObject *parent)
        : QObject(parent), m_context(context), m_cursorShapeChanged(false), m_windowStatus(Idle) {
        m_context->window()->installEventFilter(this);
    }

    QtWindowEventFilter::~QtWindowEventFilter() = default;

    bool QtWindowEventFilter::eventFilter(QObject *object, QEvent *event) {
        auto type = event->type();
        if (type < QEvent::MouseButtonPress || type > QEvent::MouseMove) {
            return false;
        }
        QObject *host = m_context->host();
        QWindow *window = m_context->window();
        WindowItemDelegate *delegate = m_context->delegate();
        bool fixedSize = delegate->isHostSizeFixed(host);
        auto me = static_cast<const QMouseEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QPoint scenePos = me->scenePosition().toPoint();
        QPoint globalPos = me->globalPosition().toPoint();
#else
        QPoint scenePos = me->windowPos().toPoint();
        QPoint globalPos = me->screenPos().toPoint();
#endif
        bool inTitleBar = m_context->isInTitleBarDraggableArea(scenePos);
        switch (type) {
            case QEvent::MouseButtonPress: {
                switch (me->button()) {
                    case Qt::LeftButton: {
                        if (!fixedSize) {
                            Qt::Edges edges = calculateWindowEdges(window, scenePos);
                            if (edges != Qt::Edges()) {
                                m_windowStatus = Resizing;
                                window->startSystemResize(edges);
                                event->accept();
                                return true;
                            }
                        }
                        if (inTitleBar) {
                            // If we call startSystemMove() now but release the mouse without actual
                            // movement, there will be no MouseReleaseEvent, so we defer it when the
                            // mouse is actually moving for the first time
                            m_windowStatus = PreparingMove;
                            event->accept();
                            return true;
                        }
                        m_windowStatus = WaitingRelease;
                        break;
                    }
                    case Qt::RightButton: {
                        m_context->showSystemMenu(globalPos);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }

            case QEvent::MouseButtonRelease: {
                switch (m_windowStatus) {
                    case PreparingMove:
                    case Moving:
                    case Resizing: {
                        m_windowStatus = Idle;
                        event->accept();
                        return true;
                    }
                    case WaitingRelease: {
                        m_windowStatus = Idle;
                        break;
                    }
                    default: {
                        if (inTitleBar) {
                            event->accept();
                            return true;
                        }
                        break;
                    }
                }
                break;
            }

            case QEvent::MouseMove: {
                switch (m_windowStatus) {
                    case Moving: {
                        return true;
                    }
                    case PreparingMove: {
                        m_windowStatus = Moving;
                        window->startSystemMove();
                        event->accept();
                        return true;
                    }
                    case Idle: {
                        if (!fixedSize) {
                            const Qt::CursorShape shape = calculateCursorShape(window, scenePos);
                            if (shape == Qt::ArrowCursor) {
                                if (m_cursorShapeChanged) {
                                    delegate->restoreCursorShape(host);
                                    m_cursorShapeChanged = false;
                                }
                            } else {
                                delegate->setCursorShape(host, shape);
                                m_cursorShapeChanged = true;
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
                break;
            }

            case QEvent::MouseButtonDblClick: {
                if (me->button() == Qt::LeftButton && inTitleBar && !fixedSize) {
                    Qt::WindowStates windowState = delegate->getWindowState(host);
                    if (!(windowState & Qt::WindowFullScreen)) {
                        if (windowState & Qt::WindowMaximized) {
                            delegate->setWindowState(host, windowState & ~Qt::WindowMaximized);
                        } else {
                            delegate->setWindowState(host, windowState | Qt::WindowMaximized);
                        }
                        event->accept();
                        return true;
                    }
                }
                break;
            }

            default:
                break;
        }
        return false;
    }

    QtWindowContext::QtWindowContext() : AbstractWindowContext() {
    }

    QtWindowContext::~QtWindowContext() = default;

    QString QtWindowContext::key() const {
        return QStringLiteral("qt");
    }

    void QtWindowContext::virtual_hook(int id, void *data) {
        switch (id) {
            case ShowSystemMenuHook:
                return;
            default:
                break;
        }
        AbstractWindowContext::virtual_hook(id, data);
    }

    bool QtWindowContext::setupHost() {
        m_delegate->setWindowFlags(m_host, Qt::FramelessWindowHint);
        std::ignore = new QtWindowEventFilter(this, this);
        return true;
    }

}