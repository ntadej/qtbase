// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include "qwasmcompositor.h"
#include "qwasmwindow.h"
#include "qwasmeventtranslator.h"
#include "qwasmeventdispatcher.h"
#include "qwasmclipboard.h"
#include "qwasmevent.h"

#include <QtGui/private/qwindow_p.h>

#include <private/qguiapplication_p.h>

#include <qpa/qwindowsysteminterface.h>
#include <QtCore/qcoreapplication.h>
#include <QtGui/qguiapplication.h>

#include <emscripten/bind.h>

namespace {
QWasmWindow *asWasmWindow(QWindow *window)
{
    return static_cast<QWasmWindow*>(window->handle());
}
}  // namespace

using namespace emscripten;

Q_GUI_EXPORT int qt_defaultDpiX();

bool g_scrollingInvertedFromDevice = false;

static void mouseWheelEvent(emscripten::val event)
{
    emscripten::val wheelInverted = event["webkitDirectionInvertedFromDevice"];
    if (wheelInverted.as<bool>())
        g_scrollingInvertedFromDevice = true;
}

EMSCRIPTEN_BINDINGS(qtMouseModule) {
    function("qtMouseWheelEvent", &mouseWheelEvent);
}

QWasmCompositor::QWasmCompositor(QWasmScreen *screen)
    : QObject(screen),
      m_windowManipulation(screen),
      m_windowStack(std::bind(&QWasmCompositor::onTopWindowChanged, this)),
      m_eventTranslator(std::make_unique<QWasmEventTranslator>())
{
    m_touchDevice = std::make_unique<QPointingDevice>(
            "touchscreen", 1, QInputDevice::DeviceType::TouchScreen,
            QPointingDevice::PointerType::Finger,
            QPointingDevice::Capability::Position | QPointingDevice::Capability::Area
                | QPointingDevice::Capability::NormalizedPosition,
            10, 0);
    QWindowSystemInterface::registerInputDevice(m_touchDevice.get());
}

QWasmCompositor::~QWasmCompositor()
{
    m_windowUnderMouse.clear();

    if (m_requestAnimationFrameId != -1)
        emscripten_cancel_animation_frame(m_requestAnimationFrameId);

    deregisterEventHandlers();
    destroy();
}

void QWasmCompositor::deregisterEventHandlers()
{
    QByteArray screenElementSelector = screen()->eventTargetId().toUtf8();
    emscripten_set_keydown_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_keyup_callback(screenElementSelector.constData(), 0, 0, NULL);

    emscripten_set_focus_callback(screenElementSelector.constData(), 0, 0, NULL);

    emscripten_set_wheel_callback(screenElementSelector.constData(), 0, 0, NULL);

    emscripten_set_touchstart_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_touchend_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_touchmove_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_touchcancel_callback(screenElementSelector.constData(), 0, 0, NULL);

    screen()->element().call<void>("removeEventListener", std::string("drop"),
                                   val::module_property("qtDrop"), val(true));
}

void QWasmCompositor::destroy()
{
    // TODO(mikolaj.boc): Investigate if m_isEnabled is needed at all. It seems like a frame should
    // not be generated after this instead.
    m_isEnabled = false; // prevent frame() from creating a new m_context
}

void QWasmCompositor::initEventHandlers()
{
    if (platform() == Platform::MacOS) {
        if (!emscripten::val::global("window")["safari"].isUndefined()) {
            screen()->element().call<void>("addEventListener", val("wheel"),
                                           val::module_property("qtMouseWheelEvent"));
        }
    }

    constexpr EM_BOOL UseCapture = 1;

    const QByteArray screenElementSelector = screen()->eventTargetId().toUtf8();
    emscripten_set_keydown_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                    &keyboard_cb);
    emscripten_set_keyup_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                  &keyboard_cb);

    val screenElement = screen()->element();
    const auto callback = std::function([this](emscripten::val event) {
        if (processPointer(*PointerEvent::fromWeb(event)))
            event.call<void>("preventDefault");
    });

    m_pointerDownCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerdown", callback);
    m_pointerMoveCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointermove", callback);
    m_pointerUpCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerup", callback);
    m_pointerEnterCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerenter", callback);
    m_pointerLeaveCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerleave", callback);

    emscripten_set_focus_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                  &focus_cb);

    emscripten_set_wheel_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                  &wheel_cb);

    emscripten_set_touchstart_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                       &touchCallback);
    emscripten_set_touchend_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                     &touchCallback);
    emscripten_set_touchmove_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                      &touchCallback);
    emscripten_set_touchcancel_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                        &touchCallback);

    screenElement.call<void>("addEventListener", std::string("drop"),
                             val::module_property("qtDrop"), val(true));
    screenElement.set("data-qtdropcontext", // ? unique
                      emscripten::val(quintptr(reinterpret_cast<void *>(screen()))));
}

