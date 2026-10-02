/*
 * main.cpp — QPA plugin factory entry point.
 *
 * Registers the plugin under the key "etnaviv2d".
 * Select at runtime: QT_QPA_PLATFORM=etnaviv2d
 * With params:       QT_QPA_PLATFORM=etnaviv2d:card=/dev/dri/card0:render=/dev/dri/renderD128
 *
 * Reference: src/plugins/platforms/linuxfb/main.cpp
 */
#include <qpa/qplatformintegrationplugin.h>
#include "qetnaviv2dintegration.h"

QT_BEGIN_NAMESPACE

class QEtnaviv2dIntegrationPlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "etnaviv2d.json")
public:
    QPlatformIntegration *create(const QString &system,
                                 const QStringList &paramList) override;
};

QPlatformIntegration *QEtnaviv2dIntegrationPlugin::create(
        const QString &system, const QStringList &paramList)
{
    if (system.compare(QLatin1String("etnaviv2d"), Qt::CaseInsensitive) != 0)
        return nullptr;

    auto *integration = new QEtnaviv2dIntegration(paramList);
    if (!integration->initializeHardware()) {
        delete integration;
        return nullptr;
    }
    return integration;
}

QT_END_NAMESPACE

#include "main.moc"
