#ifndef KIS_TOOL_HALINE_H_
#define KIS_TOOL_HALINE_H_

#include <QVector>
#include <QPointF>

#include <kis_tool_shape.h>
#include <KisToolPaintFactoryBase.h>
#include <kis_icon.h>
#include <klocalizedstring.h>

class KisToolHaline : public KisToolShape
{
    Q_OBJECT
public:
    KisToolHaline(KoCanvasBase *canvas);
    ~KisToolHaline() override;

    void paint(QPainter &gc, const KoViewConverter &converter) override;

protected:
    void beginPrimaryAction(KoPointerEvent *event) override;
    void continuePrimaryAction(KoPointerEvent *event) override;
    void endPrimaryAction(KoPointerEvent *event) override;

private:
    void correctStroke(const QVector<QPointF> &points);
    void updatePreview();

    QVector<QPointF> m_points;
    bool m_drawing {false};
};

class KisToolHalineFactory : public KisToolPaintFactoryBase
{
public:
    KisToolHalineFactory()
        : KisToolPaintFactoryBase("KritaShape/KisToolHaline")
    {
        setToolTip(i18n("Haline AI: corrects your stroke automatically"));
        setSection(ToolBoxSection::Shape);
        setIconName(koIconNameCStr("haline_eta"));
        setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_H));
        setPriority(1);
        setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
    }

    KoToolBase *createTool(KoCanvasBase *canvas) override
    {
        return new KisToolHaline(canvas);
    }
};

#endif