void QWasmCompositor::setEnabled(bool enabled)
{
    m_isEnabled = enabled;
}

void QWasmCompositor::startResize(Qt::Edges edges)
{
    m_windowManipulation.startResize(edges);
}

void QWasmCompositor::addWindow(QWasmWindow *window)
{
    m_windowStack.pushWindow(window);
    m_windowStack.topWindow()->requestActivateWindow();
}

void QWasmCompositor::removeWindow(QWasmWindow *window)
{
    m_requestUpdateWindows.remove(window);
    m_windowStack.removeWindow(window);
    if (m_windowStack.topWindow())
        m_windowStack.topWindow()->requestActivateWindow();
}

void QWasmCompositor::raise(QWasmWindow *window)
{
    m_windowStack.raise(window);
}

void QWasmCompositor::lower(QWasmWindow *window)
{
    m_windowStack.lower(window);
}

QWindow *QWasmCompositor::windowAt(QPoint targetPointInScreenCoords, int padding) const
{
    const auto found = std::find_if(
            m_windowStack.begin(), m_windowStack.end(),
            [padding, &targetPointInScreenCoords](const QWasmWindow *window) {
                const QRect geometry = window->windowFrameGeometry().adjusted(-padding, -padding,
                                                                              padding, padding);

                return window->isVisible() && geometry.contains(targetPointInScreenCoords);
            });
    return found != m_windowStack.end() ? (*found)->window() : nullptr;
}

QWindow *QWasmCompositor::keyWindow() const
{
    return m_windowStack.topWindow() ? m_windowStack.topWindow()->window() : nullptr;
}

void QWasmCompositor::requestUpdateAllWindows()
{
    m_requestUpdateAllWindows = true;
    requestUpdate();
}

void QWasmCompositor::requestUpdateWindow(QWasmWindow *window, UpdateRequestDeliveryType updateType)
{
    auto it = m_requestUpdateWindows.find(window);
    if (it == m_requestUpdateWindows.end()) {
        m_requestUpdateWindows.insert(window, updateType);
    } else {
        // Already registered, but upgrade ExposeEventDeliveryType to UpdateRequestDeliveryType.
        // if needed, to make sure QWindow::updateRequest's are matched.
        if (it.value() == ExposeEventDelivery && updateType == UpdateRequestDelivery)
            it.value() = UpdateRequestDelivery;
    }

    requestUpdate();
}

// Requests an update/new frame using RequestAnimationFrame
void QWasmCompositor::requestUpdate()
{
    if (m_requestAnimationFrameId != -1)
        return;

    static auto frame = [](double frameTime, void *context) -> int {
        Q_UNUSED(frameTime);

        QWasmCompositor *compositor = reinterpret_cast<QWasmCompositor *>(context);

        compositor->m_requestAnimationFrameId = -1;
        compositor->deliverUpdateRequests();

        return 0;
    };
    m_requestAnimationFrameId = emscripten_request_animation_frame(frame, this);
}

