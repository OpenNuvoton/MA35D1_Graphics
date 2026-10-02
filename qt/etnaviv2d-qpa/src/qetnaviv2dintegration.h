/*
 * qetnaviv2dintegration.h — QPlatformIntegration for the etnaviv2d QPA plugin.
 *
 * Owns the shared etna2d_device and etna2d_context (used by all blittables
 * and the backing store).  Creates the KMS screen and wires all Qt platform
 * hooks.
 *
 * In plane=wayland mode, also owns a QEtnaviv2dWaylandTransport instance that
 * replaces the KMS transport.  The screen is still created (for QFbScreen
 * geometry/format state) but does no KMS ops.
 *
 * Modelled on QLinuxFbIntegration (linuxfb) + QDirectFbIntegration (directfb).
 */
#ifndef QETNAVIV2DINTEGRATION_H
#define QETNAVIV2DINTEGRATION_H

#include <qpa/qplatformintegration.h>
#include <qpa/qplatformnativeinterface.h>
#include <QtCore/QScopedPointer>

extern "C" {
#include "etna2d.h"
}

QT_BEGIN_NAMESPACE

class QEtnaviv2dScreen;
class QEtnaviv2dWaylandTransport;
class QFbVtHandler;

class QEtnaviv2dIntegration : public QPlatformIntegration,
                               public QPlatformNativeInterface
{
public:
    explicit QEtnaviv2dIntegration(const QStringList &params);
    ~QEtnaviv2dIntegration() override;

    /* Two-phase init — called by the plugin factory after construction.
     * Returns false on fatal error (factory will delete and return nullptr). */
    bool initializeHardware();

    /* QPlatformIntegration::initialize() — called by Qt after construction. */
    void initialize() override;

    /* --- QPlatformIntegration --- */
    bool hasCapability(Capability cap) const override;

    QPlatformPixmap       *createPlatformPixmap(QPlatformPixmap::PixelType type) const override;
    QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override;
    QPlatformWindow       *createPlatformWindow(QWindow *window) const override;

    QAbstractEventDispatcher *createEventDispatcher() const override;

    QPlatformFontDatabase  *fontDatabase() const override;
    QPlatformServices      *services()     const override;
    QPlatformNativeInterface *nativeInterface() const override;

    /* Wayland transport accessor — used by BackingStore in wayland mode. */
    QEtnaviv2dWaylandTransport *waylandTransport() const { return m_waylandTransport; }

private:
    void createInputHandlers();

    QStringList m_params;

    /* Shared GPU objects. */
    etna2d_device  *m_etnadev = nullptr;
    etna2d_context *m_etnaCtx = nullptr;

    /* Display. */
    QEtnaviv2dScreen            *m_screen           = nullptr; /* owned by Qt after handleScreenAdded */
    QEtnaviv2dWaylandTransport  *m_waylandTransport = nullptr; /* owned; non-null in wayland mode only */

    QScopedPointer<QPlatformFontDatabase> m_fontDb;
    QScopedPointer<QPlatformServices>     m_services;
    QScopedPointer<QFbVtHandler>             m_vtHandler;
    QPlatformInputContext                   *m_inputContext = nullptr;
};

QT_END_NAMESPACE

#endif // QETNAVIV2DINTEGRATION_H

