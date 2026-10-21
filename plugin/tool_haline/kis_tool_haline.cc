#include "kis_tool_haline.h"

#include <KoPointerEvent.h>
#include <KoCanvasBase.h>
#include <kis_cursor.h>
#include <kis_debug.h>
#include <kis_figure_painting_tool_helper.h>
#include <kundo2magicstring.h>
#include <kis_image.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

static void halineToolLog(const QString &msg)
{
    QFile f(QDir::tempPath() + "/haline_plugin.log");
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(msg.toUtf8() + "\n");
    }
}

namespace {

enum Shape { SHAPE_LINE = 0, SHAPE_RECT, SHAPE_CIRCLE, SHAPE_CURVE, SHAPE_TRIANGLE, SHAPE_POLYLINE };
const char *const kShapeNames[] = {"LINE", "RECTANGLE", "CIRCLE", "CURVE", "TRIANGLE", "POLYLINE"};
constexpr int kRaster = 256;

// haline_engine.dll is MSVC/libtorch; loaded at runtime through its C ABI so the
// mingw-built plugin never links against torch.
class Engine
{
public:
    static Engine &instance()
    {
        static Engine e;
        return e;
    }

    bool ready() const { return m_process != nullptr; }

    int classify(const QVector<uchar> &gray, int w, int h, float probs[6])
    {
        QVector<uchar> clean(gray.size());
        return m_process(gray.constData(), w, h, clean.data(), probs);
    }

private:
    using LoadFn = int (*)(const char *, const char *);
    using ErrFn = const char *(*)();
    using ProcessFn = int (*)(const uchar *, int, int, uchar *, float *);

    Engine()
    {
        QStringList dirs;
        const QString env = qEnvironmentVariable("HALINE_ENGINE_DIR");
        if (!env.isEmpty()) dirs << env;
        dirs << QFileInfo(pluginPath()).absolutePath() + "/haline";

        for (const QString &dir : dirs) {
            const QString dll = dir + "/haline_engine.dll";
            if (!QFileInfo::exists(dll)) continue;

            const QString models = findModels(dir);
            if (models.isEmpty()) {
                halineToolLog("engine: models not found near " + dir);
                continue;
            }
            HMODULE mod = LoadLibraryExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(dll).utf16()),
                                         nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!mod) {
                halineToolLog(QString("engine: LoadLibrary failed (%1) %2").arg(GetLastError()).arg(dll));
                continue;
            }
            auto load = reinterpret_cast<LoadFn>(GetProcAddress(mod, "haline_load"));
            auto err = reinterpret_cast<ErrFn>(GetProcAddress(mod, "haline_last_error"));
            auto process = reinterpret_cast<ProcessFn>(GetProcAddress(mod, "haline_process"));
            if (!load || !err || !process) {
                halineToolLog("engine: missing exports in " + dll);
                continue;
            }
            const QByteArray cleaner = QDir::toNativeSeparators(models + "/cleaner_ts.pt").toUtf8();
            const QByteArray classifier = QDir::toNativeSeparators(models + "/classifier_ts.pt").toUtf8();
            if (load(cleaner.constData(), classifier.constData()) != 0) {
                halineToolLog(QString("engine: haline_load failed: ") + err());
                continue;
            }
            m_process = process;
            halineToolLog("engine: loaded " + dll + " models " + models);
            return;
        }
        halineToolLog("engine: unavailable, strokes stay freehand");
    }

    static QString findModels(const QString &engineDir)
    {
        for (const QString &cand : QStringList{engineDir + "/models", engineDir + "/../../models"}) {
            if (QFileInfo::exists(cand + "/cleaner_ts.pt") && QFileInfo::exists(cand + "/classifier_ts.pt"))
                return QDir(cand).absolutePath();
        }
        return QString();
    }

    static QString pluginPath()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&halineToolLog), &self);
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
        return QDir::fromNativeSeparators(QString::fromWCharArray(buf, int(n)));
    }

    ProcessFn m_process {nullptr};
};