void QWasmCompositor::deliverUpdateRequests()
{
    // We may get new update requests during the window content update below:
    // prepare for recording the new update set by setting aside the current
    // update set.
    auto requestUpdateWindows = m_requestUpdateWindows;
    m_requestUpdateWindows.clear();
    bool requestUpdateAllWindows = m_requestUpdateAllWindows;
    m_requestUpdateAllWindows = false;

    // Update window content, either all windows or a spesific set of windows. Use the correct
    // update type: QWindow subclasses expect that requested and delivered updateRequests matches
    // exactly.
    m_inDeliverUpdateRequest = true;
    if (requestUpdateAllWindows) {
        for (QWasmWindow *window : m_windowStack) {
            auto it = requestUpdateWindows.find(window);
            UpdateRequestDeliveryType updateType =
                (it == m_requestUpdateWindows.end() ? ExposeEventDelivery : it.value());
            deliverUpdateRequest(window, updateType);
        }
    } else {
        for (auto it = requestUpdateWindows.constBegin(); it != requestUpdateWindows.constEnd(); ++it) {
            auto *window = it.key();
            UpdateRequestDeliveryType updateType = it.value();
            deliverUpdateRequest(window, updateType);
        }
    }
    m_inDeliverUpdateRequest = false;
    frame(requestUpdateAllWindows, requestUpdateWindows.keys());
}

void QWasmCompositor::deliverUpdateRequest(QWasmWindow *window, UpdateRequestDeliveryType updateType)
{
    // update by deliverUpdateRequest and expose event accordingly.
    if (updateType == UpdateRequestDelivery) {
        window->QPlatformWindow::deliverUpdateRequest();
    } else {
        QWindow *qwindow = window->window();
        QWindowSystemInterface::handleExposeEvent<QWindowSystemInterface::SynchronousDelivery>(
            qwindow, QRect(QPoint(0, 0), qwindow->geometry().size()));
    }
}

void QWasmCompositor::handleBackingStoreFlush(QWindow *window)
{
    // Request update to flush the updated backing store content, unless we are currently
    // processing an update, in which case the new content will flushed as a part of that update.
    if (!m_inDeliverUpdateRequest)
        requestUpdateWindow(asWasmWindow(window));
}

int dpiScaled(qreal value)
{
    return value * (qreal(qt_defaultDpiX()) / 96.0);
}

void QWasmCompositor::frame(bool all, const QList<QWasmWindow *> &windows)
{
    if (!m_isEnabled || m_windowStack.empty() || !screen())
        return;

    if (all) {
        std::for_each(m_windowStack.rbegin(), m_windowStack.rend(),
                      [](QWasmWindow *window) { window->paint(); });
    } else {
        std::for_each(windows.begin(), windows.end(), [](QWasmWindow *window) { window->paint(); });
    }
}

void QWasmCompositor::WindowManipulation::resizeWindow(const QPoint& amount)
{
    const auto& minShrink = std::get<ResizeState>(m_state->operationSpecific).m_minShrink;
    const auto& maxGrow = std::get<ResizeState>(m_state->operationSpecific).m_maxGrow;
    const auto &resizeEdges = std::get<ResizeState>(m_state->operationSpecific).m_resizeEdges;

    const QPoint cappedGrowVector(
            std::min(maxGrow.x(),
                     std::max(minShrink.x(),
                              (resizeEdges & Qt::Edge::LeftEdge)            ? -amount.x()
                                      : (resizeEdges & Qt::Edge::RightEdge) ? amount.x()
                                                                            : 0)),
            std::min(maxGrow.y(),
                     std::max(minShrink.y(),
                              (resizeEdges & Qt::Edge::TopEdge)              ? -amount.y()
                                      : (resizeEdges & Qt::Edge::BottomEdge) ? amount.y()
                                                                             : 0)));

    const auto& initialBounds =
        std::get<ResizeState>(m_state->operationSpecific).m_initialWindowBounds;
    m_state->window->setGeometry(initialBounds.adjusted(
            (resizeEdges & Qt::Edge::LeftEdge) ? -cappedGrowVector.x() : 0,
            (resizeEdges & Qt::Edge::TopEdge) ? -cappedGrowVector.y() : 0,
            (resizeEdges & Qt::Edge::RightEdge) ? cappedGrowVector.x() : 0,
            (resizeEdges & Qt::Edge::BottomEdge) ? cappedGrowVector.y() : 0));
}

