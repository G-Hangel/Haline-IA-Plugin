#ifndef TOOL_HALINE_H_
#define TOOL_HALINE_H_

#include <QObject>
#include <QVariant>

class ToolHaline : public QObject
{
    Q_OBJECT
public:
    ToolHaline(QObject *parent, const QVariantList &);
    ~ToolHaline() override;
};

#endif