QVector<uchar> rasterize(const QVector<QPointF> &pts)
{
    QRectF box;
    for (const QPointF &p : pts) box |= QRectF(p, QSizeF(0.001, 0.001));
    const qreal margin = kRaster * 0.1;
    const qreal scale = (kRaster - 2 * margin) / std::max({box.width(), box.height(), 1.0});
    const QPointF offset = QPointF(kRaster, kRaster) / 2 - box.center() * scale;

    QImage img(kRaster, kRaster, QImage::Format_Grayscale8);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::black, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF poly;
        for (int i = 0; i < pts.size(); ++i) {
            const QPointF a = pts[std::max(0, i - 1)], b = pts[i], c = pts[std::min(int(pts.size()) - 1, i + 1)];
            poly << ((a + 2 * b + c) / 4) * scale + offset;
        }
        p.drawPolyline(poly);
    }
    QVector<uchar> out(kRaster * kRaster);
    for (int y = 0; y < kRaster; ++y)
        memcpy(out.data() + y * kRaster, img.constScanLine(y), kRaster);
    return out;
}

QPointF centroid(const QVector<QPointF> &pts)
{
    QPointF c;
    for (const QPointF &p : pts) c += p;
    return c / pts.size();
}

qreal distToLine(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF d = b - a;
    const qreal len = std::hypot(d.x(), d.y());
    if (len < 1e-9) return std::hypot(p.x() - a.x(), p.y() - a.y());
    return std::abs(d.x() * (a.y() - p.y()) - d.y() * (a.x() - p.x())) / len;
}

QVector<QPointF> fitLine(const QVector<QPointF> &pts)
{
    const QPointF c = centroid(pts);
    qreal sxx = 0, syy = 0, sxy = 0;
    for (const QPointF &p : pts) {
        const QPointF d = p - c;
        sxx += d.x() * d.x(); syy += d.y() * d.y(); sxy += d.x() * d.y();
    }
    const qreal angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
    const QPointF dir(std::cos(angle), std::sin(angle));
    qreal lo = 1e18, hi = -1e18;
    for (const QPointF &p : pts) {
        const qreal t = QPointF::dotProduct(p - c, dir);
        lo = std::min(lo, t); hi = std::max(hi, t);
    }
    return {c + dir * lo, c + dir * hi};
}

QVector<QPointF> fitRect(const QVector<QPointF> &pts)
{
    // Minimum-area bounding box over orientations; hand-drawn squares make PCA unstable.
    qreal bestArea = 1e300;
    QVector<QPointF> best;
    for (int deg = 0; deg < 90; ++deg) {
        const qreal a = qDegreesToRadians(qreal(deg));
        const QPointF u(std::cos(a), std::sin(a)), v(-std::sin(a), std::cos(a));
        qreal u0 = 1e18, u1 = -1e18, v0 = 1e18, v1 = -1e18;
        for (const QPointF &p : pts) {
            const qreal pu = QPointF::dotProduct(p, u), pv = QPointF::dotProduct(p, v);
            u0 = std::min(u0, pu); u1 = std::max(u1, pu);
            v0 = std::min(v0, pv); v1 = std::max(v1, pv);
        }
        const qreal area = (u1 - u0) * (v1 - v0);
        if (area < bestArea) {
            bestArea = area;
            best = {u * u0 + v * v0, u * u1 + v * v0, u * u1 + v * v1, u * u0 + v * v1};
        }
    }
    return best;
}

QVector<QPointF> fitTriangle(const QVector<QPointF> &pts)
{
    QPointF a = pts.first(), b = pts.first();
    qreal farthest = -1;
    for (int i = 0; i < pts.size(); ++i)
        for (int j = i + 1; j < pts.size(); ++j) {
            const QPointF d = pts[i] - pts[j];
            const qreal dd = QPointF::dotProduct(d, d);
            if (dd > farthest) { farthest = dd; a = pts[i]; b = pts[j]; }
        }
    QPointF c = pts.first();
    qreal best = -1;
    for (const QPointF &p : pts) {
        const qreal d = distToLine(p, a, b);
        if (d > best) { best = d; c = p; }
    }
    return {a, b, c};
}