void QWasmCompositor::onTopWindowChanged()
{
    constexpr int zOrderForElementInFrontOfScreen = 3;
    int z = zOrderForElementInFrontOfScreen;
    std::for_each(m_windowStack.rbegin(), m_windowStack.rend(),
                  [&z](QWasmWindow *window) { window->setZOrder(z++); });

    auto it = m_windowStack.begin();
    if (it == m_windowStack.end()) {
        return;
    }
    (*it)->onActivationChanged(true);
    ++it;
    for (; it != m_windowStack.end(); ++it) {
        (*it)->onActivationChanged(false);
    }
}

QWasmScreen *QWasmCompositor::screen()
{
    return static_cast<QWasmScreen *>(parent());
}

int QWasmCompositor::keyboard_cb(int eventType, const EmscriptenKeyboardEvent *keyEvent, void *userData)
{
    QWasmCompositor *wasmCompositor = reinterpret_cast<QWasmCompositor *>(userData);
    return static_cast<int>(wasmCompositor->processKeyboard(eventType, keyEvent));
}

int QWasmCompositor::focus_cb(int eventType, const EmscriptenFocusEvent *focusEvent, void *userData)
{
    Q_UNUSED(eventType)
    Q_UNUSED(focusEvent)
    Q_UNUSED(userData)

    return 0;
}

int QWasmCompositor::wheel_cb(int eventType, const EmscriptenWheelEvent *wheelEvent, void *userData)
{
    QWasmCompositor *compositor = (QWasmCompositor *) userData;
    return static_cast<int>(compositor->processWheel(eventType, wheelEvent));
}

int QWasmCompositor::touchCallback(int eventType, const EmscriptenTouchEvent *touchEvent, void *userData)
{
    auto compositor = reinterpret_cast<QWasmCompositor*>(userData);
    return static_cast<int>(compositor->processTouch(eventType, touchEvent));
}

bool QWasmCompositor::processPointer(const PointerEvent& event)
{
    if (event.pointerType != PointerType::Mouse)
        return false;

    QWindow *const targetWindow = ([this, &event]() -> QWindow * {
        auto *targetWindow = m_mouseCaptureWindow != nullptr ? m_mouseCaptureWindow.get()
                : m_windowManipulation.operation() == WindowManipulation::Operation::None
                ? screen()->compositor()->windowAt(event.point, 5)
                : nullptr;

        return targetWindow ? targetWindow : m_lastMouseTargetWindow.get();
    })();
    if (!targetWindow)
        return false;
    m_lastMouseTargetWindow = targetWindow;

    const QPoint pointInTargetWindowCoords = targetWindow->mapFromGlobal(event.point);
    const bool pointerIsWithinTargetWindowBounds = targetWindow->geometry().contains(event.point);
    const bool isTargetWindowBlocked = QGuiApplicationPrivate::instance()->isWindowBlocked(targetWindow);

    if (m_mouseInScreen && m_windowUnderMouse != targetWindow
        && pointerIsWithinTargetWindowBounds) {
        // delayed mouse enter
        enterWindow(targetWindow, pointInTargetWindowCoords, event.point);
        m_windowUnderMouse = targetWindow;
    }

    QWasmWindow *wasmTargetWindow = asWasmWindow(targetWindow);
    Qt::WindowStates windowState = targetWindow->windowState();
    const bool isTargetWindowResizable = !windowState.testFlag(Qt::WindowMaximized) && !windowState.testFlag(Qt::WindowFullScreen);

    switch (event.type) {
    case EventType::PointerDown:
    {
        screen()->element().call<void>("setPointerCapture", event.pointerId);

        if (targetWindow)
            targetWindow->requestActivate();

        m_windowManipulation.onPointerDown(event, targetWindow);
        break;
    }
    case EventType::PointerUp:
    {
        screen()->element().call<void>("releasePointerCapture", event.pointerId);

        m_windowManipulation.onPointerUp(event);
        break;
    }
    case EventType::PointerMove:
    {
        if (wasmTargetWindow && event.mouseButtons.testFlag(Qt::NoButton)) {
            const bool isOnResizeRegion = wasmTargetWindow->isPointOnResizeRegion(event.point);

            if (isTargetWindowResizable && isOnResizeRegion && !isTargetWindowBlocked) {
                const QCursor resizingCursor = QWasmEventTranslator::cursorForEdges(
                        wasmTargetWindow->resizeEdgesAtPoint(event.point));

                if (resizingCursor != targetWindow->cursor()) {
                    m_isResizeCursorDisplayed = true;
                    QWasmCursor::setOverrideWasmCursor(resizingCursor, targetWindow->screen());
                }
            } else if (m_isResizeCursorDisplayed) {  // off resizing area
                m_isResizeCursorDisplayed = false;
                QWasmCursor::clearOverrideWasmCursor(targetWindow->screen());
            }
        }

        m_windowManipulation.onPointerMove(event);
        if (m_windowManipulation.operation() != WindowManipulation::Operation::None)
            requestUpdate();
        break;
    }
    case EventType::PointerEnter:
        processMouseEnter(nullptr);
        break;
    case EventType::PointerLeave:
        processMouseLeave();
        break;
    default:
        break;
    };

    if (!pointerIsWithinTargetWindowBounds && event.mouseButtons.testFlag(Qt::NoButton)) {
        leaveWindow(m_lastMouseTargetWindow);
    }

    const bool eventAccepted = deliverEventToTarget(event, targetWindow);
    if (!eventAccepted && event.type == EventType::PointerDown)
        QGuiApplicationPrivate::instance()->closeAllPopups();
    return eventAccepted;
}

