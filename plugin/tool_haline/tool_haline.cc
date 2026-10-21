#include "tool_haline.h"

#include <QFile>
#include <QDir>

#include <kpluginfactory.h>
#include <KoToolRegistry.h>

#include "kis_tool_haline.h"

static void halineLog(const QString &msg)
{
    QFile f(QDir::tempPath() + "/haline_plugin.log");
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(msg.toUtf8() + "\n");
    }
}

__attribute__((constructor)) static void halineDllLoaded()
{
    halineLog("DLL loaded");
}

K_PLUGIN_FACTORY_WITH_JSON(ToolHalineFactory, "kritatoolhaline.json", registerPlugin<ToolHaline>();)

ToolHaline::ToolHaline(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    halineLog("ToolHaline ctor");
    KoToolRegistry::instance()->add(new KisToolHalineFactory());
}

ToolHaline::~ToolHaline()
{
}

#include "tool_haline.moc"