void rdp(const QVector<QPointF> &pts, int lo, int hi, qreal eps, std::vector<bool> &keep)
{
    if (hi <= lo + 1) return;
    qreal maxD = -1;
    int idx = lo;
    for (int i = lo + 1; i < hi; ++i) {
        const qreal d = distToLine(pts[i], pts[lo], pts[hi]);
        if (d > maxD) { maxD = d; idx = i; }
    }
    if (maxD > eps) {
        keep[idx] = true;
        rdp(pts, lo, idx, eps, keep);
        rdp(pts, idx, hi, eps, keep);
    }
}

QVector<QPointF> fitPolyline(const QVector<QPointF> &pts, qreal diag)
{
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    rdp(pts, 0, pts.size() - 1, std::max(2.0, diag * 0.04), keep);
    QVector<QPointF> out;
    for (int i = 0; i < pts.size(); ++i)
        if (keep[i]) out << pts[i];
    return out;
}

QVector<QPointF> resample(const QVector<QPointF> &pts, qreal spacing)
{
    QVector<QPointF> out{pts.first()};
    qreal carry = 0;
    for (int i = 1; i < pts.size(); ++i) {
        QPointF a = pts[i - 1];
        const QPointF b = pts[i];
        qreal seg = std::hypot(b.x() - a.x(), b.y() - a.y());
        while (carry + seg >= spacing) {
            const qreal t = (spacing - carry) / seg;
            a = a + (b - a) * t;
            out << a;
            seg = std::hypot(b.x() - a.x(), b.y() - a.y());
            carry = 0;
        }
        carry += seg;
    }
    if (out.last() != pts.last()) out << pts.last();
    return out;
}

QVector<QPointF> smooth(QVector<QPointF> pts, int passes)
{
    for (int k = 0; k < passes; ++k) {
        QVector<QPointF> next = pts;
        for (int i = 2; i + 2 < pts.size(); ++i)
            next[i] = (pts[i - 2] + 2 * pts[i - 1] + 3 * pts[i] + 2 * pts[i + 1] + pts[i + 2]) / 9;
        pts = next;
    }
    return pts;
}

QPainterPath fitCurve(const QVector<QPointF> &pts, qreal diag)
{
    // Smoothed Catmull-Rom spline: keeps waves/S-shapes that a single Bezier would flatten.
    const QVector<QPointF> p = smooth(resample(pts, std::max(2.0, diag / 40)), 4);
    QPainterPath path(p.first());
    for (int i = 0; i + 1 < p.size(); ++i) {
        const QPointF p0 = p[std::max(0, i - 1)], p1 = p[i], p2 = p[i + 1];
        const QPointF p3 = p[std::min(int(p.size()) - 1, i + 2)];
        path.cubicTo(p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2);
    }
    return path;
}

int rdpCount(const QVector<QPointF> &pts, qreal eps)
{
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    rdp(pts, 0, pts.size() - 1, eps, keep);
    return int(std::count(keep.begin(), keep.end(), true));
}

// Open strokes: decide line / polyline / curve from the geometry itself, since the
// classifier rarely emits POLYLINE and confuses zig-zags with curves or triangles.
int classifyOpen(const QVector<QPointF> &raw, qreal diag)
{
    const QVector<QPointF> pts = smooth(resample(raw, std::max(1.5, diag / 120)), 1);
    const int loose = rdpCount(pts, diag * 0.04);
    if (loose <= 2) return SHAPE_LINE;
    const int tight = rdpCount(pts, std::max(2.5, diag * 0.015));
    // Straight segments survive a tighter tolerance; curved spans keep splitting.
    if (loose <= 12 && tight <= loose + 2) return SHAPE_POLYLINE;
    return SHAPE_CURVE;
}

} // namespace