bool QWasmCompositor::deliverEventToTarget(const PointerEvent &event, QWindow *eventTarget)
{
    Q_ASSERT(!m_mouseCaptureWindow || m_mouseCaptureWindow.get() == eventTarget);

    const QPoint targetPointClippedToScreen(
            std::max(screen()->geometry().left(),
                     std::min(screen()->geometry().right(), event.point.x())),
            std::max(screen()->geometry().top(),
                     std::min(screen()->geometry().bottom(), event.point.y())));

    bool deliveringToPreviouslyClickedWindow = false;

    if (!eventTarget) {
        if (event.type != EventType::PointerUp || !m_lastMouseTargetWindow)
            return false;

        eventTarget = m_lastMouseTargetWindow;
        m_lastMouseTargetWindow = nullptr;
        deliveringToPreviouslyClickedWindow = true;
    }

    WindowArea windowArea = WindowArea::Client;
    if (!deliveringToPreviouslyClickedWindow && !m_mouseCaptureWindow
        && !eventTarget->geometry().contains(targetPointClippedToScreen)) {
        if (!eventTarget->frameGeometry().contains(targetPointClippedToScreen))
            return false;
        windowArea = WindowArea::NonClient;
    }

    const QEvent::Type eventType =
        MouseEvent::mouseEventTypeFromEventType(event.type, windowArea);

    return eventType != QEvent::None &&
           QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
               eventTarget, QWasmIntegration::getTimestamp(),
               eventTarget->mapFromGlobal(targetPointClippedToScreen),
               targetPointClippedToScreen, event.mouseButtons, event.mouseButton,
               eventType, event.modifiers);
}

QWasmCompositor::WindowManipulation::WindowManipulation(QWasmScreen *screen)
    : m_screen(screen)
{
    Q_ASSERT(!!screen);
}

QWasmCompositor::WindowManipulation::Operation QWasmCompositor::WindowManipulation::operation() const
{
    if (!m_state)
        return Operation::None;

    return std::holds_alternative<MoveState>(m_state->operationSpecific)
        ? Operation::Move : Operation::Resize;
}

void QWasmCompositor::WindowManipulation::onPointerDown(
    const PointerEvent& event, QWindow* windowAtPoint)
{
    // Only one operation at a time.
    if (operation() != Operation::None)
        return;

    if (event.mouseButton != Qt::MouseButton::LeftButton)
        return;

    const bool isTargetWindowResizable =
        !windowAtPoint->windowStates().testFlag(Qt::WindowMaximized) &&
        !windowAtPoint->windowStates().testFlag(Qt::WindowFullScreen);
    if (!isTargetWindowResizable)
        return;

    const bool isTargetWindowBlocked =
        QGuiApplicationPrivate::instance()->isWindowBlocked(windowAtPoint);
    if (isTargetWindowBlocked)
        return;

    std::unique_ptr<std::variant<ResizeState, MoveState>> operationSpecific;
    if (asWasmWindow(windowAtPoint)->isPointOnTitle(event.point)) {
        operationSpecific = std::make_unique<std::variant<ResizeState, MoveState>>(
                MoveState{ .m_lastPointInScreenCoords = event.point });
    } else if (asWasmWindow(windowAtPoint)->isPointOnResizeRegion(event.point)) {
        operationSpecific = std::make_unique<std::variant<ResizeState, MoveState>>(ResizeState{
                .m_resizeEdges = asWasmWindow(windowAtPoint)->resizeEdgesAtPoint(event.point),
                .m_originInScreenCoords = event.point,
                .m_initialWindowBounds = windowAtPoint->geometry(),
                .m_minShrink =
                        QPoint(windowAtPoint->minimumWidth() - windowAtPoint->geometry().width(),
                               windowAtPoint->minimumHeight() - windowAtPoint->geometry().height()),
                .m_maxGrow =
                        QPoint(windowAtPoint->maximumWidth() - windowAtPoint->geometry().width(),
                               windowAtPoint->maximumHeight() - windowAtPoint->geometry().height()),
        });
    } else {
        return;
    }

    m_state.reset(new OperationState{
        .pointerId = event.pointerId,
        .window = windowAtPoint,
        .operationSpecific = std::move(*operationSpecific),
    });
}

void QWasmCompositor::WindowManipulation::onPointerMove(
    const PointerEvent& event)
{
    m_systemDragInitData = {
        .lastMouseMovePoint = m_screen->clipPoint(event.point),
        .lastMousePointerId = event.pointerId,
    };

    if (operation() == Operation::None || event.pointerId != m_state->pointerId)
        return;

    switch (operation()) {
        case Operation::Move: {
            const QPoint targetPointClippedToScreen = m_screen->clipPoint(event.point);
            const QPoint difference = targetPointClippedToScreen -
                std::get<MoveState>(m_state->operationSpecific).m_lastPointInScreenCoords;

            std::get<MoveState>(m_state->operationSpecific).m_lastPointInScreenCoords = targetPointClippedToScreen;

            m_state->window->setPosition(m_state->window->position() + difference);
            break;
        }
        case Operation::Resize: {
            const auto pointInScreenCoords = m_screen->geometry().topLeft() + event.point;
            resizeWindow(pointInScreenCoords -
                std::get<ResizeState>(m_state->operationSpecific).m_originInScreenCoords);
            break;
        }
        case Operation::None:
            Q_ASSERT(0);
            break;
    }
}

void QWasmCompositor::WindowManipulation::onPointerUp(const PointerEvent& event)
{
    if (operation() == Operation::None || event.mouseButtons != 0 || event.pointerId != m_state->pointerId)
        return;

    m_state.reset();
}

void QWasmCompositor::WindowManipulation::startResize(Qt::Edges edges)
{
    Q_ASSERT_X(operation() == Operation::None, Q_FUNC_INFO,
               "Resize must not start anew when one is in progress");

    auto *window = m_screen->compositor()->windowAt(m_systemDragInitData.lastMouseMovePoint);
    if (Q_UNLIKELY(!window))
        return;

    m_state.reset(new OperationState{
        .pointerId = m_systemDragInitData.lastMousePointerId,
        .window = window,
        .operationSpecific =
            ResizeState{
                .m_resizeEdges = edges,
                .m_originInScreenCoords = m_systemDragInitData.lastMouseMovePoint,
                .m_initialWindowBounds = window->geometry(),
                .m_minShrink =
                    QPoint(window->minimumWidth() - window->geometry().width(),
                        window->minimumHeight() - window->geometry().height()),
                .m_maxGrow =
                    QPoint(window->maximumWidth() - window->geometry().width(),
                        window->maximumHeight() - window->geometry().height()),
            },
    });
    m_screen->element().call<void>("setPointerCapture", m_systemDragInitData.lastMousePointerId);
}