KisToolHaline::KisToolHaline(KoCanvasBase *canvas)
    : KisToolShape(canvas, KisCursor::load("tool_freehand_cursor.xpm", 2, 2))
{
    setObjectName("tool_haline");
    Engine::instance();
}

KisToolHaline::~KisToolHaline()
{
}

void KisToolHaline::beginPrimaryAction(KoPointerEvent *event)
{
    if (!nodeEditable() || nodePaintAbility() != KisToolPaint::PAINT) {
        event->ignore();
        return;
    }
    setMode(KisTool::PAINT_MODE);
    beginShape();
    m_drawing = true;
    m_points.clear();
    m_points.append(convertToPixelCoord(event));
}

void KisToolHaline::continuePrimaryAction(KoPointerEvent *event)
{
    if (!m_drawing) return;
    m_points.append(convertToPixelCoord(event));
    updatePreview();
}

void KisToolHaline::endPrimaryAction(KoPointerEvent *event)
{
    Q_UNUSED(event);
    if (!m_drawing) return;
    m_drawing = false;
    setMode(KisTool::HOVER_MODE);
    if (m_points.size() > 1) correctStroke(m_points);
    m_points.clear();
    updatePreview();
    endShape();
}

void KisToolHaline::paint(QPainter &gc, const KoViewConverter &converter)
{
    if (m_drawing && m_points.size() > 1) {
        QPainterPath path(pixelToView(m_points.first()));
        for (int i = 1; i < m_points.size(); ++i) path.lineTo(pixelToView(m_points[i]));
        paintToolOutline(&gc, path);
    }
    KisToolShape::paint(gc, converter);
}

void KisToolHaline::updatePreview()
{
    if (image()) updateCanvasPixelRect(image()->bounds());
}

void KisToolHaline::correctStroke(const QVector<QPointF> &points)
{
    KisFigurePaintingToolHelper helper(kundo2_i18n("Haline AI Stroke"), image(), currentNode(),
                                       canvas()->resourceManager(), strokeStyle(), fillStyle(),
                                       fillTransform());

    Engine &engine = Engine::instance();
    if (!engine.ready() || points.size() < 3) {
        helper.paintPolyline(points);
        return;
    }

    float probs[6] = {};
    int cls = engine.classify(rasterize(points), kRaster, kRaster, probs);
    QRectF box;
    for (const QPointF &p : points) box |= QRectF(p, QSizeF(0.001, 0.001));
    const qreal diag = std::hypot(box.width(), box.height());

    halineToolLog(QString("stroke: %1 pts -> %2 (p=%3)")
                      .arg(points.size())
                      .arg(cls >= 0 && cls < 6 ? kShapeNames[cls] : "?")
                      .arg(cls >= 0 && cls < 6 ? probs[cls] : 0.f, 0, 'f', 3));

    const QPointF gap = points.first() - points.last();
    const bool open = std::hypot(gap.x(), gap.y()) > diag * 0.2;
    if (open && cls != SHAPE_CIRCLE) {
        cls = classifyOpen(points, diag);
        halineToolLog(QString("  open stroke -> %1").arg(kShapeNames[cls]));
    } else if (open && probs[SHAPE_CIRCLE] < 0.6f) {
        cls = classifyOpen(points, diag);
    }

    switch (cls) {
    case SHAPE_LINE: helper.paintPolyline(fitLine(points)); break;
    case SHAPE_RECT: helper.paintPolygon(fitRect(points)); break;
    case SHAPE_CIRCLE: helper.paintEllipse(box); break;
    case SHAPE_CURVE: helper.paintPainterPath(fitCurve(points, diag)); break;
    case SHAPE_TRIANGLE: helper.paintPolygon(fitTriangle(points)); break;
    case SHAPE_POLYLINE: helper.paintPolyline(fitPolyline(smooth(resample(points, std::max(1.5, diag / 120)), 1), diag)); break;
    default: helper.paintPolyline(points); break;
    }
}