bool QWasmCompositor::processKeyboard(int eventType, const EmscriptenKeyboardEvent *emKeyEvent)
{
    constexpr bool ProceedToNativeEvent = false;
    Q_ASSERT(eventType == EMSCRIPTEN_EVENT_KEYDOWN || eventType == EMSCRIPTEN_EVENT_KEYUP);

    auto translatedEvent = m_eventTranslator->translateKeyEvent(eventType, emKeyEvent);

    const QFlags<Qt::KeyboardModifier> modifiers = KeyboardModifier::getForEvent(*emKeyEvent);

    const auto clipboardResult = QWasmIntegration::get()->getWasmClipboard()->processKeyboard(
            translatedEvent, modifiers);

    using ProcessKeyboardResult = QWasmClipboard::ProcessKeyboardResult;
    if (clipboardResult == ProcessKeyboardResult::NativeClipboardEventNeeded)
        return ProceedToNativeEvent;

    if (translatedEvent.text.isEmpty())
        translatedEvent.text = QString(emKeyEvent->key);
    if (translatedEvent.text.size() > 1)
        translatedEvent.text.clear();
    const auto result =
            QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
                    0, translatedEvent.type, translatedEvent.key, modifiers, translatedEvent.text);
    return clipboardResult == ProcessKeyboardResult::NativeClipboardEventAndCopiedDataNeeded
            ? ProceedToNativeEvent
            : result;
}

bool QWasmCompositor::processWheel(int eventType, const EmscriptenWheelEvent *wheelEvent)
{
    Q_UNUSED(eventType);

    const EmscriptenMouseEvent* mouseEvent = &wheelEvent->mouse;

    int scrollFactor = 0;
    switch (wheelEvent->deltaMode) {
        case DOM_DELTA_PIXEL:
            scrollFactor = 1;
            break;
        case DOM_DELTA_LINE:
            scrollFactor = 12;
            break;
        case DOM_DELTA_PAGE:
            scrollFactor = 20;
            break;
    };

    scrollFactor = -scrollFactor; // Web scroll deltas are inverted from Qt deltas.

    Qt::KeyboardModifiers modifiers = KeyboardModifier::getForEvent(*mouseEvent);
    QPoint targetPointInScreenElementCoords(mouseEvent->targetX, mouseEvent->targetY);
    QPoint targetPointInScreenCoords =
            screen()->geometry().topLeft() + targetPointInScreenElementCoords;

    QWindow *targetWindow = screen()->compositor()->windowAt(targetPointInScreenCoords, 5);
    if (!targetWindow)
        return 0;
    QPoint pointInTargetWindowCoords = targetWindow->mapFromGlobal(targetPointInScreenCoords);

    QPoint pixelDelta;

    if (wheelEvent->deltaY != 0) pixelDelta.setY(wheelEvent->deltaY * scrollFactor);
    if (wheelEvent->deltaX != 0) pixelDelta.setX(wheelEvent->deltaX * scrollFactor);

    QPoint angleDelta = pixelDelta; // FIXME: convert from pixels?

    bool accepted = QWindowSystemInterface::handleWheelEvent(
            targetWindow, QWasmIntegration::getTimestamp(), pointInTargetWindowCoords,
            targetPointInScreenCoords, pixelDelta, angleDelta, modifiers,
            Qt::NoScrollPhase, Qt::MouseEventNotSynthesized,
            g_scrollingInvertedFromDevice);
    return accepted;
}

bool QWasmCompositor::processTouch(int eventType, const EmscriptenTouchEvent *touchEvent)
{
    QList<QWindowSystemInterface::TouchPoint> touchPointList;
    touchPointList.reserve(touchEvent->numTouches);
    QWindow *targetWindow = nullptr;

    for (int i = 0; i < touchEvent->numTouches; i++) {

        const EmscriptenTouchPoint *touches = &touchEvent->touches[i];

        QPoint targetPointInScreenElementCoords(touches->targetX, touches->targetY);
        QPoint targetPointInScreenCoords =
                screen()->geometry().topLeft() + targetPointInScreenElementCoords;

        targetWindow = screen()->compositor()->windowAt(targetPointInScreenCoords, 5);
        if (targetWindow == nullptr)
            continue;

        QWindowSystemInterface::TouchPoint touchPoint;

        touchPoint.area = QRect(0, 0, 8, 8);
        touchPoint.id = touches->identifier;
        touchPoint.pressure = 1.0;

        touchPoint.area.moveCenter(targetPointInScreenCoords);

        const auto tp = m_pressedTouchIds.constFind(touchPoint.id);
        if (tp != m_pressedTouchIds.constEnd())
            touchPoint.normalPosition = tp.value();

        QPointF pointInTargetWindowCoords = QPointF(targetWindow->mapFromGlobal(targetPointInScreenCoords));
        QPointF normalPosition(pointInTargetWindowCoords.x() / targetWindow->width(),
                               pointInTargetWindowCoords.y() / targetWindow->height());

        const bool stationaryTouchPoint = (normalPosition == touchPoint.normalPosition);
        touchPoint.normalPosition = normalPosition;

        switch (eventType) {
            case EMSCRIPTEN_EVENT_TOUCHSTART:
                if (tp != m_pressedTouchIds.constEnd()) {
                    touchPoint.state = (stationaryTouchPoint
                                        ? QEventPoint::State::Stationary
                                        : QEventPoint::State::Updated);
                } else {
                    touchPoint.state = QEventPoint::State::Pressed;
                }
                m_pressedTouchIds.insert(touchPoint.id, touchPoint.normalPosition);

                break;
            case EMSCRIPTEN_EVENT_TOUCHEND:
                touchPoint.state = QEventPoint::State::Released;
                m_pressedTouchIds.remove(touchPoint.id);
                break;
            case EMSCRIPTEN_EVENT_TOUCHMOVE:
                touchPoint.state = (stationaryTouchPoint
                                    ? QEventPoint::State::Stationary
                                    : QEventPoint::State::Updated);

                m_pressedTouchIds.insert(touchPoint.id, touchPoint.normalPosition);
                break;
            default:
                break;
        }

        touchPointList.append(touchPoint);
    }

    QFlags<Qt::KeyboardModifier> keyModifier = KeyboardModifier::getForEvent(*touchEvent);

    bool accepted = false;

    if (eventType == EMSCRIPTEN_EVENT_TOUCHCANCEL)
        accepted = QWindowSystemInterface::handleTouchCancelEvent(targetWindow, QWasmIntegration::getTimestamp(), m_touchDevice.get(), keyModifier);
    else
        accepted = QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(
                targetWindow, QWasmIntegration::getTimestamp(), m_touchDevice.get(), touchPointList, keyModifier);

    return static_cast<int>(accepted);
}

void QWasmCompositor::setCapture(QWasmWindow *window)
{
    Q_ASSERT(std::find(m_windowStack.begin(), m_windowStack.end(), window) != m_windowStack.end());
    m_mouseCaptureWindow = window->window();
}

void QWasmCompositor::releaseCapture()
{
    m_mouseCaptureWindow = nullptr;
}

void QWasmCompositor::leaveWindow(QWindow *window)
{
    m_windowUnderMouse = nullptr;
    QWindowSystemInterface::handleLeaveEvent<QWindowSystemInterface::SynchronousDelivery>(window);
}

void QWasmCompositor::enterWindow(QWindow *window, const QPoint &pointInTargetWindowCoords, const QPoint &targetPointInScreenCoords)
{
    QWindowSystemInterface::handleEnterEvent<QWindowSystemInterface::SynchronousDelivery>(window, pointInTargetWindowCoords, targetPointInScreenCoords);
}

bool QWasmCompositor::processMouseEnter(const EmscriptenMouseEvent *mouseEvent)
{
    Q_UNUSED(mouseEvent)
    // mouse has entered the screen area
    m_mouseInScreen = true;
    return true;
}

bool QWasmCompositor::processMouseLeave()
{
    m_mouseInScreen = false;
    return true;
}
